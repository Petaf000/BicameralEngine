// multires_bindings.hlsli — 多重解像度のシェーダー(multires_step.hlsl の Compute と multires_graph.hlsl の Work Graph)が共有する
// バッファの結び方・ルート定数・表の読み方。ルート署名は engine/src/sim/gpu_multires.cpp の ROOT_LAYOUT と同じ順。
// RW のバッファは globallycoherent: Work Graph の再帰で、親のレベルのグループが書いたセルを、次のレベルのグループが読むため
// (書いた側は出力の前に Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC))。
#ifndef BICAMERAL_MULTIRES_BINDINGS_HLSLI
#define BICAMERAL_MULTIRES_BINDINGS_HLSLI

#include "common/multires.hlsli"

// --- 結び付け ---
// u0 ブロックの見出し [枠] / u1 セル [枠 × 512] / u2 端数 [端数の枠 × 512] / u3 数える欄(MR_COUNTER_*)/
// t0〜t3 反応の表(物質・規則・索引・速度)
globallycoherent RWStructuredBuffer<MrBlock> g_blocks : register(u0);
globallycoherent RWStructuredBuffer<RxCell> g_cells : register(u1);
globallycoherent RWStructuredBuffer<MrFraction> g_fractions : register(u2);
globallycoherent RWStructuredBuffer<uint32_t> g_counters : register(u3);
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

#endif  // BICAMERAL_MULTIRES_BINDINGS_HLSLI
