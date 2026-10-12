// world_physics.hlsli — 世界ごとに違ってよい物理の値(今は重力の強さ)の、器の側の決まり: 単位・範囲・各段の単位への換算。
// 値そのものは世界のデータ(ADR-0151 の 5・ADR-0171)で、物理(PxParameters::gravity)と気体(GasConfig::gravityMicrometersPerSecond2)が
// 同じ 1 つの値から換算して持つ。CPU リファレンスと GPU は同じ換算の関数を通るので、同じ値になる(原則 4)。
// HLSL と C++ の両方から読む(fixed.hlsli の約束に従う)。浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_WORLD_PHYSICS_HLSLI
#define BICAMERAL_WORLD_PHYSICS_HLSLI

#include "fixed.hlsli"

FX_NAMESPACE_BEGIN

// --- 重力の強さ(世界のデータ)-----------------------------------------------------------------
// 1 = 1 µm/s²(uint32)。向きは器が決める(物理は -y、気体は -z。どちらも「下」)。0 = 無重力
// 上限 100 m/s²(約 10 g): 気体の重さの力積の係数 g × 2^36(gas_reference.cpp の MakeGasCoefficients)が 64bit に収まるのは
// 約 268 m/s² まで。余裕を見て 100 m/s² で切る(層の重さの圧力・物理の変位の桁はこれより広い)
FX_CONST uint32_t GRAVITY_MAX_MICROMETERS_PER_SECOND2 = 100000000;

// 標準の重力 9.80665 m/s²(定義値)。標準パッケージ(T-0373)が世界のデータとして渡すまでの、ただ 1 つの置き場所
FX_CONST uint32_t STANDARD_GRAVITY_MICROMETERS_PER_SECOND2 = 9806650;

// --- 各段の単位への換算(端数は四捨五入。標準の重力では今までの定数とビット一致)------------------
// 物理: 2^-20 m/s²(9806650 → 10283018)
FX_FN int64_t PxGravityFromMicrometers(uint32_t gravityMicrometersPerSecond2) {
    return (int64_t)((((uint64_t)gravityMicrometersPerSecond2 << 20) + 500000) / 1000000);
}

// 気体: 質量 1 mg が 1 セル(0.5 m)上がる仕事 = 1e-6 kg × g × 0.5 m の µJ × 10^6(9806650 → 4903325)
FX_FN uint64_t GasGravityWorkFromMicrometers(uint32_t gravityMicrometersPerSecond2) {
    return ((uint64_t)gravityMicrometersPerSecond2 + 1) >> 1;
}

// 気体: 層の重さで下がる圧力 m(mg)× 1e-6 × g ÷ A(0.25 m²)の µPa × 10^4(9806650 → 392266)
FX_FN uint64_t GasHydrostaticFromMicrometers(uint32_t gravityMicrometersPerSecond2) {
    return ((uint64_t)gravityMicrometersPerSecond2 * 4 + 50) / 100;
}

FX_NAMESPACE_END

#endif  // BICAMERAL_WORLD_PHYSICS_HLSLI
