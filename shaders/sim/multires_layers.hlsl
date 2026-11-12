// multires_layers.hlsl — 層を合わせる段(T-1221・T-1240。ADR-0481 の 5・20 §10.5): 刻みの段(反応)がセルの成分の量を書いた後、
// 層の頁を持つ頁のセルごとに、成分ごとの層を物質で結んで 層 = min(前の層, 今の量) に付け替える(CPU の StorePageCell の付け替えと同じ規則)。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)で、外のバッファ u4 に層の置き場を結ぶ(並びは
// common/multires_layers.hlsli・道具は sim/multires_layer_access.hlsli)。1 グループ = 層の頁 1 つの 64 セル、1 スレッド = セル 1 つ。
// 空いている層の頁(持ち主が無い)は何もしない(頁を返す段が層の頁も返す。T-1240)。
// 溢れを使う世界の変種(MR_WIDE_CELLS。multires_layers_wide。T-1240 の 7 の 5)は、インラインに無い物質の量を頁の今の面の溢れから引く
// (刻みの段 WideDeferred・WideRetry・WideBig が面を入れ替えた後に回る)。
// セルは読むだけ。CPU リファレンスは sim::StoreDenseCellImage → nest_detail::StorePageCell(試験 gpu_multires_layers)。
//
// 層を使う世界で反応が粒を生む表(T-1323。ADR-0489 の 3 (B)・CPU の nest_detail::GrowGrainLayers): 刻みの前に SnapshotGrainCells が
// 刻む前のセル(u5。u1 と同じ並び)を写し、刻みの後に ReconcileGrownLayers が枠ごとに前と比べて、増えた粒の物質の分を層に足す
// (層の頁の無い頁で粒が生まれたら層の頁を取る。尽きたら計器で数えて舞う粒のまま)。増えないセルは ReconcileLayers と同じ min。
// 影の引き戻しの後は今の ReconcileLayers のまま(粒は生まれない)。
// 溢れを使う世界の変種(multires_layers_grain_*_wide。T-1340): 写しは u5 を生のバッファにして、セルに続けて溢れの領域
// (u6 の g_overflowBase から。頁の見出し・塊の置き場・一様の値の見出し)を同じ並びのまま写す。刻む前の溢れは、写しの中を
// 今の溢れと同じ道(見出しの面 → 塊の表 → 塊)でたどって読む(刻みの段は同じ刻みの中で同じ頁を 2 回書くことがあり、
// 頁のもう片方の面は刻む前の溢れとは限らない)。層の組は 1 セル 8 つなので、粒が生まれて 0 でない層を持つ物質が 9 種以上に
// なるセルは断って数え(CountLayerShortage)、舞う粒のまま(min で合わせる。仮【エンジン】D-689 と同じ形・T-1340)。
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

// 刻む前のセル(u1 と同じ並び = [一様の値 × 枠][頁 × 512])。枠の所は、刻む前に一様なら一様の値、頁を持っていれば印(padding = 1)。
// 溢れを使う変種は生のバッファで、セルの後ろ(g_external2 バイト目から)に溢れの領域の語(g_external3 語)を続ける(gpu_multires.cpp)
#ifdef MR_WIDE_CELLS
RWByteAddressBuffer g_grainBefore : register(u5);
#else
RWStructuredBuffer<RxCell> g_grainBefore : register(u5);
#endif

static const uint32_t GRAIN_THREADS = 64;  // 1 グループ = 枠 1 つ
static const uint32_t GRAIN_CELLS_PER_THREAD = MR_BLOCK_CELLS / GRAIN_THREADS;
static const uint32_t GRAIN_PAGED_MARK = 1;  // 刻む前に頁を持っていた枠(g_grainBefore[slot].padding)

// 写しのセル address を読む・書く
#ifdef MR_WIDE_CELLS
static const uint32_t GRAIN_CELL_BYTES = 112;  // sizeof(RxCell)(gpu_multires.cpp の static_assert)

RxCell LoadGrainBefore(uint32_t address) {
    return g_grainBefore.Load<RxCell>(address * GRAIN_CELL_BYTES);
}

void StoreGrainBefore(uint32_t address, RxCell cell) {
    g_grainBefore.Store<RxCell>(address * GRAIN_CELL_BYTES, cell);
}
#else
RxCell LoadGrainBefore(uint32_t address) {
    return g_grainBefore[address];
}

void StoreGrainBefore(uint32_t address, RxCell cell) {
    g_grainBefore[address] = cell;
}
#endif

#ifdef MR_WIDE_CELLS
// --- 刻む前の溢れ(写しの溢れの領域。語の番地は u6 と同じ = g_overflowBase からの差で写しを引く)---
static const uint32_t GRAIN_TAIL_WORDS_PER_THREAD = 16;  // 溢れの領域を写すグループ 1 つ = 64 × 16 語

uint32_t GrainTailGroups() {
    const uint32_t groupWords = GRAIN_THREADS * GRAIN_TAIL_WORDS_PER_THREAD;

    return (g_external3 + groupWords - 1) / groupWords;
}

// 溢れの領域の語 word(u6 の番地)の刻む前の値
uint32_t BeforeWord(uint32_t word) {
    return g_grainBefore.Load(g_external2 + ((word - g_overflowBase) * 4));
}

// 刻む前の面 side の論理の語 word の番地(WideWord と同じ道を写しで。見出し・置き場の番地は塊の数だけで決まる)
uint32_t BeforeWideWord(uint32_t area, uint32_t side, uint32_t word) {
    const uint32_t table = MrWideTableAt(WideAreaHeader(area), side);
    const uint32_t offset = MrWideSideOffset(word);
    const uint32_t j = offset / MR_WIDE_CHUNK_WORDS;
    const uint32_t entryWord = j == 0 ? table + 1
                                      : MrWideChunkWord(WidePoolWord(), BeforeWord(table + 1)) +
                                            MrWideTableEntryInChunk(j);

    return MrWideChunkWord(WidePoolWord(), BeforeWord(entryWord)) + (offset % MR_WIDE_CHUNK_WORDS);
}

// 頁(か一様の値の領域)area のセル index の刻む前の溢れの在り処(面は写しの見出しの面)
LayerTail BeforeLayerTail(uint32_t area, uint32_t index) {
    LayerTail tail;
    tail.area = area;
    tail.side = BeforeWord(WideHeaderAt(area, MR_WIDE_HEADER_SIDE));
    tail.index = index;

    return tail;
}

// 刻む前の溢れ tail の物質 species の量(無ければ 0。LayerTailAmountOf と同じ道を写しで)
uint64_t BeforeTailAmountOf(LayerTail tail, uint32_t species) {
    const uint32_t table = MrWideTableAt(WideAreaHeader(tail.area), tail.side);
    if (BeforeWord(table) == 0)
        return 0;  // 塊を持たない面は溢れが無い

    uint64_t amount = 0;
    const uint32_t begin = BeforeWord(BeforeWideWord(tail.area, tail.side, tail.index));
    const uint32_t end = BeforeWord(BeforeWideWord(tail.area, tail.side, tail.index + 1));
    for (uint32_t entry = begin; entry < end; ++entry) {
        const uint32_t word = MrWideEntryWord(entry);
        if (BeforeWord(BeforeWideWord(tail.area, tail.side, word)) == species)
            amount = FX_U64(BeforeWord(BeforeWideWord(tail.area, tail.side, word + 2)),
                            BeforeWord(BeforeWideWord(tail.area, tail.side, word + 1)));
    }

    return amount;
}

// 溢れの領域を写す(グループ group。g_overflowBase からの g_external3 語を写しのセルの後ろへ)
void SnapshotGrainTail(uint32_t group, uint32_t thread) {
    const uint32_t first = group * GRAIN_THREADS * GRAIN_TAIL_WORDS_PER_THREAD;
    for (uint32_t k = 0; k < GRAIN_TAIL_WORDS_PER_THREAD; ++k) {
        const uint32_t word = first + (k * GRAIN_THREADS) + thread;
        if (word < g_external3)
            g_grainBefore.Store(g_external2 + (word * 4), g_treeWords[g_overflowBase + word]);
    }
}
#endif

groupshared uint32_t gs_grainGrew;
groupshared uint32_t gs_grainLayerPage;

// 刻む前のセルを写す: 枠ごとに、一様なら一様の値・頁を持てば印と頁のセル全部
[numthreads(GRAIN_THREADS, 1, 1)] void SnapshotGrainCells(uint3 groupId : SV_GroupID,
                                                          uint3 threadId : SV_GroupThreadID) {
    const uint32_t slot = groupId.x;
#ifdef MR_WIDE_CELLS
    if (slot >= g_blockCount) {
        if (slot - g_blockCount < GrainTailGroups())
            SnapshotGrainTail(slot - g_blockCount, threadId.x);  // 枠の後ろのグループは溢れの領域(T-1340)

        return;
    }
#else
    if (slot >= g_blockCount)
        return;
#endif

    const MrBlock block = g_blocks[slot];
    const bool wasUniform = MrIsUniform(block);
    if (threadId.x == 0) {
        RxCell value = g_cells[slot];
        value.padding = wasUniform ? 0u : GRAIN_PAGED_MARK;
        StoreGrainBefore(slot, value);
    }

    if (wasUniform)
        return;

    for (uint32_t k = 0; k < GRAIN_CELLS_PER_THREAD; ++k) {
        const uint32_t address = PageCellAddress(block.page, threadId.x + (GRAIN_THREADS * k));
        StoreGrainBefore(address, g_cells[address]);
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

// 粒の増えないセル: ReconcileLayers と同じ min(変わった組だけ書く。溢れを使う変種は溢れの成分の量まで)
void BindCellLayers(uint32_t base, RxCell cell, LayerTail tail) {
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t address = base + (k * MR_LAYER_ENTRY_WORDS);
        const uint32_t low = g_layers[address + 1];
        const uint32_t high = g_layers[address + 2];
        if ((low | high) == 0)
            continue;

        const uint64_t next = LayerAfterWriteAt(cell, tail, g_layers[address], FX_U64(high, low));
        if ((uint32_t)next == low && (uint32_t)(next >> 32) == high)
            continue;

        g_layers[address + 1] = (uint32_t)next;
        g_layers[address + 2] = (uint32_t)(next >> 32);
    }
}

// --- 溢れの成分まで比べる・詰める(溢れを使う変種。T-1340)---------------------------------------------------------------
#ifdef MR_WIDE_CELLS
// 今のセル cell の溢れ(tail)を読むか(インラインが埋まっていて、溢れの在り処がある)
bool GrainHasTail(RxCell cell, LayerTail tail) {
    return cell.speciesCount == RX_MAX_CELL_SPECIES && tail.area != MR_NO_PAGE;
}

// 今の溢れ tail の成分 entry の物質・量
uint32_t TailSpeciesAt(LayerTail tail, uint32_t entry) {
    return g_treeWords[WideWord(tail.area, tail.side, MrWideEntryWord(entry))];
}

uint64_t TailAmountAt(LayerTail tail, uint32_t entry) {
    const uint32_t word = MrWideEntryWord(entry);

    return FX_U64(g_treeWords[WideWord(tail.area, tail.side, word + 2)],
                  g_treeWords[WideWord(tail.area, tail.side, word + 1)]);
}

// 刻む前のセル before(溢れは beforeTail = 写し)の物質 species の量(無ければ 0)
uint64_t GrainAmountBefore(RxCell before, LayerTail beforeTail, uint32_t species) {
    uint64_t amount = 0;
    bool found = false;
    FX_UNROLL
    for (uint32_t j = 0; j < RX_MAX_CELL_SPECIES; ++j) {
        if (j < before.speciesCount && before.species[j] == species) {
            amount = before.amounts[j];
            found = true;
        }
    }

    if (!found && GrainHasTail(before, beforeTail))
        amount = BeforeTailAmountOf(beforeTail, species);

    return amount;
}

// 前のセルより粒の物質が増えたか(溢れの成分まで)
bool GrainGrewAt(RxCell before, LayerTail beforeTail, RxCell cell, LayerTail tail) {
    bool grew = false;
    FX_UNROLL
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i >= cell.speciesCount || !IsGrainSpecies(cell.species[i]))
            continue;

        grew = grew || MrLayerLess(GrainAmountBefore(before, beforeTail, cell.species[i]), cell.amounts[i]);
    }

    const uint2 range = GrainHasTail(cell, tail) ? WideRange(tail.area, tail.side, tail.index) : uint2(0, 0);
    for (uint32_t entry = range.x; entry < range.y; ++entry) {
        const uint32_t species = TailSpeciesAt(tail, entry);
        if (IsGrainSpecies(species))
            grew = grew || MrLayerLess(GrainAmountBefore(before, beforeTail, species), TailAmountAt(tail, entry));
    }

    return grew;
}

// 書き直す前の層の組(8 組を読んでおく。書きながら前の層を読まないように)
struct GrainPrevLayers {
    uint32_t species[RX_MAX_CELL_SPECIES];
    uint64_t layers[RX_MAX_CELL_SPECIES];
};

GrainPrevLayers ReadPrevLayers(uint32_t base) {
    GrainPrevLayers prev;
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t address = base + (k * MR_LAYER_ENTRY_WORDS);
        prev.species[k] = g_layers[address];
        prev.layers[k] = FX_U64(g_layers[address + 2], g_layers[address + 1]);
    }

    return prev;
}

// 物質 species の生まれた粒を足した層(CPU の GrowGrainLayers の 1 成分): 前の層は前の組を前の量と結ぶ(min)。
// 増えた粒は 前の層 + 増えた分、ほかは min(前の層, 今の量)
uint64_t GrownLayerOf(GrainPrevLayers prev, RxCell before, LayerTail beforeTail, uint32_t species, uint64_t amount) {
    uint64_t raw = 0;
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const bool nonzero = ((uint32_t)prev.layers[k] | (uint32_t)(prev.layers[k] >> 32)) != 0;
        if (prev.species[k] == species && nonzero)
            raw = prev.layers[k];
    }

    const uint64_t amountBefore = GrainAmountBefore(before, beforeTail, species);
    const uint64_t layerBefore = MrLayerLess(amountBefore, raw) ? amountBefore : raw;
    if (IsGrainSpecies(species) && MrLayerLess(amountBefore, amount))
        return GrainAdd(layerBefore, GrainSubtract(amount, amountBefore));

    return MrLayerLess(amount, layerBefore) ? amount : layerBefore;
}

bool GrainLayerNonzero(uint64_t layer) {
    return ((uint32_t)layer | (uint32_t)(layer >> 32)) != 0;
}

void WriteLayerEntry(uint32_t base, uint32_t k, uint32_t species, uint64_t layer) {
    const uint32_t address = base + (k * MR_LAYER_ENTRY_WORDS);
    g_layers[address] = species;
    g_layers[address + 1] = (uint32_t)layer;
    g_layers[address + 2] = (uint32_t)(layer >> 32);
}

// 粒が増えたセルの層を書き直す(インライン + 溢れの成分ごとに GrownLayerOf)。0 でない層を成分の順に 8 組へ前から詰める
// (層は物質で結ぶ)。0 でない層が 9 種以上なら書かずに偽(呼ぶ側が断って数え、舞う粒のまま)
bool WriteGrownLayersAt(uint32_t base, RxCell before, LayerTail beforeTail, RxCell cell, LayerTail tail) {
    const GrainPrevLayers prev = ReadPrevLayers(base);
    const uint2 range = GrainHasTail(cell, tail) ? WideRange(tail.area, tail.side, tail.index) : uint2(0, 0);

    // --- 0 でない層の数(組に入るか)---
    uint32_t count = 0;
    FX_UNROLL
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < cell.speciesCount &&
            GrainLayerNonzero(GrownLayerOf(prev, before, beforeTail, cell.species[i], cell.amounts[i])))
            ++count;
    }

    for (uint32_t entry = range.x; entry < range.y; ++entry) {
        if (GrainLayerNonzero(
                GrownLayerOf(prev, before, beforeTail, TailSpeciesAt(tail, entry), TailAmountAt(tail, entry))))
            ++count;
    }

    if (count > RX_MAX_CELL_SPECIES)
        return false;

    // --- 書く(残りの組は 0)---
    uint32_t k = 0;
    FX_UNROLL
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i >= cell.speciesCount)
            continue;

        const uint64_t layer = GrownLayerOf(prev, before, beforeTail, cell.species[i], cell.amounts[i]);
        if (!GrainLayerNonzero(layer))
            continue;

        WriteLayerEntry(base, k, cell.species[i], layer);
        ++k;
    }

    for (uint32_t entry = range.x; entry < range.y; ++entry) {
        const uint32_t species = TailSpeciesAt(tail, entry);
        const uint64_t layer = GrownLayerOf(prev, before, beforeTail, species, TailAmountAt(tail, entry));
        if (!GrainLayerNonzero(layer))
            continue;

        WriteLayerEntry(base, k, species, layer);
        ++k;
    }

    for (; k < RX_MAX_CELL_SPECIES; ++k)
        WriteLayerEntry(base, k, 0, 0);

    return true;
}

// 刻む前の溢れの在り処(頁を持っていた枠は写しの頁の溢れ、一様だった枠は写しの一様の値の溢れ。観察の枠は持たない)
LayerTail GrainBeforeTail(uint32_t slot, RxCell beforeValue, uint32_t page, uint32_t index) {
    if (beforeValue.padding == GRAIN_PAGED_MARK)
        return BeforeLayerTail(page, index);

    if (slot >= g_worldBlocks)
        return NoLayerTail();

    return BeforeLayerTail(WideUniformArea(slot), 0);
}

LayerTail GrainAfterTail(uint32_t page, uint32_t index) {
    return PageLayerTail(page, index);
}
#else
// 溢れを使わない変種は溢れを読まない(前と同じ式)
bool GrainGrewAt(RxCell before, LayerTail beforeTail, RxCell cell, LayerTail tail) {
    return GrainGrew(before, cell);
}

bool WriteGrownLayersAt(uint32_t base, RxCell before, LayerTail beforeTail, RxCell cell, LayerTail tail) {
    WriteGrownLayers(base, before, cell);

    return true;
}

LayerTail GrainBeforeTail(uint32_t slot, RxCell beforeValue, uint32_t page, uint32_t index) {
    return NoLayerTail();
}

LayerTail GrainAfterTail(uint32_t page, uint32_t index) {
    return NoLayerTail();
}
#endif

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

// 一様だった枠のセル index(覆われていれば空。MrUniformCell と同じ答え)。子は 8 つから選ぶ(溢れを使う変種の MR_CHILD_OF は
// 見出しの写しの children を動的な添字で読み、この段では局所の配列になった。T-1266・T-1340)
RxCell GrainUniformCell(MrBlock block, RxCell value, uint32_t index) {
    const uint32_t octant = MrOctantOfCell(index);
    uint32_t child = MR_NO_BLOCK;
    FX_UNROLL
    for (uint32_t k = 0; k < 8; ++k)
        child = k == octant ? block.children[k] : child;

    if (block.kind == MR_BLOCK_REAL && child != MR_NO_BLOCK)
        return RxMakeEmptyCell(0);

    return value;
}

// 刻む前のセル index(枠の写し beforeValue が印なら頁の写し、でなければ一様の値。広げた頁は一様の値で埋めてから刻む)
RxCell GrainBeforeCell(MrBlock block, RxCell beforeValue, uint32_t address, uint32_t index) {
    if (beforeValue.padding == GRAIN_PAGED_MARK)
        return LoadGrainBefore(address);

    return GrainUniformCell(block, beforeValue, index);
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
    const RxCell beforeValue = LoadGrainBefore(slot);
    if (threadId.x == 0)
        gs_grainGrew = 0;

    GroupMemoryBarrierWithGroupSync();

    uint32_t grewMask = 0;
    for (uint32_t k = 0; k < GRAIN_CELLS_PER_THREAD; ++k) {
        const uint32_t index = threadId.x + (GRAIN_THREADS * k);
        const uint32_t address = PageCellAddress(block.page, index);
        const RxCell before = GrainBeforeCell(block, beforeValue, address, index);
        const LayerTail beforeTail = GrainBeforeTail(slot, beforeValue, block.page, index);
        if (GrainGrewAt(before, beforeTail, g_cells[address], GrainAfterTail(block.page, index)))
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
        const LayerTail tail = GrainAfterTail(block.page, index);
        if ((grewMask & (1u << k)) == 0) {
            BindCellLayers(base, cell, tail);
            continue;
        }

        // 0 でない層が 9 種以上になるセルは断って数え、舞う粒のまま(溢れを使う世界だけ。仮【エンジン】T-1340)
        const RxCell before = GrainBeforeCell(block, beforeValue, address, index);
        if (!WriteGrownLayersAt(base, before, GrainBeforeTail(slot, beforeValue, block.page, index), cell, tail)) {
            CountLayerShortage(1);
            BindCellLayers(base, cell, tail);
        }
    }
}
