// phase_layer_cells.hlsl — 凝縮相と気体が混ざるセルの 2 つのエネルギーのセルの段(shaders/common/phase_layer.hlsli の PlStepLoneCell:
// 層の上の面 → 層の温度の反応 → 混ざり方の変わり目)を GPU で走らせる compute シェーダー(T-1211・ADR-0486)。
// 1 スレッド = 1 セル(1 セルだけのブロック。面の流れは無い)。区間の初めに「直前の刻みに変わった」とみなす(reaction_cells.hlsl と同じ)。
// 表(物質・規則・索引・速度)はベイクしたものを t0〜t3 に、セルは u0・Ec(mJ)は u1。最初の区間だけ初めの値(t4・t5)から読む。
// CPU リファレンス(同じ PlAdvanceLoneCell を C++ で)とビット一致することを tests/gpu_phase_layer_test.cpp が確かめる
// (PlEvenCondensedEnergy の 64bit の差と比べ・PlGasEnergyAfter の床の割り算を RTX で確かめる。CLAUDE.md §4・T-1078)。
#include "common/phase_layer.hlsli"

RWStructuredBuffer<RxCell> g_cells : register(u0);
RWStructuredBuffer<int64_t> g_condensedEnergies : register(u1);
StructuredBuffer<RxSpecies> g_species : register(t0);
StructuredBuffer<RxRule> g_rules : register(t1);
StructuredBuffer<uint32_t> g_ruleIndex : register(t2);
ByteAddressBuffer g_rates : register(t3);
StructuredBuffer<RxCell> g_initialCells : register(t4);
StructuredBuffer<int64_t> g_initialCondensedEnergies : register(t5);

cbuffer RootConstants : register(b0) {
    uint32_t g_cellCount;
    uint32_t g_tickCount;
    uint32_t g_tickBeginLow;
    uint32_t g_tickBeginHigh;  // 区間の初めの刻み(1 以上)
    uint32_t g_seedLow;
    uint32_t g_seedHigh;
    uint32_t g_fromInitial;  // 1 なら初めの値(t4・t5)から読む
};

// reaction.hlsli の Table の約束(表の読み方)
struct GpuReactionTable {
    uint32_t unused;

    RxSpecies Species(uint32_t id) { return g_species[id]; }

    RxRule Rule(uint32_t id) { return g_rules[id]; }
    RxRule RateRule(uint32_t id) { return g_rules[id]; }

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
    int64_t condensedEnergy;
    if (g_fromInitial != 0) {
        cell = g_initialCells[index];
        condensedEnergy = g_initialCondensedEnergies[index];
    } else {
        cell = g_cells[index];
        condensedEnergy = g_condensedEnergies[index];
    }

    const uint64_t tickBegin = FX_U64(g_tickBeginHigh, g_tickBeginLow);
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const PlLoneCell lone = PlAdvanceLoneCell(table, PlMakeLoneCell(cell, condensedEnergy, tickBegin - 1), seed,
                                              tickBegin, g_tickCount, index);

    g_cells[index] = lone.cell;
    g_condensedEnergies[index] = lone.condensedEnergy;
}
