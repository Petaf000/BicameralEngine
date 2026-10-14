// radiation.hlsli — 離れたセルどうしの放射の熱の交換(docs/design/07-transport.md §1.3。T-0028・T-0583・ADR-0252)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(sim/radiation_reference.cpp)と、後の GPU(T-0586)が同じ関数を呼ぶ。
//
// データの流れ:
//   セルの成分(物質の任意の欄「1 mol が光を遮る面積」a。パッケージ radiation_absorption_mm2_per_mol)
//   → 光学的な厚さ τ = Σ a·n ÷ 面の面積(RdOpticalDepthQ32)→ セルの吸収率 α = 1 − e^−τ(RdAbsorptanceQ16。放射率も同じ値: 灰色体・Kirchhoff)
//   → 2 つのセル i・j の対の重み w = α_i·α_j·F_ij(F は見え方の割合。光線で求めるのは T-0584)
//   → 対の係数 h = σ·A·Δt·w·(T_i + T_j)(T_i² + T_j²)(RdPairCoefficient。伝導の面の係数と同じ単位 2^-32 mJ/mK)
//   → 対の流れ Q = h × (T_i − T_j)(RdPairFlow。i → j が正)= σ·A·Δt·w·(T_i⁴ − T_j⁴)
//
// 約束(D-465・D-466):
//   - 保存: 対の流れは 1 つの整数で、i から引いた値と同じ値を j に足す(呼ぶ側)。h は i と j について対称な式なので、
//     i と j を入れ替えると符号だけが反転する(ビット単位)。どちらの側が計算しても同じ値。
//   - 坂を下る: h ≥ 0 なので、熱は温度の高い側から低い側へだけ流れる。行き過ぎの上限(RdPairLimit)は対の分け前 × 熱容量で、
//     1 つのセルの放射の分け前の合計が 1/4 以下なら、伝導(6 面で 6/8)と合わせて新しい温度は凸結合になり、温度の順序が入れ替わらない。
//   - 4 乗の差は因数分解した (T_i − T_j)(T_i + T_j)(T_i² + T_j²) で求める(表引きをしない。差が小さくても桁が落ちない)。
//   - 温度は RD_TEMPERATURE_LIMIT_MK(約 134,000 K)で飽和させる。飽和は各セルの温度だけの関数なので、対称と保存は崩れない。
// σ(Stefan–Boltzmann 定数)は器の換算の定数(SI の定義値 k_B・h・c から決まる。気体定数 R と同じ扱い。ADR-0171 §2・ADR-0252)。
// 物質ごとの放射の強さは a(と、その結果の α ≤ 1)でパッケージが決める。a を書かない物質は放射しない・吸わない(既定 0。ブランクの世界は放射なし)。
#ifndef BICAMERAL_RADIATION_HLSLI
#define BICAMERAL_RADIATION_HLSLI

#include "reaction.hlsli"

RX_NAMESPACE_BEGIN

// --- 単位と換算(レベル 0 のセル: 0.5 m 角・面 0.25 m²。1 刻み = 1/60 秒)---

// 温度の飽和(mK)。2^27 − 1 mK ≒ 134,217 K。和 < 2^28・二乗和 < 2^55 で、係数の掛け算が 128bit に収まる
FX_CONST uint32_t RD_TEMPERATURE_LIMIT_MK = 0x07FFFFFFu;

// σ·A·Δt(mJ/K⁴)= 5.670374419e-8 W/(m²K⁴) × 0.25 m² ÷ 60 × 1000 = 2.36265600792e-7。
// 係数の換算: h(2^-32 mJ/mK)= σAΔt × w × (T_i + T_j)(T_i² + T_j²)[mK³] × 1e-12 × 2^32。
// g = ((T_i + T_j)(T_i² + T_j²)) >> 32 と置くと h = σAΔt × 2^64 × 1e-12 × w × g。その定数の Q32 = floor(4.35832... × 2^32)
FX_CONST uint64_t RD_PAIR_SCALE_Q32 = FX_U64(0x00000004u, 0x5BBB95C8u);  // 18,718,889,416

// τ の換算: τ = Σ a[mm²/mol] × n[µmol] ÷ (面 250,000 mm² × 10^6 µmol/mol)。
// τ(Q32)= (Σ a·n × floor(2^70 ÷ 2.5e11)) >> 38
FX_CONST uint64_t RD_DEPTH_RECIPROCAL = FX_U64(0x00000001u, 427399186u);  // 4,722,366,482
FX_CONST uint32_t RD_DEPTH_SHIFT = 38;
// τ ≥ 2^5 = 32 なら e^−τ < 2^−46 で、吸収率は Q16 で 1(65536)。和をこの値で飽和させて 64bit に収める
FX_CONST uint64_t RD_DEPTH_SATURATED_Q32 = FX_U64(0x00000020u, 0u);
FX_CONST uint32_t RD_ONE_Q16 = 65536u;

// 熱容量 C(nJ/K)→ 対の上限の係数(2^-32 mJ/mK): C × 1/4 ÷ 1e9。(C >> 12) × floor(2^32 × 4096 ÷ 4e9)。
// 伝導の HC_LIMIT_PER_CAPACITY(1/8)の 2 倍。対の分け前(Q16)を掛けて使う
FX_CONST uint32_t RD_CAPACITY_SHIFT = 12;
FX_CONST uint64_t RD_LIMIT_PER_CAPACITY = 4398;

// --- セルの吸収率 ---

// Σ a·n(mm²/mol × µmol)の和 → τ(Q32)。和が大きすぎる時は飽和(吸収率 1)
FX_FN uint64_t RdOpticalDepthQ32(FxU128 absorptionSum) {
    // RD_DEPTH_SATURATED_Q32 に相当する和 = 32 × 2.5e11 = 8e12 < 2^43。hi が 0 でなければ十分に厚い
    if (absorptionSum.hi != 0)
        return RD_DEPTH_SATURATED_Q32;

    const uint64_t depth = FxShiftRightU128(FxMulU64Full(absorptionSum.lo, RD_DEPTH_RECIPROCAL), RD_DEPTH_SHIFT);

    return depth < RD_DEPTH_SATURATED_Q32 ? depth : RD_DEPTH_SATURATED_Q32;
}

// 吸収率 α = 1 − e^−τ(Q16。0〜65536)。放射率も同じ値(灰色体。Kirchhoff の法則: 吸う割合と出す割合が等しいので、
// 同じ温度の物どうしは差し引き 0 になり、放射だけで温度の坂を上らない)
FX_FN uint32_t RdAbsorptanceQ16(uint64_t depthQ32) {
    if (depthQ32 >= RD_DEPTH_SATURATED_Q32)
        return RD_ONE_Q16;

    const uint64_t transmitted = FxExpQ32(-(int64_t)depthQ32, 16);  // e^−τ(Q16。0 方向の切り捨て)

    return transmitted >= RD_ONE_Q16 ? 0u : RD_ONE_Q16 - (uint32_t)transmitted;
}

// 1 成分の a·n を和に足す(呼ぶ側が成分ごとに回す。物質の表の形はセルの形ごとに違うので、ここでは表を知らない)
FX_FN FxU128 RdAddAbsorption(FxU128 sum, uint32_t absorptionMm2PerMol, uint64_t amountMicromoles) {
    const FxU128 term = FxMulU64Full(amountMicromoles, (uint64_t)absorptionMm2PerMol);
    const uint64_t low = sum.lo + term.lo;

    FxU128 result;
    result.hi = sum.hi + term.hi + (low < sum.lo ? (uint64_t)1 : (uint64_t)0);
    result.lo = low;

    return result;
}

// --- 対の重み・係数・流れ ---

// 対の重み w = α_i × α_j × F(すべて Q16)→ Q32。i と j について対称(掛け算の順を固定するので、入れ替えても同じビット)
FX_FN uint64_t RdPairWeightQ32(uint32_t absorptanceAQ16, uint32_t absorptanceBQ16, uint32_t viewQ16) {
    const uint64_t smaller = absorptanceAQ16 < absorptanceBQ16 ? absorptanceAQ16 : absorptanceBQ16;
    const uint64_t larger = absorptanceAQ16 < absorptanceBQ16 ? absorptanceBQ16 : absorptanceAQ16;

    return ((smaller * larger) * (uint64_t)viewQ16) >> 16;  // ≤ 2^16 × 2^16 × 2^16 = 2^48 → ≤ 2^32(= 1)
}

FX_FN uint64_t RdClampTemperature(uint32_t temperatureMk) {
    return temperatureMk < RD_TEMPERATURE_LIMIT_MK ? (uint64_t)temperatureMk : (uint64_t)RD_TEMPERATURE_LIMIT_MK;
}

// 対の係数 h(2^-32 mJ/mK)。温度は刻みの初め(Jacobi 型。計算の順番に依存しない)。h は i と j について対称
FX_FN uint64_t RdPairCoefficient(uint32_t temperatureAMk, uint32_t temperatureBMk, uint64_t weightQ32) {
    const uint64_t a = RdClampTemperature(temperatureAMk);
    const uint64_t b = RdClampTemperature(temperatureBMk);
    const uint64_t sum = a + b;              // < 2^28
    const uint64_t squares = a * a + b * b;  // < 2^55

    // g = (和 × 二乗和) >> 32 < 2^51。c = σAΔt の定数 × w ≤ 2^35。h = (c × g) >> 32 < 2^54
    const uint64_t cubic = FxShiftRightU128(FxMulU64Full(sum, squares), 32);
    const uint64_t scale = FxShiftRightU128(FxMulU64Full(RD_PAIR_SCALE_Q32, weightQ32), 32);

    return FxShiftRightU128(FxMulU64Full(scale, cubic), 32);
}

// 行き過ぎの上限: 対の係数 ≤ min(両側の熱容量) × 1/4 × 対の分け前(Q16)。分け前は呼ぶ側が決める
// (1 つのセルの分け前の合計 ≤ 1 にすると、放射だけで 1 刻みに動く温度は差の 1/4 まで)
FX_FN uint64_t RdPairLimit(uint64_t heatCapacityANanojoules, uint64_t heatCapacityBNanojoules, uint32_t shareQ16) {
    const uint64_t smaller = heatCapacityANanojoules < heatCapacityBNanojoules ? heatCapacityANanojoules
                                                                               : heatCapacityBNanojoules;
    // 熱容量 < 2^62 nJ/K の範囲で 64bit に収まる(伝導の HcFaceLimit と同じ形)
    const uint64_t perShare = (smaller >> RD_CAPACITY_SHIFT) * RD_LIMIT_PER_CAPACITY;

    return FxShiftRightU128(FxMulU64Full(perShare, (uint64_t)shareQ16), 16);
}

// 対の流れ(i → j が正、mJ)。係数は RdPairCoefficient と RdPairLimit の小さい方を渡す
FX_FN int64_t RdPairFlow(uint32_t temperatureAMk, uint32_t temperatureBMk, uint64_t coefficient) {
    const uint64_t a = RdClampTemperature(temperatureAMk);
    const uint64_t b = RdClampTemperature(temperatureBMk);
    const bool outward = a >= b;
    const uint64_t difference = outward ? a - b : b - a;

    // |ΔT| < 2^27、係数 < 2^55 なので積 < 2^82、流れ < 2^50
    const uint64_t magnitude = FxShiftRightU128(FxMulU64Full(difference, coefficient), 32);

    return outward ? (int64_t)magnitude : -(int64_t)magnitude;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_RADIATION_HLSLI
