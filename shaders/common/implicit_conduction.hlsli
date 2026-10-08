// implicit_conduction.hlsli — 細かいレベルの熱の陰解法(方式②。ADR-0019)の式。HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。
// CPU リファレンスは engine/src/sim/implicit_conduction.cpp、GPU は shaders/sim/implicit_conduct.hlsl(T-0117)。同じ関数を呼ぶので
// 1 刻みの結果(エネルギー・端数・V サイクルの回数・安全網)がビット単位で一致する。
//
// データの流れ: セルのエネルギー → 温度(ImTemperature。mK × 2^16)→ V サイクルで近似解 T* → 面の流れ(ImFaceFlow)
//   → 安全網(刻みの初めの温度の範囲の外に出るセルの面を陽解法の流れ ImExplicitCoefficient に戻す)→ 両側に足す(MrSplitCrossFlow)
// 数の幅(04 R8): 面の係数は 4^k で大きくなるので 128bit。反復は「係数 ÷ 対角」の重み(Q48)だけを使う。
#ifndef BICAMERAL_IMPLICIT_CONDUCTION_HLSLI
#define BICAMERAL_IMPLICIT_CONDUCTION_HLSLI

#include "multires_conduction.hlsli"

MR_NAMESPACE_BEGIN

// --- 定数 --------------------------------------------------------------------------------------
FX_CONST uint32_t IM_WEIGHT_SHIFT = 48;                        // 重みの Q
FX_CONST uint32_t IM_TEMPERATURE_SHIFT = 16;                   // 温度の端数(mK × 2^16)
FX_CONST uint32_t IM_FLOW_SHIFT = 32 + IM_TEMPERATURE_SHIFT;   // 係数(2^-32)× 温度(2^-16)→ エネルギー
FX_CONST uint32_t IM_ENERGY_BITS_PER_LEVEL = 3;                // 1 段で単位は 8 倍
FX_CONST uint32_t IM_CORRECTION_SCALE_SHIFT = 8;               // 多重格子の直しの倍率の Q
FX_CONST int64_t IM_EXCESS_CAP = (int64_t)FX_U64(0x100u, 0u);  // 範囲を超えた量の記録の上限(2^40)
FX_CONST int64_t IM_INT64_MAX = (int64_t)FX_U64(0x7FFFFFFFu, 0xFFFFFFFFu);
FX_CONST int64_t IM_INT64_MIN = (int64_t)FX_U64(0x80000000u, 0u);

// --- GPU のバッファの形(C++ と同じ並び。64bit の欄を先に置き、大きさは 8 の倍数)-----------------------------
struct ImGpuCell {
    int64_t energy;         // そのレベルの単位
    uint64_t fraction;      // 2^-64 単位の端数(違うレベルの面の粗い側が受ける)
    uint64_t heatCapacity;  // C(一定。試作の約束)
    int64_t
        startTemperature;  // 0 以上なら刻みの初めの温度(mK × 2^16。木につなぐ時は MrCellThermal から。T-0119・T-0127)
    uint32_t faceStart;    // 面の一覧(g_lists の番地。番号 × 2 + 粗い側なら 1。面の番号の昇順 = CPU の足す順)
    uint32_t faceEnd;
};

struct ImGpuFace {
    uint64_t coefficientHigh;  // 面の係数(128bit)
    uint64_t coefficientLow;
    uint32_t fine;
    uint32_t coarse;
    uint32_t gap;
    uint32_t coarseFraction;  // 粗い側が端数を受けられるか(0 なら整数の単位の倍数だけ。ADR-0017。T-0127)
};

// 多重格子の節(全部の段を 1 本に並べた番号。段 0 = セル)
struct ImGpuNode {
    int64_t selfWeight;      // C ÷ D(Q48)
    int64_t restrictWeight;  // 自分の D(親の単位)÷ 親の D(最も粗い段は 0)
    uint32_t linkStart;      // 隣の一覧(g_links)
    uint32_t linkEnd;
    uint32_t parent;      // 親の節(全体の番号。最も粗い段は 0)
    uint32_t color;       // 赤黒の色
    uint32_t childStart;  // 子の一覧(g_lists の番地。子の番号の昇順)
    uint32_t childEnd;
};

struct ImGpuLink {
    int64_t weight;  // 面の係数 ÷ D(Q48)
    uint32_t neighbor;
    uint32_t padding;
};

// --- 128bit の小さな道具(負にならない値)-------------------------------------------------------
FX_FN FxU128 ImWide(uint64_t value) {
    FxU128 result = {(uint64_t)0, value};
    return result;
}

FX_FN bool ImWideLess(FxU128 a, FxU128 b) {
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

FX_FN FxU128 ImWideMin(FxU128 a, FxU128 b) {
    if (ImWideLess(a, b))
        return a;

    return b;
}

// 桁あふれは R8 の assert
FX_FN FxU128 ImWideShiftLeft(FxU128 value, uint32_t shift) {
    if (shift == 0 || (value.hi == 0 && value.lo == 0))
        return value;

    FX_ASSERT(shift < 128);
    if (shift >= 64) {
        FX_ASSERT(value.hi == 0 && (shift == 64 || (value.lo >> (128 - shift)) == 0));
        FxU128 high = {value.lo << (shift - 64), (uint64_t)0};
        return high;
    }

    FX_ASSERT((value.hi >> (64 - shift)) == 0);
    FxU128 result = {(value.hi << shift) | (value.lo >> (64 - shift)), value.lo << shift};

    return result;
}

// a × b(b は 64bit)。桁あふれは assert
FX_FN FxU128 ImWideMul(FxU128 a, uint64_t b) {
    const FxU128 low = FxMulU64Full(a.lo, b);
    const FxU128 high = FxMulU64Full(a.hi, b);
    FX_ASSERT(high.hi == 0);
    FxU128 product = {low.hi + high.lo, low.lo};
    FX_ASSERT(product.hi >= low.hi);

    return product;
}

FX_FN bool ImWideIsZero(FxU128 value) {
    return value.hi == 0 && value.lo == 0;
}

// a + b(桁あふれは assert)
FX_FN FxU128 ImWideAdd(FxU128 a, FxU128 b) {
    FxU128 sum = {a.hi + b.hi, a.lo + b.lo};
    if (sum.lo < a.lo)
        sum.hi += 1;

    FX_ASSERT(sum.hi >= a.hi);

    return sum;
}

FX_FN FxU128 ImWideShiftRight(FxU128 value, uint32_t shift) {
    if (shift == 0)
        return value;

    if (shift >= 128)
        return ImWide((uint64_t)0);

    if (shift >= 64)
        return ImWide(value.hi >> (shift - 64));

    FxU128 result = {value.hi >> shift, (value.lo >> shift) | (value.hi << (64 - shift))};

    return result;
}

FX_FN uint32_t ImWideMsb(FxU128 value) {
    return value.hi != 0 ? 64 + FxMsbU64(value.hi) : FxMsbU64(value.lo);
}

// numerator ÷ denominator を Q fractionBits で(商は 2^63 未満であること)。分母が 63bit を超える時は両方の下位を落とす
// (重みの近似が少し粗くなるだけ。保存には効かない)。多重格子の段の重み(CPU の BuildImplicitGrid と GPU の implicit_levels.hlsl。T-0134)
FX_FN int64_t ImWideRatio(FxU128 numerator, FxU128 denominator, uint32_t fractionBits) {
    FX_ASSERT(!ImWideIsZero(denominator));
    if (ImWideIsZero(numerator))
        return 0;

    const uint32_t msb = ImWideMsb(denominator);
    const uint32_t drop = msb > 62 ? msb - 62 : 0;
    const uint64_t divisor = ImWideShiftRight(denominator, drop).lo;
    const FxU128 scaled = ImWideShiftLeft(ImWideShiftRight(numerator, drop), fractionBits);
    FX_ASSERT(scaled.hi < divisor);
    const uint64_t quotient = FxDivU128By64(scaled, divisor).quotient;
    FX_ASSERT(quotient < ((uint64_t)1 << 63));

    return (int64_t)quotient;
}

// 多重格子の重み(Q48)
FX_FN int64_t ImWeight(FxU128 numerator, FxU128 denominator) {
    return ImWideRatio(numerator, denominator, IM_WEIGHT_SHIFT);
}

// --- 式 ----------------------------------------------------------------------------------------

// 重み × 値(Q48)
FX_FN int64_t ImMulWeight(int64_t weight, int64_t value) {
    return FxMulShiftS64(weight, value, IM_WEIGHT_SHIFT);
}

// 負にもなる値を符号と大きさで右へずらす(0 方向に切り捨て。算術シフトの丸めの向きに依存しない)
FX_FN int64_t ImShiftRightSigned(int64_t value, uint32_t shift) {
    return FxApplySign(shift >= 64 ? (uint64_t)0 : FxAbsU64(value) >> shift, value < 0);
}

// 範囲の判定用に頭打ちで足す
FX_FN int64_t ImAddClamped(int64_t a, int64_t b) {
    if (b > 0 && a > IM_INT64_MAX - b)
        return IM_INT64_MAX;

    if (b < 0 && a < IM_INT64_MIN - b)
        return IM_INT64_MIN;

    return a + b;
}

// 温度(mK × 2^16)= エネルギー × 2^48 ÷ 熱容量(整数部のエネルギーだけから)
FX_FN int64_t ImTemperature(int64_t energy, uint64_t heatCapacity) {
    const uint64_t magnitude = FxAbsU64(energy);
    FxU128 numerator = {magnitude >> (64 - IM_FLOW_SHIFT), magnitude << IM_FLOW_SHIFT};

    return FxApplySign(FxDivU128By64(numerator, heatCapacity).quotient, energy < 0);
}

// 温度(mK)からエネルギー
FX_FN int64_t ImEnergyFor(uint64_t heatCapacity, int64_t millikelvin) {
    const uint64_t energy = FxShiftRightU128(FxMulU64Full(heatCapacity, FxAbsU64(millikelvin)), 32);

    return FxApplySign(energy, millikelvin < 0);
}

// 面の流れ(細かい側の単位。細かい → 粗いが正)= 係数 × 温度差 ÷ 2^48
FX_FN int64_t ImFaceFlow(FxU128 coefficient, int64_t difference) {
    const uint64_t magnitude = FxAbsU64(difference);
    const FxU128 low = FxMulU64Full(coefficient.lo, magnitude);
    const FxU128 high = FxMulU64Full(coefficient.hi, magnitude);
    FX_ASSERT(high.hi == 0);

    // (high · 2^64 + low) >> 48 = (high + low.hi) << 16 + low.lo >> 48
    const uint64_t upper = high.lo + low.hi;
    FX_ASSERT(upper >= low.hi && (upper >> (63 - (64 - IM_FLOW_SHIFT))) == 0);
    const uint64_t flow = (upper << (64 - IM_FLOW_SHIFT)) + (low.lo >> IM_FLOW_SHIFT);
    FX_ASSERT(flow < FX_U64(0x80000000u, 0u));

    return FxApplySign(flow, difference < 0);
}

// 陽解法の面の係数(細かい側の単位)= min(係数, 細かい側の C/8, 粗い側の C/8 × 2^d)(ADR-0017。安全網が使う)
FX_FN FxU128 ImExplicitCoefficient(FxU128 coefficient, uint64_t fineCapacity, uint64_t coarseCapacity, uint32_t gap) {
    const FxU128 fineLimit = ImWide(fineCapacity >> IM_ENERGY_BITS_PER_LEVEL);
    const FxU128 coarseLimit = ImWideShiftLeft(ImWide(coarseCapacity >> IM_ENERGY_BITS_PER_LEVEL), gap);

    return ImWideMin(coefficient, ImWideMin(fineLimit, coarseLimit));
}

// 新しい温度の誤差の見込み(|残差| × D/C)が tolerance(mK)を超えるか。|残差| × 2^48 > tolerance × 2^16 × (C/D の重み)
FX_FN bool ImExceedsTolerance(int64_t residual, int64_t selfWeight, uint32_t toleranceMillikelvin) {
    const FxU128 tolerance = ImWide((uint64_t)toleranceMillikelvin << IM_TEMPERATURE_SHIFT);
    const FxU128 amplified = ImWideShiftLeft(ImWide(FxAbsU64(residual)), IM_WEIGHT_SHIFT);
    const FxU128 allowed = ImWideMul(tolerance, (uint64_t)selfWeight);

    return ImWideLess(allowed, amplified);
}

// 範囲を超えた量(中なら 0 以下)
FX_FN int64_t ImOverrun(int64_t energy, int64_t lowest, int64_t highest) {
    return energy > highest ? energy - highest : lowest - energy;
}

// 範囲を超えた量を mK に(大きすぎる時は 2^40 単位で止める)
FX_FN int64_t ImExcessMillikelvin(int64_t over, uint64_t heatCapacity) {
    if (over <= 0)
        return 0;

    return ImTemperature(over < IM_EXCESS_CAP ? over : IM_EXCESS_CAP, heatCapacity) >> IM_TEMPERATURE_SHIFT;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_IMPLICIT_CONDUCTION_HLSLI
