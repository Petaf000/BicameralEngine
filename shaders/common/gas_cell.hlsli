// gas_cell.hlsli — 気体の 1 セルの熱力学の部品(導く値・移す塊のエネルギー・勾配の制限・乱数の丸め)を、1 レベルの箱の段
// (shaders/sim/gas_step.hlsl。T-0185)と多重解像度の木の段(shaders/sim/gas_tree_step.hlsl。T-0209)で共有する。
// CPU リファレンスは engine/src/sim/gas_reference.cpp の同じ名前の関数(gas_internal.h)で、式はそれを写したもの。
// include する前に、設定(GasGpuParameters)を引く GAS_P を #define しておく。浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_GAS_CELL_HLSLI
#define BICAMERAL_GAS_CELL_HLSLI

#include "common/gas_gpu.hlsli"

// --- 物理の定数(gas_reference.cpp と同じ)---
static const uint64_t PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 = 285682760;
static const uint64_t MICRO = 1000000;
static const uint64_t ONE_Q32 = FX_U64(1u, 0u);
static const uint64_t MAX_U64 = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);
static const uint64_t MAX_CELL_AMOUNT = FX_U64(0u, 0x80000000u);
static const uint32_t GAS_ROUNDING_PURPOSE = 0x47610001u;
static const uint32_t KINETIC_SHIFT = 41;                    // ½ m v² = P² × 2^-41 ÷ m(µJ)
static const uint64_t MAX_MOMENTUM = FX_U64(0x100000u, 0u);  // 2^52

uint64_t MulShiftU64(uint64_t a, uint64_t b, uint32_t shift) {
    return FxShiftRightU128(FxMulU64Full(a, b), shift);
}

int64_t MinS64(int64_t a, int64_t b) {
    return a < b ? a : b;
}

// --- セルから導く値(gas_reference.cpp の DeriveRaw・DeriveGasCell)---

uint64_t TotalAmount(GasGpuCell cell) {
    uint64_t total = 0;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
        total += cell.amounts[s];
    }

    return total;
}

uint64_t HeatCapacity(GasGpuCell cell) {
    uint64_t capacity = 0;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s)
        capacity += cell.amounts[s] * (uint64_t)GAS_P.heatCapacity[s];

    return capacity;
}

int64_t ChemicalEnergy(GasGpuCell cell) {
    int64_t chemical = 0;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s)
        chemical += (int64_t)cell.amounts[s] * GAS_P.formationEnergy[s];

    return chemical;
}

uint64_t CellMass(GasGpuCell cell) {
    uint64_t mass = 0;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s)
        mass += MulShiftU64(cell.amounts[s], GAS_P.massPerAmount[s], 32);

    return mass;
}

// 運動エネルギー(µJ。gas_reference.cpp の KineticEnergy)。等温は 0
int64_t KineticEnergy(GasGpuCell cell, uint64_t mass) {
    if (GAS_P.thermodynamics != GAS_GPU_THERMODYNAMICS_ADIABATIC || mass == 0)
        return 0;

    uint64_t scaledSquare = 0;
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        const uint64_t magnitude = FxAbsU64(cell.momentum[axis]);
        FX_ASSERT(magnitude < MAX_MOMENTUM);
        scaledSquare += FxShiftRightU128(FxMulU64Full(magnitude, magnitude), KINETIC_SHIFT);
    }

    return (int64_t)(scaledSquare / mass);
}

uint32_t CellTemperature(GasGpuCell cell, int64_t kineticMicroJoule) {
    const int64_t heat = cell.energy * 1000 - ChemicalEnergy(cell) - kineticMicroJoule;
    const uint64_t capacity = HeatCapacity(cell);
    if (heat <= 0 || capacity == 0)
        return 0;

    const uint64_t temperature = FxDivU128By64(FxMulU64Full((uint64_t)heat, MICRO), capacity).quotient;
    FX_ASSERT(temperature <= 0xFFFFFFFFu);

    return (uint32_t)temperature;
}

GasGpuDerived DeriveCellFromReference(GasGpuCell cell, int64_t referencePressure) {
    const uint64_t amount = TotalAmount(cell);
    FX_ASSERT(amount < MAX_CELL_AMOUNT);

    GasGpuDerived derived;
    derived.mass = CellMass(cell);
    derived.inverseMass = derived.mass == 0 ? 0 : MAX_U64 / derived.mass;
    derived.inverseAmount = amount == 0 ? 0 : MAX_U64 / amount;
    derived.temperature = CellTemperature(cell, KineticEnergy(cell, derived.mass));
    derived.unused = 0;

    // --- 本物の圧力 p = n × T × R ÷ V ---
    const FxU128 amountTemperature = FxMulU64Full(amount, derived.temperature);
    FX_ASSERT(amountTemperature.hi == 0);
    derived.pressure = (int64_t)MulShiftU64(amountTemperature.lo, PRESSURE_PER_AMOUNT_TEMPERATURE_Q32, 32);

    // --- p̃ = α(p − p_基準)。頭打ちの温度より熱いセルは α × T_頭 ÷ T(T-0269)---
    const int64_t deviation = derived.pressure - referencePressure;
    int64_t scaled = FxMulShiftS64(deviation, (int64_t)GAS_P.soundScale, 32);
    if (derived.temperature > GAS_P.soundTemperature)
        scaled = FxDivS64(FxMulShiftS64(scaled, (int64_t)GAS_P.soundTemperature, 0), (int64_t)derived.temperature);

    derived.scaledPressure = scaled;

    return derived;
}

int64_t LimitedSlope(int64_t a, int64_t b) {
    if (a == 0 || b == 0 || (a > 0) != (b > 0))
        return 0;

    const int64_t sign = a > 0 ? 1 : -1;
    const int64_t smaller = MinS64(a * sign, b * sign);
    if (GAS_P.reconstruction == GAS_GPU_RECONSTRUCTION_MINMOD)
        return smaller * sign;

    const int64_t central = ((a * sign) + (b * sign)) / 2;
    return MinS64(2 * smaller, central) * sign;
}

uint64_t RoundAmount(uint64_t amountQ32, uint64_t hash) {
    const uint64_t whole = amountQ32 >> 32;
    if (GAS_P.stochasticRounding == 0)
        return whole;

    return whole + ((hash >> 32) < (amountQ32 & FX_LOW32_MASK) ? 1 : 0);
}

// --- 移す塊のエネルギー(gas_reference.cpp の DivideNearest・MovedEnergy)---

int64_t DivideNearest(int64_t a, uint64_t b) {
    FxU128 product;
    product.hi = 0;
    product.lo = FxAbsU64(a);
    const FxDivResult result = FxDivU128By64(product, b);
    const uint64_t quotient = result.quotient + (result.remainder * 2 >= b ? 1 : 0);

    return FxApplySign(quotient, a < 0);
}

// µJ(丸める前)。断熱: + 押し出す仕事 n R T + 運動エネルギーの分け前(T-0234)
int64_t MovedEnergy(GasGpuCell source, GasGpuDerived sourceDerived, GasGpuCell moved, uint64_t fraction) {
    const uint64_t sourceCapacity = HeatCapacity(source);
    const int64_t movedChemical = ChemicalEnergy(moved);
    if (sourceCapacity == 0)
        return movedChemical;

    const int64_t sourceKinetic = KineticEnergy(source, sourceDerived.mass);
    const int64_t sourceHeat = source.energy * 1000 - ChemicalEnergy(source) - sourceKinetic;
    const FxU128 numerator = FxMulU64Full(FxAbsU64(sourceHeat), HeatCapacity(moved));
    const FxDivResult share = FxDivU128By64(numerator, sourceCapacity);
    const uint64_t heat = share.quotient + (share.remainder * 2 >= sourceCapacity ? 1 : 0);
    const int64_t movedHeat = FxApplySign(heat, sourceHeat < 0);
    int64_t energy = movedHeat + movedChemical;
    if (GAS_P.thermodynamics != GAS_GPU_THERMODYNAMICS_ADIABATIC)
        return energy;

    const FxU128 amountTemperature = FxMulU64Full(TotalAmount(moved), sourceDerived.temperature);
    FX_ASSERT(amountTemperature.hi == 0);
    energy += (int64_t)MulShiftU64(amountTemperature.lo, PRESSURE_PER_AMOUNT_TEMPERATURE_Q32, 35);
    energy += (int64_t)MulShiftU64((uint64_t)sourceKinetic, fraction, 32);

    return energy;
}

#endif  // BICAMERAL_GAS_CELL_HLSLI
