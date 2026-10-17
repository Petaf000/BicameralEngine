// gas_cell.hlsli — 気体の芯(shaders/common/gas_flow.hlsli。CPU と同じ関数)を、GPU の欄の並び(gas_gpu.hlsli の GasGpuCell・GasGpuDerived・
// 設定の頭 GasGpuParameters・成分の値 GasGpuSpecies)から呼ぶ薄い包み(T-0719)。1 レベルの箱の段(shaders/sim/gas_step.hlsl)・
// 仮の世界の気体の段(shaders/sim/probe_gas_tick.hlsl)・多重解像度の木の段(shaders/sim/gas_tree_step.hlsl。凍結)が include する。
// 式はここに書かない(gas_flow.hlsli だけ)。CPU リファレンスは engine/src/sim/gas_reference.cpp で、同じ gas_flow.hlsli を呼ぶ。
// include する前に、設定の頭(GasGpuParameters)を引く GAS_P と、成分 s の値(GasGpuSpecies)を引く GAS_SPECIES(s) を #define しておく。
// 成分の数に上限は無い(T-0598): セルの成分の和は GasGpuTotals に 1 つずつ足して作る。浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_GAS_CELL_HLSLI
#define BICAMERAL_GAS_CELL_HLSLI

#include "common/gas_flow.hlsli"
#include "common/gas_gpu.hlsli"
#include "common/units.hlsli"

// --- 段が使う定数(gas_flow.hlsli の値の別名)---
static const uint64_t MICRO = GF_MICRO;
static const uint64_t ONE_Q32 = GF_ONE_Q32;
static const uint64_t MAX_U64 = GF_MAX_U64;
static const uint32_t GAS_ROUNDING_PURPOSE = GF_ROUNDING_PURPOSE;

uint64_t MulShiftU64(uint64_t a, uint64_t b, uint32_t shift) {
    return GfMulShiftU64(a, b, shift);
}

// 見立ての値(設定の頭から)
GfParams GasFlowParams() {
    const GasGpuParameters parameters = GAS_P;
    GfParams params;
    params.centralFlow = parameters.centralFlow;
    params.pressureFlow = parameters.pressureFlow;
    params.pressureImpulse = parameters.pressureImpulse;
    params.gravityImpulse = parameters.gravityImpulse;
    params.hydrostatic = parameters.hydrostatic;
    params.gravityWork = parameters.gravityWork;
    params.soundScale = parameters.soundScale;
    params.soundTemperature = parameters.soundTemperature;
    params.gravity = parameters.gravity;
    params.reconstruction = parameters.reconstruction;
    params.stochasticRounding = parameters.stochasticRounding;
    params.adiabatic = parameters.thermodynamics == GAS_GPU_THERMODYNAMICS_ADIABATIC ? 1u : 0u;
    params.openness = parameters.openness;

    return params;
}

// --- セルの成分の和(T-0598。gas_flow.hlsli の GfTotals)---
typedef GfTotals GasGpuTotals;

GasGpuTotals GasZeroTotals() {
    return GfZeroTotals();
}

void GasAddSpecies(inout GasGpuTotals totals, uint32_t s, uint64_t amount) {
    const GasGpuSpecies species = GAS_SPECIES(s);
    totals = GfAddSpecies(totals, amount, (uint64_t)species.heatCapacity, species.formationEnergy,
                          species.massPerAmount);
}

// --- セルから導く値(gas_flow.hlsli の GfKineticEnergy・GfDerive)---

int64_t KineticEnergy(GasGpuCell cell, uint64_t mass) {
    return GfKineticEnergy(GasFlowParams(), cell.momentum[0], cell.momentum[1], cell.momentum[2], mass);
}

GasGpuDerived ToGasGpuDerived(GfDerived flow) {
    GasGpuDerived derived;
    derived.mass = flow.mass;
    derived.inverseMass = flow.inverseMass;
    derived.inverseAmount = flow.inverseAmount;
    derived.temperature = flow.temperature;
    derived.unused = 0;
    derived.pressure = flow.pressure;
    derived.scaledPressure = flow.scaledPressure;

    return derived;
}

// 気体の空き open(Q16)のセルの導く値(全部なら GAS_GPU_OPEN_FULL)。referencePressure = 層の基準の本物の圧力
GasGpuDerived DeriveOpenCellFromReference(GasGpuCell cell, GasGpuTotals totals, int64_t referencePressure,
                                          uint32_t open) {
    return ToGasGpuDerived(GfDerive(GasFlowParams(), cell.energy, cell.momentum[0], cell.momentum[1], cell.momentum[2],
                                    totals, referencePressure, open));
}

GasGpuDerived DeriveCellFromReference(GasGpuCell cell, GasGpuTotals totals, int64_t referencePressure) {
    return DeriveOpenCellFromReference(cell, totals, referencePressure, GAS_GPU_OPEN_FULL);
}

// --- 移す成分と塊(gas_flow.hlsli)---

int64_t LimitedSlope(int64_t a, int64_t b) {
    return GfLimitedSlope(GasFlowParams(), a, b);
}

uint64_t RoundAmount(uint64_t amountQ32, uint64_t hash) {
    return GfRoundAmount(GasFlowParams(), amountQ32, hash);
}

int64_t DivideNearest(int64_t a, uint64_t b) {
    return GfDivideNearest(a, b);
}

// 移す塊のエネルギー(µJ。丸める前)。sourceTotals・movedTotals は風上の中身と移す塊の成分の和
int64_t MovedEnergy(GasGpuCell source, GasGpuTotals sourceTotals, GasGpuDerived sourceDerived, GasGpuTotals movedTotals,
                    uint64_t fraction) {
    return GfMovedEnergy(GasFlowParams(), source.energy, sourceTotals, KineticEnergy(source, sourceDerived.mass),
                         sourceDerived.temperature, movedTotals, fraction);
}

// 出ていく割合の合計の換算(T-0646・T-0647・ADR-0286)。totals = 風上の成分の和
uint64_t EffectiveOutflow(GasGpuTotals totals, uint64_t outflow) {
    return GfEffectiveOutflow(GasFlowParams(), totals, outflow);
}

#endif  // BICAMERAL_GAS_CELL_HLSLI
