// time_step.hlsli — 刻みの長さを「状態が変わる速さ」で選ぶ(研究 R-T2。ADR-0390・ADR-0410・20 §14.2。T-0856)の式。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU の試作は engine/src/sim/time_step_lab.cpp(GPU は T-0951)。
//
// ブロックは 1 歩で N = 2^e 刻み進む(e = 0〜TS_MAX_EXPONENT。歩の境目は 2^e の倍数の刻み = 2 進の区間なので、
// 隣どうしの歩は入れ子か離れている)。1 歩の中の進め方(試作):
//   - 運ぶ・広がる(熱の伝導・川と一緒に運ばれる): 後ろ向きの Euler(長い刻みでも安定。一定に流れる所の釣り合いを崩さない)
//   - 決まった割合の反応: 閉じた形 X0 (1 − e^−kN)(TsDecayedPart。長さによらない。端数は決まった乱数で丸める)
//   面の流れは 1 回だけ作って両側に足す(保存は厳密。20 §2.4)。
// 選び方(TsDesiredExponent): 前の歩での変化(計器の精度の単位。温度 1 mK・量はセルの中身の 100 万分の 1。D-435)から
//   決まった整数の段で。試し 1 = 規則 A(変化の量。N に比例と見る)・試し 2 = 規則 B(後ろ向きの Euler の 1 回と半分 2 回の差 =
//   局所の誤差の見積もり。N² に比例と見る。進めるのは 1 回の解き)。伸ばした先でも予算の半分に入るなら 1 段伸ばし、
//   超えれば縮める。
// 隣のブロックとの差は 1 段まで・歩の境目でだけ変える(呼ぶ側。20 §14.2)。浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_TIME_STEP_HLSLI
#define BICAMERAL_TIME_STEP_HLSLI

#include "fixed.hlsli"

#ifdef __cplusplus
#define TS_NAMESPACE_BEGIN           \
    namespace bicameral::time_step { \
        using namespace ::bicameral::fx;
#define TS_NAMESPACE_END }
#else
#define TS_NAMESPACE_BEGIN
#define TS_NAMESPACE_END
#endif

TS_NAMESPACE_BEGIN

// --- 定数 --------------------------------------------------------------------------------------
FX_CONST int32_t TS_MAX_EXPONENT = 20;   // 2^20 刻み = 約 4.9 時間
FX_CONST uint32_t TS_RULE_CHANGE = 0;    // 規則 A: 変化の量
FX_CONST uint32_t TS_RULE_ESTIMATE = 1;  // 規則 B: 誤差の見積もり(半分 2 回 − 1 回)
FX_CONST uint64_t TS_ONE_Q32 = FX_U64(1u, 0u);
FX_CONST uint32_t TS_TEMPERATURE_BITS = 16;  // 温度は Q16 mK で持つ

// --- 小さな道具 --------------------------------------------------------------------------------

// a × b ÷ 2^32(Q32 の積。0 方向の切り捨て)
FX_FN uint64_t TsMulQ32(uint64_t a, uint64_t b) {
    return FxShiftRightU128(FxMulU64Full(a, b), 32);
}

// floor(|value| × multiplier × weight ÷ 2^shift)に value の符号(shift は 1〜96。途中は 192bit、結果は 63bit に収まること)
FX_FN int64_t TsProductTerm(int64_t value, uint64_t multiplier, uint64_t weight, uint32_t shift) {
    FX_ASSERT(shift >= 1 && shift <= 96);
    const FxU128 product = FxMulU64Full(FxAbsU64(value), weight);
    const FxU128 low = FxMulU64Full(product.lo, multiplier);
    const FxU128 high = FxMulU64Full(product.hi, multiplier);
    FX_ASSERT(high.hi == 0);

    // --- 全体 = upper × 2^64 + low.lo(upper は 128bit)---
    FxU128 upper;
    upper.lo = low.hi + high.lo;
    upper.hi = upper.lo < low.hi ? (uint64_t)1 : (uint64_t)0;
    uint64_t magnitude = 0;
    if (shift >= 64)
        magnitude = FxShiftRightU128(upper, shift - 64);
    else {
        FX_ASSERT(upper.hi == 0 && (upper.lo >> shift) == 0);
        magnitude = (upper.lo << (64 - shift)) | (low.lo >> shift);
    }

    return FxApplySign(magnitude, value < 0);
}

// 決まった割合 k(Q32/刻み)で ticks 刻みに減る分 = amount × (1 − e^−x)、x = k·ticks。x < 1/4 は 1 − e^−x を Q62 の級数
// (x (1 − x/2 (1 − x/3 (…))) を 14 項。打ち切りの残り < 2^-62)、それ以上は 1 − e^−x(Q62 の exp)。x ≥ 40 は全部
FX_CONST uint64_t TS_DECAY_ALL_Q32 = FX_U64(40u, 0u);
FX_CONST uint64_t TS_QUARTER_Q32 = FX_U64(0u, 0x40000000u);
FX_CONST uint64_t TS_ONE_Q62 = FX_U64(0x40000000u, 0u);
FX_CONST uint32_t TS_DECAY_TERMS = 14;

FX_FN uint64_t TsDecayFractionQ62(uint64_t xQ32) {
    if (xQ32 >= TS_QUARTER_Q32)
        return TS_ONE_Q62 - FxExpQ32(-(int64_t)xQ32, 62);

    const uint64_t x = xQ32 << 30;  // Q62(< 2^60)
    uint64_t inner = TS_ONE_Q62;
    for (uint32_t n = TS_DECAY_TERMS; n >= 2; --n) {
        const uint64_t term = FxShiftRightU128(FxMulU64Full(x, inner), 62) / (uint64_t)n;
        inner = TS_ONE_Q62 - term;
    }

    return FxShiftRightU128(FxMulU64Full(x, inner), 62);
}

// 端数は決まった乱数で丸める(random = FxHash64 の値。端数 > 乱数の 62bit なら切り上げ。期待値は丸める前と同じ = 偏りが無い。
// 最も近い整数への丸めは、毎刻みの歩でほぼ同じ端数が続くと偏りが溜まった〔1 日で 100 万分の 1 を超える。T-0856〕。D-552 の広がる家族と同じ形)
FX_FN int64_t TsDecayedPart(int64_t amount, uint64_t rateQ32, uint64_t ticks, uint64_t random) {
    FX_ASSERT(amount >= 0 && ticks != 0 && ticks <= ((uint64_t)1 << TS_MAX_EXPONENT));
    if (rateQ32 == 0 || amount == 0)
        return 0;

    const FxU128 x = FxMulU64Full(rateQ32, ticks);
    if (x.hi != 0 || x.lo >= TS_DECAY_ALL_Q32)
        return amount;

    const uint64_t fraction = TsDecayFractionQ62(x.lo);
    const FxU128 product = FxMulU64Full((uint64_t)amount, fraction);
    const uint64_t whole = FxShiftRightU128(product, 62);
    const uint64_t remainder = product.lo & (TS_ONE_Q62 - 1);

    return (int64_t)(whole + (remainder > (random >> 2) ? (uint64_t)1 : (uint64_t)0));
}

// --- 熱 -----------------------------------------------------------------------------------------

// 温度(Q16 mK)= 熱(mJ)÷ 熱容量(Q16 mJ/mK)× 2^32。負の熱は 0 K として読む
FX_FN int64_t TsTemperatureQ16(int64_t heat, uint64_t capacityQ16) {
    FX_ASSERT(capacityQ16 != 0);
    if (heat <= 0)
        return 0;

    FxU128 numerator;
    numerator.hi = (uint64_t)heat >> 32;
    numerator.lo = (uint64_t)heat << 32;

    return (int64_t)FxDivU128By64(numerator, capacityQ16).quotient;
}

// 面(や地面・流れ出し)の流れ(a → b が正、値の単位)= 係数 × N × (y_a − y_b)。係数は 2^-16 値 / y の単位 / 刻み、y は Q16
FX_FN int64_t TsFaceFlow(uint64_t coefficientQ16, uint64_t ticks, int64_t valueA, int64_t valueB) {
    return TsProductTerm(valueA - valueB, ticks, coefficientQ16, 32);
}

// --- 刻みの長さを選ぶ ----------------------------------------------------------------------------

// 前の歩の測った値 measure(精度の単位)から次の e の望み。規則 A は N に比例、規則 B は N² に比例と見る。
// 伸ばした先でも予算の半分に入る(A: 予算の 1/4・B: 1/8 以下)なら 1 段伸ばす(1 歩に 1 段まで。一度に伸ばすと、
// 近づく前線を知らずに長い歩に入る。半分の余裕は伸ばしては縮むの繰り返しを止める)・予算以下ならそのまま・
// 超えれば予算に入るまで縮める
FX_FN int32_t TsDesiredExponent(uint32_t rule, int32_t exponent, uint64_t measure, uint64_t budget) {
    const uint32_t power = rule == TS_RULE_ESTIMATE ? 2u : 1u;
    if (measure <= (budget >> (power + 1)))
        return exponent < TS_MAX_EXPONENT ? exponent + 1 : exponent;

    if (measure <= budget)
        return exponent;

    int32_t shrink = exponent > 0 ? 1 : 0;
    while (shrink < exponent && (budget << (power * (uint32_t)shrink)) < measure)
        ++shrink;

    return exponent - shrink;
}

// 刻み tick で始めてよい最も長い e(tick が 2^e の倍数)
FX_FN int32_t TsAlignedCap(uint64_t tick) {
    if (tick == 0)
        return TS_MAX_EXPONENT;

    int32_t exponent = 0;
    while (exponent < TS_MAX_EXPONENT && ((tick >> (uint32_t)exponent) & 1) == 0)
        ++exponent;

    return exponent;
}

TS_NAMESPACE_END

#endif  // BICAMERAL_TIME_STEP_HLSLI
