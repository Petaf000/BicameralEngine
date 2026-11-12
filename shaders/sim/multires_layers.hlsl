// multires_layers.hlsl — 層を合わせる段(T-1221・T-1240。ADR-0481 の 5・20 §10.5): 刻みの段(反応)がセルの成分の量を書いた後、
// 層の頁を持つ頁のセルごとに、成分ごとの層を物質で結んで 層 = min(前の層, 今の量) に付け替える(CPU の StorePageCell の付け替えと同じ規則)。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)で、外のバッファ u4 に層の置き場を結ぶ(並びは
// common/multires_layers.hlsli・道具は sim/multires_layer_access.hlsli)。1 グループ = 層の頁 1 つの 64 セル、1 スレッド = セル 1 つ。
// 空いている層の頁(持ち主が無い)は何もしない(頁を返す段が層の頁も返す。T-1240)。
// 溢れを使う世界の変種(MR_WIDE_CELLS。multires_layers_wide。T-1240 の 7 の 5)は、インラインに無い物質の量を頁の今の面の溢れから引く
// (刻みの段 WideDeferred・WideRetry・WideBig が面を入れ替えた後に回る)。生まれた粒を層に足す段は溢れを使う世界ではまだ使わない(T-1340)。
// セルは読むだけ。CPU リファレンスは sim::StoreDenseCellImage → nest_detail::StorePageCell(試験 gpu_multires_layers)。
//
// 層を使う世界で反応が粒を生む表(T-1323。ADR-0489 の 3 (B)・CPU の nest_detail::GrowGrainLayers): 刻みの前に SnapshotGrainCells が
// 刻む前のセル(u5。u1 と同じ並び)を写し、刻みの後に ReconcileGrownLayers が枠ごとに前と比べて、増えた粒の物質の分を層に足す
// (層の頁の無い頁で粒が生まれたら層の頁を取る。尽きたら計器で数えて舞う粒のまま)。増えないセルは ReconcileLayers と同じ min。
// 影の引き戻しの後は今の ReconcileLayers のまま(粒は生まれない)。
#include "sim/multires_layer_access.hlsli"

[numthreads(MR_LAYER_GROUP_THREADS, 1, 1)] void ReconcileLayers(uint3 groupId : SV_GroupID,
                                                                uint3 threadId : SV_GroupThreadID) {
    const uint32_t capacity = LayerCapacity();
    const uint32_t pageCount = LayerPageCount();
    const uint32_t layerPage = groupId.x / MR_LAYER_GROUPS_PER_PAGE;
    if (layerPage >= capacity)
        return;

    const uint32_t page = g_layers[MrLayerOwnerAddress(pageCount, layerPage)];
    if (page == MR_LAYER_NO_PAGE)
        return;

    const uint32_t index = ((groupId.x % MR_LAYER_GROUPS_PER_PAGE) * MR_LAYER_GROUP_THREADS) + threadId.x;
    const RxCell cell = g_cells[PageCellAddress(page, index)];
#ifdef MR_WIDE_CELLS
    const LayerTail tail = PageLayerTail(page,
                                         index);  // インラインに無い物質の量は頁の今の面の溢れから(T-1240 の 7 の 5)
#else
    const LayerTail tail = NoLayerTail();
#endif

    // --- 組ごとに付け替える(変わった組だけ書く)---
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t address = MrLayerEntryAddress(pageCount, capacity, layerPage, index, k);
        const uint32_t low = g_layers[address + 1];
        const uint32_t high = g_layers[address + 2];
        if ((low | high) == 0)
            continue;

        const uint64_t layer = ((uint64_t)high << 32) | low;
        const uint64_t next = LayerAfterWriteAt(cell, tail, g_layers[address], layer);
        const uint32_t nextLow = (uint32_t)next;
        const uint32_t nextHigh = (uint32_t)(next >> 32);
        if (nextLow == low && nextHigh == high)
            continue;

        g_layers[address + 1] = nextLow;
        g_layers[address + 2] = nextHigh;
    }
}

// --- 層を使う世界で生まれた粒を層に足す(T-1323)---------------------------------------------------------------------------

// 刻む前のセル(u1 と同じ並び = [一様の値 × 枠][頁 × 512])。枠の所は、刻む前に一様なら一様の値、頁を持っていれば印(padding = 1)
RWStructuredBuffer<RxCell> g_grainBefore : register(u5);

static const uint32_t GRAIN_THREADS = 64;  // 1 グループ = 枠 1 つ
static const uint32_t GRAIN_CELLS_PER_THREAD = MR_BLOCK_CELLS / GRAIN_THREADS;
static const uint32_t GRAIN_PAGED_MARK = 1;  // 刻む前に頁を持っていた枠(g_grainBefore[slot].padding)

groupshared uint32_t gs_grainGrew;
groupshared uint32_t gs_grainLayerPage;

// 刻む前のセルを写す: 枠ごとに、一様なら一様の値・頁を持てば印と頁のセル全部
[numthreads(GRAIN_THREADS, 1, 1)] void SnapshotGrainCells(uint3 groupId : SV_GroupID,
                                                          uint3 threadId : SV_GroupThreadID) {
    const uint32_t slot = groupId.x;
    if (slot >= g_blockCount)
        return;

    const MrBlock block = g_blocks[slot];
    const bool wasUniform = MrIsUniform(block);
    if (threadId.x == 0) {
        RxCell value = g_cells[slot];
        value.padding = wasUniform ? 0u : GRAIN_PAGED_MARK;
        g_grainBefore[slot] = value;
    }

    if (wasUniform)
        return;

    for (uint32_t k = 0; k < GRAIN_CELLS_PER_THREAD; ++k) {
        const uint32_t address = PageCellAddress(block.page, threadId.x + (GRAIN_THREADS * k));
        g_grainBefore[address] = g_cells[address];
    }
}

// 64bit の差 a − b(a ≥ b)と和 a + b を 32bit の 2 つで(シミュの HLSL で 64bit の引き算と比べを並べない約束。CLAUDE.md §4・T-1078)
uint64_t GrainSubtract(uint64_t a, uint64_t b) {
    const uint32_t aLow = (uint32_t)a;
    const uint32_t bLow = (uint32_t)b;
    const uint32_t borrow = aLow < bLow ? 1u : 0u;
    const uint32_t high = (uint32_t)(a >> 32) - (uint32_t)(b >> 32) - borrow;

    return FX_U64(high, aLow - bLow);
}

uint64_t GrainAdd(uint64_t a, uint64_t b) {
    const uint32_t low = (uint32_t)a + (uint32_t)b;
    const uint32_t carry = low < (uint32_t)a ? 1u : 0u;
    const uint32_t high = (uint32_t)(a >> 32) + (uint32_t)(b >> 32) + carry;

    return FX_U64(high, low);
}

bool IsGrainSpecies(uint32_t species) {
    return (g_species[species].flags & RX_SPECIES_GRAIN) != 0u;
}

// セル before の物質 species の量(無ければ 0)
uint64_t GrainAmountOf(RxCell cell, uint32_t species) {
    uint64_t amount = 0;
    FX_UNROLL
    for (uint32_t j = 0; j < RX_MAX_CELL_SPECIES; ++j) {
        if (j < cell.speciesCount && cell.species[j] == species)
            amount = cell.amounts[j];
    }

    return amount;
}

// 前のセルより粒の物質が増えたか
bool GrainGrew(RxCell before, RxCell cell) {
    bool grew = false;
    FX_UNROLL
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i >= cell.speciesCount || !IsGrainSpecies(cell.species[i]))
            continue;

        grew = grew || MrLayerLess(GrainAmountOf(before, cell.species[i]), cell.amounts[i]);
    }

    return grew;
}

// 粒が増えたセルの層を書き直す(CPU の GrowGrainLayers): 組 k = 今のセルの k 番目の物質。増えた粒は 前の層 + 増えた分、
// ほかは min(前の層, 今の量)。前の層は前のセルと結んで読む(CPU の LoadWidePageCellLayers)。全部求めてから書く
void WriteGrownLayers(uint32_t base, RxCell before, RxCell cell) {
    uint64_t bound[RX_MAX_CELL_SPECIES];
    FX_UNROLL
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        bound[i] = 0;
        if (i >= cell.speciesCount)
            continue;

        const uint32_t species = cell.species[i];
        const uint64_t amountBefore = GrainAmountOf(before, species);
        const uint64_t layerBefore = BoundLayerOf(base, species, before);
        if (IsGrainSpecies(species) && MrLayerLess(amountBefore, cell.amounts[i]))
            bound[i] = GrainAdd(layerBefore, GrainSubtract(cell.amounts[i], amountBefore));
        else
            bound[i] = MrLayerLess(cell.amounts[i], layerBefore) ? cell.amounts[i] : layerBefore;
    }

    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t address = base + (k * MR_LAYER_ENTRY_WORDS);
        g_layers[address] = k < cell.speciesCount ? cell.species[k] : 0u;
        g_layers[address + 1] = (uint32_t)bound[k];
        g_layers[address + 2] = (uint32_t)(bound[k] >> 32);
    }
}

// 粒の増えないセル: ReconcileLayers と同じ min(変わった組だけ書く)
void BindCellLayers(uint32_t base, RxCell cell) {
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t address = base + (k * MR_LAYER_ENTRY_WORDS);
        const uint32_t low = g_layers[address + 1];
        const uint32_t high = g_layers[address + 2];
        if ((low | high) == 0)
            continue;

        const uint64_t next = MrLayerAfterWrite(cell, g_layers[address], FX_U64(high, low));
        if ((uint32_t)next == low && (uint32_t)(next >> 32) == high)
            continue;

        g_layers[address + 1] = (uint32_t)next;
        g_layers[address + 2] = (uint32_t)(next >> 32);
    }
}

// 層の頁を空きのスタックから 1 つ取ってセルの頁 page に結ぶ(スレッド 0 だけ)。尽きたら数えて MR_LAYER_NO_PAGE。
// 取り合いは下げてから確かめ、尽きていたら戻す(下げた値が 0 か上限より大きい = 尽きた)。取る順は層の頁の番号に効くだけで結果には効かない
uint32_t TakeLayerPage(uint32_t page) {
    const uint32_t pageCount = LayerPageCount();
    const uint32_t capacity = LayerCapacity();
    const uint32_t freeCount = MrLayerFreeCountAddress(pageCount, capacity);
    uint32_t previous = 0;
    InterlockedAdd(g_layers[freeCount], 0xFFFFFFFFu, previous);
    if (previous == 0 || previous > capacity) {
        InterlockedAdd(g_layers[freeCount], 1u);
        CountLayerShortage(1);

        return MR_LAYER_NO_PAGE;
    }

    const uint32_t layerPage = g_layers[MrLayerFreeAddress(pageCount, capacity, previous - 1)];
    AttachLayerPage(page, layerPage);

    return layerPage;
}

// 刻む前のセル index(枠の写し beforeValue が印なら頁の写し、でなければ一様の値。広げた頁は一様の値で埋めてから刻む)
RxCell GrainBeforeCell(MrBlock block, RxCell beforeValue, uint32_t address, uint32_t index) {
    if (beforeValue.padding == GRAIN_PAGED_MARK)
        return g_grainBefore[address];

    return MrUniformCell(block, beforeValue, index);
}

// 刻みの後: 枠ごとに刻む前のセルと比べ、増えた粒の物質を層に足す。1 グループ = 枠 1 つ(64 スレッド × 8 セル)
[numthreads(GRAIN_THREADS, 1, 1)] void ReconcileGrownLayers(uint3 groupId : SV_GroupID,
                                                            uint3 threadId : SV_GroupThreadID) {
    const uint32_t slot = groupId.x;
    if (slot >= g_blockCount)
        return;

    const MrBlock block = g_blocks[slot];
    if (MrIsUniform(block))
        return;  // 一様のまま = 反応で変わっていない(変わる一様な枠は頁に広げて刻む)

    // --- 刻む前のセル(刻む前に一様なら一様の値。広げた頁は一様の値で埋めてから刻む)と粒の増え ---
    const RxCell beforeValue = g_grainBefore[slot];
    if (threadId.x == 0)
        gs_grainGrew = 0;

    GroupMemoryBarrierWithGroupSync();

    uint32_t grewMask = 0;
    for (uint32_t k = 0; k < GRAIN_CELLS_PER_THREAD; ++k) {
        const uint32_t index = threadId.x + (GRAIN_THREADS * k);
        const uint32_t address = PageCellAddress(block.page, index);
        const RxCell before = GrainBeforeCell(block, beforeValue, address, index);
        if (GrainGrew(before, g_cells[address]))
            grewMask |= 1u << k;
    }

    if (grewMask != 0)
        InterlockedOr(gs_grainGrew, 1u);

    GroupMemoryBarrierWithGroupSync();

    // --- 層の頁(無くて粒が生まれたら取る。取った頁の全部のセルを書く)---
    const uint32_t existing = LayerPageOf(block.page);
    if (threadId.x == 0)
        gs_grainLayerPage = existing == MR_LAYER_NO_PAGE && gs_grainGrew != 0 ? TakeLayerPage(block.page) : existing;

    GroupMemoryBarrierWithGroupSync();

    const uint32_t layerPage = gs_grainLayerPage;
    if (layerPage == MR_LAYER_NO_PAGE)
        return;

    const bool fresh = existing == MR_LAYER_NO_PAGE;
    for (uint32_t k = 0; k < GRAIN_CELLS_PER_THREAD; ++k) {
        const uint32_t index = threadId.x + (GRAIN_THREADS * k);
        const uint32_t address = PageCellAddress(block.page, index);
        const uint32_t base = MrLayerEntryAddress(LayerPageCount(), LayerCapacity(), layerPage, index, 0);
        if (fresh)
            ClearLayerWords(base);  // 取った頁の前の中身を読まない

        const RxCell cell = g_cells[address];
        if ((grewMask & (1u << k)) == 0) {
            BindCellLayers(base, cell);
            continue;
        }

        const RxCell before = GrainBeforeCell(block, beforeValue, address, index);
        WriteGrownLayers(base, before, cell);
    }
}
