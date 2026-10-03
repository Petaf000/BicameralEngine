// multires_bindings.hlsli — 多重解像度のシェーダー(multires_step.hlsl の Compute と multires_graph.hlsl の Work Graph)が共有する
// バッファの結び方・ルート定数・表の読み方。ルート署名は engine/src/sim/gpu_multires.cpp の ROOT_LAYOUT と同じ順。
// RW のバッファは globallycoherent: Work Graph の再帰で、親のレベルのグループが書いたセルを、次のレベルのグループが読むため
// (書いた側は出力の前に Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC))。
#ifndef BICAMERAL_MULTIRES_BINDINGS_HLSLI
#define BICAMERAL_MULTIRES_BINDINGS_HLSLI

#include "common/multires_tree.hlsli"

// --- 結び付け ---
// u0 ブロックの見出し [枠] / u1 セル [枠 × 512] / u2 端数 [端数の枠 × 512] / u3 数える欄(MR_COUNTER_*)/
// u4・u5 外のバッファ(覗き窓が使う。shaders/sim/multires_peek.hlsl で宣言する。T-0096)/
// u6 世界の枠の空きのスタック / u7 端数の枠の空きのスタック / u8 世界の帳簿 / u9 索引 / u10 要求 / u11 要求の途中の値 /
// u12 取り合いの印 [世界の枠] / u13 Work Graph の GPU の入力(MR_GRAPH_INPUT_*。T-0018)/ t0〜t3 反応の表(物質・規則・索引・速度)
globallycoherent RWStructuredBuffer<MrBlock> g_blocks : register(u0);
globallycoherent RWStructuredBuffer<RxCell> g_cells : register(u1);
globallycoherent RWStructuredBuffer<MrFraction> g_fractions : register(u2);
globallycoherent RWStructuredBuffer<uint32_t> g_counters : register(u3);
globallycoherent RWStructuredBuffer<uint32_t> g_freeBlocks : register(u6);
globallycoherent RWStructuredBuffer<uint32_t> g_freeFractions : register(u7);
RWStructuredBuffer<uint64_t> g_ledger : register(u8);
globallycoherent RWStructuredBuffer<uint32_t> g_index : register(u9);
RWStructuredBuffer<MrRequest> g_requests : register(u10);
globallycoherent RWStructuredBuffer<MrRequestState> g_states : register(u11);
globallycoherent RWStructuredBuffer<uint32_t> g_claims : register(u12);
RWByteAddressBuffer g_graphInput : register(u13);
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
    uint32_t g_graphInputLow;  // u13 の GPU の番地(DispatchGraph の GPU の入力の見出しに書く)
    uint32_t g_graphInputHigh;
    uint32_t g_graphEntries;  // 下位 16bit = RefineNode、上位 16bit = CoarsenRequestNode の入口の番号
};

// --- Work Graph の GPU の入力(u13。見出しは D3D12_NODE_GPU_INPUT そのもの。gpu_multires.cpp の static_assert)---
static const uint32_t MR_GRAPH_INPUT_REFINE_HEADER = 0;    // バイト
static const uint32_t MR_GRAPH_INPUT_COARSEN_HEADER = 32;  // バイト
static const uint32_t MR_GRAPH_INPUT_REFINE_RECORDS = 64;  // MrRefineRecord × (MR_MAX_REQUESTS + 1)
static const uint32_t MR_REFINE_RECORD_BYTES = 24;
static const uint32_t MR_GRAPH_INPUT_COARSEN_RECORDS = MR_GRAPH_INPUT_REFINE_RECORDS +
                                                       MR_REFINE_RECORD_BYTES *
                                                           (MR_MAX_REQUESTS + 1);  // uint32 × (MR_MAX_REQUESTS + 1)
static const uint32_t MR_GRAPH_INPUT_BYTES = MR_GRAPH_INPUT_COARSEN_RECORDS + 4 * (MR_MAX_REQUESTS + 1);

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

uint32_t CellAddress(uint32_t slot, uint32_t index) {
    return slot * MR_BLOCK_CELLS + index;
}

// 端数(枠が無ければ空)
MrFraction LoadFraction(uint32_t fractionSlot, uint32_t index) {
    if (fractionSlot == MR_NO_FRACTION)
        return MrMakeEmptyFraction();

    return g_fractions[CellAddress(fractionSlot, index)];
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
        InterlockedCompareExchange(g_index[(home + probe) & IndexMask()], MR_INDEX_EMPTY, slot, previous);
        if (previous == MR_INDEX_EMPTY)
            return;
    }

    InterlockedAdd(g_counters[MR_COUNTER_INDEX_FULL], 1u);
}

void IndexRemove(uint32_t slot) {
    const MrBlock block = g_blocks[slot];
    const uint32_t home = MrIndexHome(block.level, block.originX, block.originY, block.originZ, g_indexEntries);
    for (uint32_t probe = 0; probe < g_indexEntries; ++probe) {
        const uint32_t address = (home + probe) & IndexMask();
        const uint32_t entry = g_index[address];
        if (entry == MR_INDEX_EMPTY)
            return;

        if (entry != slot)
            continue;

        g_index[address] = MR_INDEX_TOMBSTONE;
        InterlockedAdd(g_counters[MR_COUNTER_TOMBSTONES], 1u);
        return;
    }
}

uint32_t LookupBlock(int32_t level, int64_t originX, int64_t originY, int64_t originZ) {
    const uint32_t home = MrIndexHome(level, originX, originY, originZ, g_indexEntries);
    for (uint32_t probe = 0; probe < g_indexEntries; ++probe) {
        const uint32_t entry = g_index[(home + probe) & IndexMask()];
        if (entry == MR_INDEX_EMPTY)
            return MR_NO_BLOCK;

        if (entry != MR_INDEX_TOMBSTONE && MrBlockHasKey(g_blocks[entry], level, originX, originY, originZ))
            return entry;
    }

    return MR_NO_BLOCK;
}

// --- 割り当てた枠(空きのスタックの上から)---

uint32_t PoppedBlock(MrRequestState state, uint32_t i) {
    return g_freeBlocks[state.blockBase - 1 - i];
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

#endif  // BICAMERAL_MULTIRES_BINDINGS_HLSLI
