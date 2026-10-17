// gas_cell.hlsli — 気体の 1 セルの熱力学の部品(導く値・移す塊のエネルギー・勾配の制限・乱数の丸め)を、1 レベルの箱の段
// (shaders/sim/gas_step.hlsl。T-0185)と多重解像度の木の段(shaders/sim/gas_tree_step.hlsl。T-0209)で共有する。
// CPU リファレンスは engine/src/sim/gas_reference.cpp の同じ名前の関数(gas_internal.h)で、式はそれを写したもの。
// include する前に、設定の頭(GasGpuParameters)を引く GAS_P と、成分 s の値(GasGpuSpecies)を引く GAS_SPECIES(s) を #define しておく。
// 成分の数に上限は無い(T-0598): セルの成分の和は GasGpuTotals に 1 つずつ足して作る。浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_GAS_CELL_HLSLI
#define BICAMERAL_GAS_CELL_HLSLI

#include "common/gas_gpu.hlsli"
#include "common/units.hlsli"

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

// --- セルの成分の和(T-0598。成分の数に上限が無いので、セルの物質量は件の後ろの欄から 1 つずつ足す)---
// 和は整数の足し算なので、足す順に依らず gas_reference.cpp の TotalAmount・HeatCapacity・ChemicalEnergy・CellMass と同じ値(04 R2)
struct GasGpuTotals {
    uint64_t amount;    // µmol
    uint64_t capacity;  // 物質量 × 比熱
    int64_t chemical;   // 物質量 × 生成のエネルギー(µJ)
    uint64_t mass;      // mg
};

GasGpuTotals GasZeroTotals() {
    GasGpuTotals totals;
    totals.amount = 0;
    totals.capacity = 0;
    totals.chemical = 0;
    totals.mass = 0;

    return totals;
}

void GasAddSpecies(inout GasGpuTotals totals, uint32_t s, uint64_t amount) {
    const GasGpuSpecies species = GAS_SPECIES(s);
    totals.amount += amount;
    totals.capacity += amount * (uint64_t)species.heatCapacity;
    totals.chemical += (int64_t)amount * species.formationEnergy;
    totals.mass += MulShiftU64(amount, species.massPerAmount, 32);
}

// --- セルから導く値(gas_reference.cpp の DeriveRaw・DeriveGasCell)---

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

uint32_t CellTemperature(GasGpuCell cell, GasGpuTotals totals, int64_t kineticMicroJoule) {
    const int64_t heat = cell.energy * 1000 - totals.chemical - kineticMicroJoule;
    if (heat <= 0 || totals.capacity == 0)
        return 0;

    const uint64_t temperature = FxDivU128By64(FxMulU64Full((uint64_t)heat, MICRO), totals.capacity).quotient;
    FX_ASSERT(temperature <= 0xFFFFFFFFu);

    return (uint32_t)temperature;
}

GasGpuDerived DeriveCellFromReference(GasGpuCell cell, GasGpuTotals totals, int64_t referencePressure) {
    const uint64_t amount = totals.amount;
    FX_ASSERT(amount < MAX_CELL_AMOUNT);

    GasGpuDerived derived;
    derived.mass = totals.mass;
    derived.inverseMass = derived.mass == 0 ? 0 : MAX_U64 / derived.mass;
    derived.inverseAmount = amount == 0 ? 0 : MAX_U64 / amount;
    derived.temperature = CellTemperature(cell, totals, KineticEnergy(cell, derived.mass));
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

// µJ(丸める前)。断熱: + 押し出す仕事 n R T + 運動エネルギーの分け前(T-0234)。
// sourceTotals・movedTotals は風上の中身と移す塊の成分の和(使うのは物質量・比熱・化学のエネルギー)
int64_t MovedEnergy(GasGpuCell source, GasGpuTotals sourceTotals, GasGpuDerived sourceDerived, GasGpuTotals movedTotals,
                    uint64_t fraction) {
    const uint64_t sourceCapacity = sourceTotals.capacity;
    const int64_t movedChemical = movedTotals.chemical;
    if (sourceCapacity == 0)
        return movedChemical;

    const int64_t sourceKinetic = KineticEnergy(source, sourceDerived.mass);
    const int64_t sourceHeat = source.energy * 1000 - sourceTotals.chemical - sourceKinetic;
    const FxU128 numerator = FxMulU64Full(FxAbsU64(sourceHeat), movedTotals.capacity);
    const FxDivResult share = FxDivU128By64(numerator, sourceCapacity);
    const uint64_t heat = share.quotient + (share.remainder * 2 >= sourceCapacity ? 1 : 0);
    const int64_t movedHeat = FxApplySign(heat, sourceHeat < 0);
    int64_t energy = movedHeat + movedChemical;
    if (GAS_P.thermodynamics != GAS_GPU_THERMODYNAMICS_ADIABATIC)
        return energy;

    const FxU128 amountTemperature = FxMulU64Full(movedTotals.amount, sourceDerived.temperature);
    FX_ASSERT(amountTemperature.hi == 0);
    energy += (int64_t)MulShiftU64(amountTemperature.lo, PRESSURE_PER_AMOUNT_TEMPERATURE_Q32, 35);
    energy += (int64_t)MulShiftU64((uint64_t)sourceKinetic, fraction, 32);

    return energy;
}

// 出ていく割合の合計の、引く側の熱を負にしない換算(gas_reference.cpp の EffectiveOutflow。T-0646・T-0647・ADR-0286):
// 割合の合計 × (C + n R) ÷ C(C = 熱容量。押し出す仕事 n R T のぶん)。等温はそのまま。totals = 風上の成分の和(1 レベルの箱の段と木の段)
uint64_t EffectiveOutflow(GasGpuTotals totals, uint64_t outflow) {
    if (outflow == 0 || GAS_P.thermodynamics != GAS_GPU_THERMODYNAMICS_ADIABATIC)
        return outflow;

    const uint64_t capacity = totals.capacity;
    if (capacity == 0)
        return outflow;

    const FxDivResult push = FxDivU128By64(FxMulU64Full(totals.amount, GAS_CONSTANT_NANOJOULES_PER_MOLE_KELVIN), MICRO);
    const uint64_t pushCapacity = push.quotient + (push.remainder != 0 ? 1 : 0);  // n R(nJ/K。切り上げ)

    return FxDivU128By64(FxMulU64Full(outflow, capacity + pushCapacity), capacity).quotient;
}

#endif  // BICAMERAL_GAS_CELL_HLSLI
