// multires_bindings.hlsli — 多重解像度のシェーダー(multires_step.hlsl の Compute と multires_graph.hlsl の Work Graph)が共有する
// バッファの結び方・ルート定数・表の読み方。ルート署名は engine/src/sim/gpu_multires.cpp の ROOT_LAYOUT と同じ順。
// RW のバッファは globallycoherent: Work Graph の再帰で、親のレベルのグループが書いたセルを、次のレベルのグループが読むため
// (書いた側は出力の前に Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC))。
#ifndef BICAMERAL_MULTIRES_BINDINGS_HLSLI
#define BICAMERAL_MULTIRES_BINDINGS_HLSLI

#include "common/multires_activity.hlsli"

// --- 結び付け ---
// u0 ブロックの見出し [枠] / u1 セル [一様の値 × 枠][頁 × 512](T-0102)/ u2 端数 [端数の枠 × 512] / u3 数える欄(MR_COUNTER_*)/
// u4・u5 外のバッファ(覗き窓が使う。shaders/sim/multires_peek.hlsl で宣言する。T-0096)/
// u6 木の管理の uint32 の表 [世界の枠の空きのスタック][取り合いの印 × 世界の枠][索引][世界の頁の空きのスタック]
// (ルート署名の語を空けるために 1 本にまとめた。T-0107。番地は TreeClaimAddress・TreeIndexAddress・FreePageAddress)/
// u7 端数の枠の空きのスタック / u8 世界の帳簿 / u9 要求 / u10 要求の途中の値 /
// u11 伝導の作業場(枠ごとの刻みの印 + 頁のセルごとのエネルギーの変化。multires_conduct.hlsli。T-0107)/
// u12 Work Graph の GPU の入力(MR_GRAPH_INPUT_*。T-0018)/
// u13 書き足す活性の一覧(MR_ACTIVITY_*。要求の処理の間はこの刻みの種、刻む間は次の刻みの種。T-0100)/ t0〜t3 反応の表(物質・規則・索引・速度)
globallycoherent RWStructuredBuffer<MrBlock> g_blocks : register(u0);
globallycoherent RWStructuredBuffer<RxCell> g_cells : register(u1);
globallycoherent RWStructuredBuffer<MrFraction> g_fractions : register(u2);
globallycoherent RWStructuredBuffer<uint32_t> g_counters : register(u3);
globallycoherent RWStructuredBuffer<uint32_t> g_treeWords : register(u6);
globallycoherent RWStructuredBuffer<uint32_t> g_freeFractions : register(u7);
RWStructuredBuffer<uint64_t> g_ledger : register(u8);
RWStructuredBuffer<MrRequest> g_requests : register(u9);
globallycoherent RWStructuredBuffer<MrRequestState> g_states : register(u10);
RWByteAddressBuffer g_conduction : register(u11);
RWByteAddressBuffer g_graphInput : register(u12);
RWByteAddressBuffer g_activity : register(u13);
StructuredBuffer<RxSpecies> g_species : register(t0);
StructuredBuffer<RxRule> g_rules : register(t1);
StructuredBuffer<uint32_t> g_ruleIndex : register(t2);
ByteAddressBuffer g_rates : register(t3);

// gpu_multires.cpp の RootConstants と同じ並び(64bit は下位・上位の順)
cbuffer RootConstants : register(b0) {
    uint32_t g_seedLow;
    uint32_t g_seedHigh;
    uint32_t g_tickLow;
    uint32_t g_tickHigh;
    uint32_t g_pointXLow;
    uint32_t g_pointXHigh;
    uint32_t g_pointYLow;
    uint32_t g_pointYHigh;
    uint32_t g_pointZLow;
    uint32_t g_pointZHigh;
    int32_t g_pointLevel;
    uint32_t g_blockCount;
    uint32_t g_external0;  // 外のパイプラインの定数(T-0096)
    uint32_t g_external1;
    uint32_t g_external2;
    uint32_t g_external3;

    // --- 木の管理(T-0018)---
    int32_t g_rootLevel;
    uint32_t g_worldBlocks;
    uint32_t g_indexEntries;  // 2 の冪
    uint32_t g_ledgerColumns;
    uint32_t g_graphInputLow;  // u12 の GPU の番地(DispatchGraph の GPU の入力の見出しに書く)
    uint32_t g_graphInputHigh;
    uint32_t g_graphEntries;  // 下位 16bit = RefineNode、上位 16bit = CoarsenRequestNode の入口の番号

    // --- 刻み(T-0107)---
    uint32_t g_stepFlags;  // MR_STEP_*
};

// g_stepFlags(gpu_multires.cpp の STEP_FLAG_*)
static const uint32_t MR_STEP_CONDUCTION = 1;  // 熱の伝導を入れる(活性の刻むノードは刻まずに伝導の一覧に足す)
static const uint32_t MR_STEP_LISTED = 2;  // 伝導の段は伝導の一覧(MR_GRAPH_INPUT_CONDUCT_*)のブロック。無ければ全部の枠

// --- Work Graph の GPU の入力(u12。見出しは D3D12_NODE_GPU_INPUT そのもの。gpu_multires.cpp の static_assert)---
static const uint32_t MR_GRAPH_INPUT_REFINE_HEADER = 0;    // バイト
static const uint32_t MR_GRAPH_INPUT_COARSEN_HEADER = 32;  // バイト
static const uint32_t MR_GRAPH_INPUT_REFINE_RECORDS = 64;  // MrRefineRecord × (MR_MAX_REQUESTS + 1)
static const uint32_t MR_REFINE_RECORD_BYTES = 24;
static const uint32_t MR_GRAPH_INPUT_COARSEN_RECORDS = MR_GRAPH_INPUT_REFINE_RECORDS +
                                                       MR_REFINE_RECORD_BYTES *
                                                           (MR_MAX_REQUESTS + 1);  // uint32 × (MR_MAX_REQUESTS + 1)
static const uint32_t MR_GRAPH_INPUT_BYTES = MR_GRAPH_INPUT_COARSEN_RECORDS + 4 * (MR_MAX_REQUESTS + 1);

// 頁に広げて刻むブロックの一覧(T-0102。TreeExpand が数とレコードを書き、入口の番号と番地は CPU が写しの時に書く)。
// レコードは uint32 × (世界の枠 + 1)、レコード 0 は空(MR_NO_BLOCK)
static const uint32_t MR_GRAPH_INPUT_EXPAND_HEADER = (MR_GRAPH_INPUT_BYTES + 15) & ~15u;  // バイト
// 伝導の一覧(T-0107。活性の刻みで伝導の段が受け持つブロック。見出しは CPU が写しの時に書き、数は ConductBegin と
// ConductAppend が書く)。レコードは uint32 × (全部の枠 + 1)、レコード 0 は空。並びは決まらない(集合として使う)。
// 見出しは 3 つ(同じレコードを指し、入口だけが違う: 埋める・流れ・足す。Work Graph で伝導の段を投げる時。数は TreeFractions が写す)
static const uint32_t MR_GRAPH_INPUT_CONDUCT_HEADER = MR_GRAPH_INPUT_EXPAND_HEADER +
                                                      32;  // バイト(埋める。数を数える見出し)
static const uint32_t MR_GRAPH_INPUT_CONDUCT_FLOWS_HEADER = MR_GRAPH_INPUT_CONDUCT_HEADER + 32;
static const uint32_t MR_GRAPH_INPUT_CONDUCT_APPLY_HEADER = MR_GRAPH_INPUT_CONDUCT_FLOWS_HEADER + 32;
static const uint32_t MR_GRAPH_INPUT_EXPAND_RECORDS = MR_GRAPH_INPUT_CONDUCT_APPLY_HEADER + 32;  // バイト

uint32_t ConductRecordsOffset() {
    return MR_GRAPH_INPUT_EXPAND_RECORDS + 4 * (g_worldBlocks + 1);
}

// 細かくするノードの入力(手で決めた影の鎖と、要求の鎖の両方)
struct MrRefineRecord {
    uint32_t request;  // 要求の番号(MR_NO_BLOCK なら手で決めた影の鎖。枠は childSlot から順)
    uint32_t parentSlot;
    uint32_t childSlot;   // 影の鎖の子の枠(要求の鎖では使わない)
    uint32_t levelsLeft;  // このレベルを含めて残りの段の数(0 なら何もしない: GPU の入力の空の代わり)
    uint32_t kind;        // MR_BLOCK_REAL か MR_BLOCK_SHADOW
    uint32_t depth;       // 要求の鎖の何段目か(0 から)
};

// reaction.hlsli の Table の約束(表の読み方)
struct GpuReactionTable {
    uint32_t unused;

    RxSpecies Species(uint32_t id) { return g_species[id]; }

    RxRule Rule(uint32_t id) { return g_rules[id]; }

    uint32_t RuleIndex(uint32_t position) { return g_ruleIndex[position]; }

    uint64_t Rate(uint32_t rule, uint32_t kelvin) {
        return g_rates.Load<uint64_t>((rule * RX_RATE_TABLE_KELVINS + kelvin) * 8);
    }
};

GpuReactionTable MakeTable() {
    GpuReactionTable table;
    table.unused = 0;

    return table;
}

// --- セル(T-0102。u1 = [一様の値 × 枠][頁 × 512])---

uint32_t PageCellAddress(uint32_t page, uint32_t index) {
    return g_blockCount + page * MR_BLOCK_CELLS + index;
}

// 枠 slot(見出し block)のセル index。一様なら値か、覆われていれば空(multires_nest.cpp の LoadNestCell)
RxCell LoadBlockCell(MrBlock block, uint32_t slot, uint32_t index) {
    if (MrIsUniform(block))
        return MrUniformCell(block, g_cells[slot], index);

    return g_cells[PageCellAddress(block.page, index)];
}

RxCell LoadCell(uint32_t slot, uint32_t index) {
    return LoadBlockCell(g_blocks[slot], slot, index);
}

// --- 木の管理の uint32 の表(u6。T-0107 でまとめた)---

// 取り合いの印 [世界の枠](世界の枠の空きのスタックの後ろ)
uint32_t TreeClaimAddress(uint32_t slot) {
    return g_worldBlocks + slot;
}

// 索引 [g_indexEntries](取り合いの印の後ろ)
uint32_t TreeIndexAddress(uint32_t entry) {
    return 2 * g_worldBlocks + entry;
}

// 世界の頁の空きのスタック(索引の後ろ)
uint32_t FreePageAddress(uint32_t position) {
    return 2 * g_worldBlocks + g_indexEntries + position;
}

uint32_t FractionAddress(uint32_t fractionSlot, uint32_t index) {
    return fractionSlot * MR_BLOCK_CELLS + index;
}

// 端数(枠が無ければ空)
MrFraction LoadFraction(uint32_t fractionSlot, uint32_t index) {
    if (fractionSlot == MR_NO_FRACTION)
        return MrMakeEmptyFraction();

    return g_fractions[FractionAddress(fractionSlot, index)];
}

// --- 索引(multires_tree.cpp の IndexInsert・IndexRemove・LookupBlock と同じ探査)---

uint32_t IndexMask() {
    return g_indexEntries - 1;
}

void IndexInsert(uint32_t slot) {
    const MrBlock block = g_blocks[slot];
    const uint32_t home = MrIndexHome(block.level, block.originX, block.originY, block.originZ, g_indexEntries);
    for (uint32_t probe = 0; probe < g_indexEntries; ++probe) {
        uint32_t previous;
        InterlockedCompareExchange(g_treeWords[TreeIndexAddress((home + probe) & IndexMask())], MR_INDEX_EMPTY, slot,
                                   previous);
        if (previous == MR_INDEX_EMPTY)
            return;
    }

    InterlockedAdd(g_counters[MR_COUNTER_INDEX_FULL], 1u);
}

void IndexRemove(uint32_t slot) {
    const MrBlock block = g_blocks[slot];
    const uint32_t home = MrIndexHome(block.level, block.originX, block.originY, block.originZ, g_indexEntries);
    for (uint32_t probe = 0; probe < g_indexEntries; ++probe) {
        const uint32_t address = TreeIndexAddress((home + probe) & IndexMask());
        const uint32_t entry = g_treeWords[address];
        if (entry == MR_INDEX_EMPTY)
            return;

        if (entry != slot)
            continue;

        g_treeWords[address] = MR_INDEX_TOMBSTONE;
        InterlockedAdd(g_counters[MR_COUNTER_TOMBSTONES], 1u);
        return;
    }
}

uint32_t LookupBlock(int32_t level, int64_t originX, int64_t originY, int64_t originZ) {
    const uint32_t home = MrIndexHome(level, originX, originY, originZ, g_indexEntries);
    for (uint32_t probe = 0; probe < g_indexEntries; ++probe) {
        const uint32_t entry = g_treeWords[TreeIndexAddress((home + probe) & IndexMask())];
        if (entry == MR_INDEX_EMPTY)
            return MR_NO_BLOCK;

        if (entry != MR_INDEX_TOMBSTONE && MrBlockHasKey(g_blocks[entry], level, originX, originY, originZ))
            return entry;
    }

    return MR_NO_BLOCK;
}

// --- 割り当てた枠(空きのスタックの上から)---

uint32_t PoppedBlock(MrRequestState state, uint32_t i) {
    return g_treeWords[state.blockBase - 1 - i];
}

uint32_t PoppedPage(MrRequestState state, uint32_t i) {
    return g_treeWords[FreePageAddress(state.pageBase - 1 - i)];
}

uint32_t PoppedFraction(MrRequestState state, uint32_t i) {
    if (i >= state.fractionNeed)
        return MR_NO_FRACTION;

    return g_freeFractions[state.fractionBase - 1 - i];
}

// --- 世界の帳簿(64bit の atomic の足し算なので順に依存しない)---

void AddToLedger(int32_t level, uint32_t column, uint32_t lostBits) {
    const uint32_t address = MrLedgerAddress(level, column, g_ledgerColumns);
    if (address == MR_NO_BLOCK) {
        InterlockedAdd(g_counters[MR_COUNTER_LEDGER_OUTSIDE], 1u);
        return;
    }

    InterlockedAdd(g_ledger[address], (uint64_t)lostBits);
}

// --- 頁に広げて刻むブロックの一覧(T-0102)---

uint32_t ExpandRecordCount() {
    return g_graphInput.Load(MR_GRAPH_INPUT_EXPAND_HEADER + 4);
}

uint32_t ExpandRecord(uint32_t record) {
    return g_graphInput.Load(MR_GRAPH_INPUT_EXPAND_RECORDS + 4 * record);
}

// --- 活性(T-0100)---

// 活性の一覧に枠を足す(順は決定的でなくてよい: 集合として使う)。一杯なら落として数える
void AppendActivity(uint32_t slot) {
    uint32_t position;
    g_activity.InterlockedAdd(MR_ACTIVITY_RESERVED, 1u, position);
    if (position >= MrActivityCapacity(g_worldBlocks)) {
        g_activity.InterlockedAdd(MR_ACTIVITY_DROPPED, 1u);
        return;
    }

    g_activity.Store(MR_ACTIVITY_RECORDS + (4 * position), slot);
    g_activity.InterlockedAdd(MR_ACTIVITY_NUM_RECORDS, 1u);
}

// 木を変えたブロックをつつく: 活性の種にし、忙しさの印を「つつかれた」にする(T-0100・T-0101。multires_tree.cpp の PokeBlock)
void PokeBlock(uint32_t slot) {
    g_blocks[slot].busyTick = MR_BUSY_POKED;
    AppendActivity(slot);
}

// この刻みに刻む印を付ける。初めて付けたなら true(1 刻みに 1 回だけ刻む)
bool ScheduleBlock(uint32_t slot, uint32_t mark) {
    uint32_t previous;
    InterlockedExchange(g_blocks[slot].activeTick, mark, previous);

    return previous != mark;
}

// --- 伝導の作業場(u11。T-0107。shaders/sim/multires_conduct.hlsli)---
// [枠ごとの刻みの印 × CONDUCT_MARK_WORDS 語][頁のセルごとのエネルギーの変化(整数部 int64・端数 uint64)]。
// 印は「その刻みの印(MrActivityMark)が書いてあれば立っている」なので、刻みごとに消さなくてよい(最初は全部 0)。
// 変化は ConductFlows が 64bit の atomic で足し、ConductApply が読んで 0 に戻す
static const uint32_t CONDUCT_MARK_WORDS = 8;
static const uint32_t CONDUCT_MARK_FRACTION = 0;  // 粗い側で端数の枠が要る(CPU の MarkConductionWants の wantsFraction)
static const uint32_t CONDUCT_MARK_FROZEN = 1;    // 頁に広げたかったが頁が足りず、この刻みは凍らせた(TreeExpand)
static const uint32_t CONDUCT_MARK_EXPANDED = 2;  // この刻みに頁を配った(TreeExpand)
static const uint32_t CONDUCT_MARK_LISTED = 3;    // 伝導の一覧に入れた(ConductAppend)
static const uint32_t CONDUCT_MARK_GRANTED = 4;   // この刻みに端数の枠を配った(TreeFractions)
static const uint32_t CONDUCT_DELTA_BYTES = 16;

uint32_t CurrentStepMark() {
    return MrActivityMark(FX_U64(g_tickHigh, g_tickLow));
}

uint32_t ConductMarkAddress(uint32_t slot, uint32_t kind) {
    return (slot * CONDUCT_MARK_WORDS + kind) * 4;
}

void SetConductMark(uint32_t slot, uint32_t kind) {
    g_conduction.Store(ConductMarkAddress(slot, kind), CurrentStepMark());
}

bool HasConductMark(uint32_t slot, uint32_t kind) {
    return g_conduction.Load(ConductMarkAddress(slot, kind)) == CurrentStepMark();
}

// 頁 page のセル index の変化の番地(バイト)
uint32_t ConductDeltaAddress(uint32_t page, uint32_t index) {
    return g_blockCount * CONDUCT_MARK_WORDS * 4 + (page * MR_BLOCK_CELLS + index) * CONDUCT_DELTA_BYTES;
}

// 伝導の一覧に枠を足す(この刻みに初めてなら。順は決まらない: 集合として使う)
void ConductAppend(uint32_t slot) {
    uint32_t previous;
    g_conduction.InterlockedExchange(ConductMarkAddress(slot, CONDUCT_MARK_LISTED), CurrentStepMark(), previous);
    if (previous == CurrentStepMark())
        return;

    uint32_t position;
    g_graphInput.InterlockedAdd(MR_GRAPH_INPUT_CONDUCT_HEADER + 4, 1u, position);
    g_graphInput.Store(ConductRecordsOffset() + 4 * position, slot);
}

// multires_activity.hlsli の Tree の約束
struct GpuTree {
    uint32_t unused;

    MrBlock Block(uint32_t slot) { return g_blocks[slot]; }

    uint32_t Lookup(int32_t level, int64_t originX, int64_t originY, int64_t originZ) {
        return LookupBlock(level, originX, originY, originZ);
    }
};

GpuTree MakeTree() {
    GpuTree tree;
    tree.unused = 0;

    return tree;
}

#endif  // BICAMERAL_MULTIRES_BINDINGS_HLSLI
