// gas_internal.h — gas_reference.cpp(1 レベルの箱)と gas_tree.cpp(多重解像度の木。T-0208)が共有する、気体の定数と 1 セルの熱力学の部品
// (bicameral_gas の外からは使わない)。式の意味は gas_reference.cpp の定義の所のコメントにある。
#pragma once

#include <cstdint>

#include "sim/gas_reference.h"

namespace bicameral::sim::gas_detail {

    // --- 物理の定数(整数。導き方はコメントに)---

    // 理想気体: p(µPa)= n(µmol)× T(mK)× R ÷ V × 1e-3 = n × T × 0.0665157(R = 8.314462618 J/(mol·K)・V = 0.125 m³)。Q32
    inline constexpr uint64_t PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 = 285682760;

    // 層の重さで下がる圧力: m(mg)× 1e-6 × g ÷ A(0.25 m²)= m × 39.2266 µPa(g = 9.80665 m/s²)
    inline constexpr uint64_t HYDROSTATIC_MICROPASCAL_PER_MASS_X10000 = 392266;

    inline constexpr uint64_t GRAVITY_MICROMETER_PER_SECOND2 = 9806650;

    // 重さの仕事: 質量 1 mg が 1 セル(0.5 m)上がる仕事 = 1e-6 kg × g × 0.5 m = 4.903325 µJ(× これ ÷ 1e6)
    inline constexpr uint64_t GRAVITY_WORK_MICROJOULE_PER_MASS_X1E6 = 4903325;

    // 気体定数 R = 8.314462618 J/(mol·K) = 8314.462618 nJ/(µmol·K)(× これ ÷ 1e6。α の γ を作る)
    inline constexpr uint64_t GAS_CONSTANT_NANOJOULE_PER_AMOUNT_X1E6 = 8314462618;

    // 運動量の 2 乗を運動エネルギーへ: ½ m v² = P² × 2^-41 ÷ m(µJ。P は mg·2^-20 m/s、m は mg)
    inline constexpr uint32_t KINETIC_SHIFT = 41;
    inline constexpr uint64_t
        MAX_MOMENTUM = (uint64_t)1 << 52;  // 2 乗を 41 ずらした 3 軸の和が 64bit に収まる(150 g で約 3 万 m/s)
    inline constexpr uint64_t MICRO = 1000000;
    inline constexpr uint64_t ONE_Q32 = (uint64_t)1 << 32;
    inline constexpr uint64_t LOW_32_BITS = ONE_Q32 - 1;

    // 1 セルの物質量の上限(µmol)。移す物質量を Q32 の 64bit で計算するため(0.125 m³ に 2147 mol = 大気の約 400 倍)
    inline constexpr uint64_t MAX_CELL_AMOUNT = (uint64_t)1 << 31;

    // 丸めの乱数の用途(FxHash64。R6)
    inline constexpr uint32_t GAS_ROUNDING_PURPOSE = 0x47610001u;

    // --- 1 セルの部品(gas_reference.cpp で定義)---

    [[nodiscard]] uint64_t MulShiftU64(uint64_t a, uint64_t b, uint32_t shift);
    [[nodiscard]] uint64_t TotalAmount(const GasConfig& config, const GasCell& cell);
    [[nodiscard]] uint64_t HeatCapacity(const GasConfig& config, const GasCell& cell);
    [[nodiscard]] int64_t ChemicalEnergy(const GasConfig& config, const GasCell& cell);
    [[nodiscard]] uint64_t CellMass(const GasConfig& config, const GasCoefficients& coefficients, const GasCell& cell);
    [[nodiscard]] int64_t KineticEnergy(const GasConfig& config, const GasCell& cell, uint64_t mass);
    [[nodiscard]] GasDerived DeriveRaw(const GasConfig& config, const GasCoefficients& coefficients,
                                       const GasCell& cell);
    [[nodiscard]] int64_t DivideNearest(int64_t a, uint64_t b);
    [[nodiscard]] int64_t LimitedSlope(GasReconstruction reconstruction, int64_t a, int64_t b);
    [[nodiscard]] int64_t MovedEnergy(const GasConfig& config, const GasCell& source, const GasDerived& sourceDerived,
                                      const GasCell& moved, uint64_t fraction);

}  // namespace bicameral::sim::gas_detail
