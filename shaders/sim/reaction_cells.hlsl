// reaction_cells.hlsl — 1 セルの反応の評価(shaders/common/reaction.hlsli)を GPU で走らせる compute シェーダー(T-0014)。
// 1 スレッド = 1 セル。セルどうしは独立(隣へは書かない。02 §3 の 4)なので、スレッドの中で tickCount 刻みをまとめて進める。
// 表(物質・規則・索引・速度)はベイクしたものをそのまま t0〜t3 に、セルは u0。最初の区間だけ初めのセル(t4)から読む。
// CPU リファレンス(sim::EvaluateReactionCell)とビット一致することを tests/gpu_reaction_test.cpp が確かめる。
// 世界(Work Graphs の段)への組み込みは T-0089。
#include "common/reaction.hlsli"

RWStructuredBuffer<RxCell> g_cells : register(u0);
StructuredBuffer<RxSpecies> g_species : register(t0);
StructuredBuffer<RxRule> g_rules : register(t1);
StructuredBuffer<uint32_t> g_ruleIndex : register(t2);
ByteAddressBuffer g_rates : register(t3);
StructuredBuffer<RxCell> g_initialCells : register(t4);

cbuffer RootConstants : register(b0) {
    uint32_t g_cellCount;
    uint32_t g_tickCount;
    uint32_t g_tickBeginLow;
    uint32_t g_tickBeginHigh;
    uint32_t g_seedLow;
    uint32_t g_seedHigh;
    uint32_t g_fromInitial;  // 1 なら初めのセル(t4)から読む
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

[numthreads(64, 1, 1)] void Main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t index = dispatchThreadId.x;
    if (index >= g_cellCount)
        return;

    GpuReactionTable table;
    table.unused = 0;
    // HLSL の ?: は構造体を返せないので if で
    RxCell cell;
    if (g_fromInitial != 0)
        cell = g_initialCells[index];
    else
        cell = g_cells[index];

    const uint64_t tickBegin = FX_U64(g_tickBeginHigh, g_tickBeginLow);
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    for (uint32_t i = 0; i < g_tickCount; ++i)
        cell = RxEvaluateCell(table, cell, seed, tickBegin + i, index);

    g_cells[index] = cell;
}
