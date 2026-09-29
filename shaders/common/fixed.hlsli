// fixed.hlsli — シミュの整数の数学ライブラリ(docs/design/04-numerics-determinism.md §3)。
// 1 つのソースを HLSL(DXC, SM 6.8)と C++(MSVC)の両方でコンパイルする。GPU のカーネル・CPU のリファレンス・ベイクが
// 同じ式で計算するので、同じ入力からはどの機械でもビット単位で同じ結果になる(D-205・D-307)。浮動小数点は使わない(R1)。
//
// 両方で通すための約束(T-0010):
//   - 型は int32_t / uint32_t / int64_t / uint64_t だけ。関数は FX_FN、定数は FX_CONST を付ける。
//   - 64bit の定数は FX_U64(上位 32bit, 下位 32bit)で書く(HLSL のリテラル接尾辞に頼らない)。
//   - 桁あふれしうる計算は符号なしで行う(C++ の符号つきの桁あふれは未定義)。64 以上のシフトはしない。
//   - 丸めは「0 方向の切り捨て」で統一する(04 §3)。桁あふれはバグとして FX_ASSERT で止める。飽和はしない(R8)。
#ifndef BICAMERAL_FIXED_HLSLI
#define BICAMERAL_FIXED_HLSLI

// --- 言語の違いを吸収する -----------------------------------------------------------------------
#ifdef __cplusplus
#include <cassert>
#include <cstdint>
#define FX_FN constexpr inline
#define FX_CONST inline constexpr
#define FX_NAMESPACE_BEGIN namespace bicameral::fx {
#define FX_NAMESPACE_END }
#ifndef FX_ASSERT
#define FX_ASSERT(condition) assert(condition)
#endif
#else
#define FX_FN
#define FX_CONST static const
#define FX_NAMESPACE_BEGIN
#define FX_NAMESPACE_END
#ifndef FX_ASSERT
#define FX_ASSERT(condition)  // GPU の assert は T-0003(デバッグのリング)でつなぐ
#endif
#endif

#define FX_U64(high32, low32) ((((uint64_t)(high32)) << 32) | ((uint64_t)(low32)))

FX_NAMESPACE_BEGIN

// --- 128bit の値(HLSL に int128 は無いので 2 つの 64bit で持つ)--------------------------------
struct FxU128 {
    uint64_t hi;
    uint64_t lo;
};

struct FxDivResult {
    uint64_t quotient;
    uint64_t remainder;
};

FX_CONST uint64_t FX_LOW32_MASK = FX_U64(0x00000000u, 0xFFFFFFFFu);

// 符号つきの絶対値を符号なしで返す(INT64_MIN も 2^63 として正しく表せる)
FX_FN uint64_t FxAbsU64(int64_t value) {
    return value < 0 ? ~((uint64_t)value) + 1 : (uint64_t)value;
}

// 符号なしの大きさに符号を付けて戻す。大きさは 2^63(負のとき)/ 2^63 - 1(正のとき)まで
FX_FN int64_t FxApplySign(uint64_t magnitude, bool negative) {
    FX_ASSERT(magnitude <= (negative ? FX_U64(0x80000000u, 0u) : FX_U64(0x7FFFFFFFu, 0xFFFFFFFFu)));
    return negative ? (int64_t)(~magnitude + 1) : (int64_t)magnitude;
}

// 最上位の 1 のビットの位置(0〜63)。value は 0 でないこと
FX_FN uint32_t FxMsbU64(uint64_t value) {
    FX_ASSERT(value != 0);
    uint32_t position = 0;
    if (value >= FX_U64(1u, 0u)) {
        value >>= 32;
        position += 32;
    }
    if (value >= (uint64_t)0x10000u) {
        value >>= 16;
        position += 16;
    }
    if (value >= (uint64_t)0x100u) {
        value >>= 8;
        position += 8;
    }
    if (value >= (uint64_t)0x10u) {
        value >>= 4;
        position += 4;
    }
    if (value >= (uint64_t)0x4u) {
        value >>= 2;
        position += 2;
    }
    if (value >= (uint64_t)0x2u) position += 1;
    return position;
}

// a + b が int64 の範囲をはみ出すか(R8: 呼ぶ側が FX_ASSERT で止める)
FX_FN bool FxAddOverflowsS64(int64_t a, int64_t b) {
    const int64_t sum = (int64_t)((uint64_t)a + (uint64_t)b);
    return (a < 0) == (b < 0) && (sum < 0) != (a < 0);
}

// --- 積 ----------------------------------------------------------------------------------------
// 64bit × 64bit → 128bit。32bit に分けて 4 つの部分積を足す(04 §3 の MulHi64)
FX_FN FxU128 FxMulU64Full(uint64_t a, uint64_t b) {
    const uint64_t aLow = a & FX_LOW32_MASK;
    const uint64_t aHigh = a >> 32;
    const uint64_t bLow = b & FX_LOW32_MASK;
    const uint64_t bHigh = b >> 32;
    const uint64_t lowLow = aLow * bLow;
    const uint64_t lowHigh = aLow * bHigh;
    const uint64_t highLow = aHigh * bLow;
    const uint64_t highHigh = aHigh * bHigh;
    const uint64_t middle = (lowLow >> 32) + (lowHigh & FX_LOW32_MASK) + (highLow & FX_LOW32_MASK);
    FxU128 result = {highHigh + (lowHigh >> 32) + (highLow >> 32) + (middle >> 32),
                     (middle << 32) | (lowLow & FX_LOW32_MASK)};
    return result;
}

FX_FN uint64_t FxMulHiU64(uint64_t a, uint64_t b) {
    return FxMulU64Full(a, b).hi;
}

// 符号つき 64bit × 64bit → 128bit(2 の補数)。符号なしの積から、負の側の分を上位から引いて直す
FX_FN FxU128 FxMulS64Full(int64_t a, int64_t b) {
    FxU128 result = FxMulU64Full((uint64_t)a, (uint64_t)b);
    if (a < 0) result.hi -= (uint64_t)b;
    if (b < 0) result.hi -= (uint64_t)a;
    return result;
}

// 128bit を右へ shift(0〜127)ずらした下位 64bit。はみ出す上位のビットがあれば桁あふれ
FX_FN uint64_t FxShiftRightU128(FxU128 value, uint32_t shift) {
    FX_ASSERT(shift < 128);
    if (shift == 0) {
        FX_ASSERT(value.hi == 0);
        return value.lo;
    }
    if (shift < 64) {
        FX_ASSERT((value.hi >> shift) == 0);
        return (value.lo >> shift) | (value.hi << (64 - shift));
    }
    return shift == 64 ? value.hi : value.hi >> (shift - 64);
}

// Q 形式の積: (a × b) / 2^shift を 0 方向に切り捨てる。例: Q16.16 × Q16.16 → shift = 16
FX_FN int64_t FxMulShiftS64(int64_t a, int64_t b, uint32_t shift) {
    const bool negative = (a < 0) != (b < 0);
    const uint64_t magnitude = FxShiftRightU128(FxMulU64Full(FxAbsU64(a), FxAbsU64(b)), shift);
    return FxApplySign(magnitude, negative);
}

FX_FN int32_t FxMulShiftS32(int32_t a, int32_t b, uint32_t shift) {
    FX_ASSERT(shift < 64);
    const bool negative = (a < 0) != (b < 0);
    const uint64_t magnitude = (FxAbsU64(a) * FxAbsU64(b)) >> shift;
    FX_ASSERT(magnitude <= (negative ? (uint64_t)0x80000000u : (uint64_t)0x7FFFFFFFu));
    return (int32_t)FxApplySign(magnitude, negative);
}

// --- 割り算 ------------------------------------------------------------------------------------
// 128bit ÷ 64bit。商が 64bit に収まること(numerator.hi < divisor)。1 ビットずつ引く方法(64 回、分岐のみで決定的)
FX_FN FxDivResult FxDivU128By64(FxU128 numerator, uint64_t divisor) {
    FX_ASSERT(divisor != 0 && numerator.hi < divisor);
    uint64_t remainder = numerator.hi;
    uint64_t low = numerator.lo;
    uint64_t quotient = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        const uint64_t carry = remainder >> 63;  // 左へずらすと 2^64 を超える分
        remainder = (remainder << 1) | (low >> 63);
        low <<= 1;
        quotient <<= 1;
        if (carry != 0 || remainder >= divisor) {
            remainder -= divisor;  // carry があるときは 2^64 を足した値から引くのと同じ(真の値 < divisor)
            quotient |= 1;
        }
    }
    FxDivResult result = {quotient, remainder};
    return result;
}

// Q 形式の割り算: (a × 2^shift) / b を 0 方向に切り捨てる。例: Q16.16 ÷ Q16.16 → shift = 16
FX_FN int64_t FxDivShiftS64(int64_t a, int64_t b, uint32_t shift) {
    FX_ASSERT(b != 0 && shift < 64);
    const bool negative = (a < 0) != (b < 0);
    const uint64_t magnitudeA = FxAbsU64(a);
    FxU128 numerator = {shift == 0 ? (uint64_t)0 : magnitudeA >> (64 - shift), magnitudeA << shift};
    return FxApplySign(FxDivU128By64(numerator, FxAbsU64(b)).quotient, negative);
}

// 64bit の整数の割り算(0 方向の切り捨て)。C++ も DXIL の sdiv も 0 方向に切り捨てる
FX_FN int64_t FxDivS64(int64_t a, int64_t b) {
    FX_ASSERT(b != 0);
    return FxApplySign(FxAbsU64(a) / FxAbsU64(b), (a < 0) != (b < 0));
}

// --- 逆数の掛け算(同じ除数で何度も割る所の割り算を、掛け算とシフトに置き換える)--------------------------
// Granlund & Montgomery(1994)の「除数が不変な割り算」の方法。除数 d から逆数(乗数とシフト)を 1 回作っておけば、
// どの被除数 n でも floor(n / d) を**厳密に**返す(近似ではない。FxDivS64 などと結果がビット単位で同じ)。
// 乗数 m' = floor(2^N × (2^l − d) / d) + 1(l = ceil(log2 d))、商 = (t + ((n − t) >> min(l, 1))) >> max(l − 1, 0)、t = mulhi(m', n)。
// 逆数を作るのは割り算 1 回分の費用なので、除数がベイクの定数か、多くのセルで共有される値のときに使う(04 §6)。
struct FxRecip64 {
    uint64_t multiplier;
    uint32_t shift1;
    uint32_t shift2;
};

struct FxRecip32 {
    uint32_t multiplier;
    uint32_t shift1;
    uint32_t shift2;
};

// 符号つき: 除数の大きさの逆数と、除数の符号
struct FxRecipS64 {
    FxRecip64 magnitude;
    bool negative;
};

// ceil(log2 d)(d ≥ 1)。d = 1 なら 0
FX_FN uint32_t FxCeilLog2U64(uint64_t value) {
    FX_ASSERT(value != 0);
    return value == 1 ? 0 : FxMsbU64(value - 1) + 1;
}

FX_FN FxRecip64 FxMakeRecipU64(uint64_t divisor) {
    FX_ASSERT(divisor != 0);
    const uint32_t ceilLog2 = FxCeilLog2U64(divisor);
    // 2^l − d。l = 64 のときも符号なしの桁あふれで正しく 2^64 − d になる(2^l − d < d なので商は 64bit に収まる)
    const uint64_t powerOfTwo = ceilLog2 == 64 ? (uint64_t)0 : ((uint64_t)1 << ceilLog2);
    const FxU128 numerator = {powerOfTwo - divisor, 0};
    FxRecip64 recip = {FxDivU128By64(numerator, divisor).quotient + 1, ceilLog2 < 1 ? ceilLog2 : 1,
                       ceilLog2 > 1 ? ceilLog2 - 1 : 0};
    return recip;
}

// floor(n / d)。FxMakeRecipU64(d) で作った逆数を使う
FX_FN uint64_t FxDivRecipU64(uint64_t numerator, FxRecip64 recip) {
    const uint64_t high = FxMulHiU64(recip.multiplier, numerator);
    return (high + ((numerator - high) >> recip.shift1)) >> recip.shift2;
}

FX_FN FxRecip32 FxMakeRecipU32(uint32_t divisor) {
    FX_ASSERT(divisor != 0);
    const uint32_t ceilLog2 = FxCeilLog2U64(divisor);
    const uint64_t powerOfTwo = (uint64_t)1 << ceilLog2;  // l ≤ 32
    const uint64_t multiplier = ((powerOfTwo - divisor) << 32) / divisor + 1;
    FX_ASSERT(multiplier <= FX_LOW32_MASK);
    FxRecip32 recip = {(uint32_t)multiplier, ceilLog2 < 1 ? ceilLog2 : 1, ceilLog2 > 1 ? ceilLog2 - 1 : 0};
    return recip;
}

// floor(n / d)(32bit)。上位 32bit の積は 64bit の掛け算 1 回
FX_FN uint32_t FxDivRecipU32(uint32_t numerator, FxRecip32 recip) {
    const uint32_t high = (uint32_t)(((uint64_t)recip.multiplier * numerator) >> 32);
    return (high + ((numerator - high) >> recip.shift1)) >> recip.shift2;
}

FX_FN FxRecipS64 FxMakeRecipS64(int64_t divisor) {
    FX_ASSERT(divisor != 0);
    FxRecipS64 recip = {FxMakeRecipU64(FxAbsU64(divisor)), divisor < 0};
    return recip;
}

// 0 方向に切り捨てた a / d。FxDivS64(a, d) とビット単位で同じ(商の桁あふれ INT64_MIN / −1 は FX_ASSERT)
FX_FN int64_t FxDivRecipS64(int64_t numerator, FxRecipS64 recip) {
    const bool negative = (numerator < 0) != recip.negative;
    return FxApplySign(FxDivRecipU64(FxAbsU64(numerator), recip.magnitude), negative);
}

// --- 平方根(桁ごとの方法。入力のビットだけで決まる)-------------------------------------------
// floor(sqrt(value))。いつも 32 回まわす(GPU で分岐のばらつきを減らす)
FX_FN uint32_t FxSqrtU64(uint64_t value) {
    uint64_t result = 0;
    uint64_t bit = FX_U64(0x40000000u, 0u);  // 2^62
    for (uint32_t i = 0; i < 32; ++i) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

FX_FN uint32_t FxSqrtU32(uint32_t value) {
    uint32_t result = 0;
    uint32_t bit = 0x40000000u;  // 2^30
    for (uint32_t i = 0; i < 16; ++i) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

// --- log2・exp2(ビットごとに決める方法。結果は Q32 = 2^-32 単位)---------------------------------
// 2^(2^-k) を Q63 で(k = 1〜32)。Python の decimal で 80 桁で計算して切り捨てた値
FX_CONST uint64_t FX_EXP2_STEP_Q63[32] = {
    FX_U64(0xB504F333u, 0xF9DE6484u), FX_U64(0x9837F051u, 0x8DB8A96Fu), FX_U64(0x8B95C1E3u, 0xEA8BD6E6u),
    FX_U64(0x85AAC367u, 0xCC487B14u), FX_U64(0x82CD8698u, 0xAC2BA1D7u), FX_U64(0x8164D1F3u, 0xBC030773u),
    FX_U64(0x80B1ED4Fu, 0xD999AB6Cu), FX_U64(0x8058D7D2u, 0xD5E5F6B0u), FX_U64(0x802C6436u, 0xD0E04F50u),
    FX_U64(0x8016302Fu, 0x17467628u), FX_U64(0x800B179Cu, 0x82028FD0u), FX_U64(0x80058BAFu, 0x7FEE3B5Du),
    FX_U64(0x8002C5D0u, 0x0FDCFCB6u), FX_U64(0x800162E6u, 0x1BED4A48u), FX_U64(0x8000B172u, 0x92F702A3u),
    FX_U64(0x800058B9u, 0x2ABBAE02u), FX_U64(0x80002C5Cu, 0x8DADE4D7u), FX_U64(0x8000162Eu, 0x44EAF636u),
    FX_U64(0x80000B17u, 0x21FA7C18u), FX_U64(0x8000058Bu, 0x90DE7E4Cu), FX_U64(0x800002C5u, 0xC8678F36u),
    FX_U64(0x80000162u, 0xE431DB9Fu), FX_U64(0x800000B1u, 0x721872D0u), FX_U64(0x80000058u, 0xB90C1AA8u),
    FX_U64(0x8000002Cu, 0x5C8605A4u), FX_U64(0x80000016u, 0x2E4300E6u), FX_U64(0x8000000Bu, 0x17217FF8u),
    FX_U64(0x80000005u, 0x8B90BFDDu), FX_U64(0x80000002u, 0xC5C85FE6u), FX_U64(0x80000001u, 0x62E42FF1u),
    FX_U64(0x80000000u, 0xB17217F8u), FX_U64(0x80000000u, 0x58B90BFCu)};
FX_CONST uint64_t FX_LN2_Q62 = FX_U64(0x2C5C85FDu, 0xF473DE6Au);    // ln 2
FX_CONST uint64_t FX_LOG2E_Q62 = FX_U64(0x5C551D94u, 0xAE0BF85Du);  // log2 e = 1 / ln 2

// log2(value) を Q32 で(value > 0)。整数部は最上位ビットの位置、小数部は仮数の 2 乗を 32 回くり返して 1 ビットずつ決める
FX_FN int64_t FxLog2U64(uint64_t value) {
    const uint32_t msb = FxMsbU64(value);
    uint64_t mantissa = value << (63 - msb);  // Q63 で [1, 2)
    uint64_t fraction = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        const uint64_t square = FxMulHiU64(mantissa, mantissa);  // Q62 で [1, 4)
        fraction <<= 1;
        if ((square >> 63) != 0) {  // 2 以上: このビットは 1。値を半分にした Q63 としてそのまま使う
            fraction |= 1;
            mantissa = square;
        } else {
            mantissa = square << 1;
        }
    }
    return (int64_t)(((uint64_t)msb << 32) | fraction);
}

// value が Q(fractionBits) の固定小数点のときの log2(Q32)
FX_FN int64_t FxLog2Q(uint64_t value, uint32_t fractionBits) {
    return FxLog2U64(value) - (int64_t)((uint64_t)fractionBits << 32);
}

// 2^x(x は Q32)を Q(outFractionBits) で返す。0 方向の切り捨て。結果が 64bit に収まらなければ桁あふれ
FX_FN uint64_t FxExp2Q32(int64_t x, uint32_t outFractionBits) {
    const int64_t integerPart = x >> 32;  // 算術シフト = 下への切り捨て(小数部を [0, 1) にするため)
    const uint64_t fraction = (uint64_t)x & FX_LOW32_MASK;
    uint64_t result = FX_U64(0x80000000u, 0u);  // 1.0(Q63)
    for (uint32_t k = 0; k < 32; ++k) {
        if (((fraction >> (31 - k)) & 1) != 0) result = FxMulHiU64(result, FX_EXP2_STEP_Q63[k]) << 1;
    }
    // result は 2^小数部 の Q63。2^整数部 と出力の桁に合わせてずらす
    const int64_t shift = integerPart + (int64_t)outFractionBits - 63;
    FX_ASSERT(shift <= 0);
    if (shift > 0) return 0;
    return -shift >= 64 ? (uint64_t)0 : result >> (uint32_t)(-shift);
}

// ln(value)(Q32)と e^x(x は Q32)。log2 / exp2 に定数を掛けて作る
FX_FN int64_t FxLnU64(uint64_t value) {
    return FxMulShiftS64(FxLog2U64(value), (int64_t)FX_LN2_Q62, 62);
}

FX_FN uint64_t FxExpQ32(int64_t x, uint32_t outFractionBits) {
    return FxExp2Q32(FxMulShiftS64(x, (int64_t)FX_LOG2E_Q62, 62), outFractionBits);
}

// --- sin・cos(CORDIC)---------------------------------------------------------------------------
// 角度は「1 周 = 2^32」の符号なし整数(1 周で自然に一周する)。結果は Q30(1.0 = 2^30)
FX_CONST int32_t FX_CORDIC_ATAN_TURN32[30] = {
    536870912, 316933406, 167458907, 85004756, 42667331, 21354465, 10679838, 5340245, 2670163, 1335087,
    667544,    333772,    166886,    83443,    41722,    20861,    10430,    5215,    2608,    1304,
    652,       326,       163,       81,       41,       20,       10,       5,       3,       1};
FX_CONST int32_t FX_CORDIC_GAIN_INVERSE_Q30 = 652032874;  // Π 1/sqrt(1 + 2^-2i)(i = 0〜29)

struct FxSinCos {
    int32_t sine;
    int32_t cosine;
};

FX_FN FxSinCos FxSinCosTurn32(uint32_t angle) {
    // 一番近い 90° の倍数を引き、残り(±45°)を CORDIC で回す
    const uint32_t quadrant = ((angle + 0x20000000u) >> 30) & 3u;
    int32_t remaining = (int32_t)(angle - (quadrant << 30));
    int32_t x = FX_CORDIC_GAIN_INVERSE_Q30;
    int32_t y = 0;
    for (uint32_t i = 0; i < 30; ++i) {
        const int32_t xShifted = x >> i;
        const int32_t yShifted = y >> i;
        if (remaining >= 0) {
            x -= yShifted;
            y += xShifted;
            remaining -= FX_CORDIC_ATAN_TURN32[i];
        } else {
            x += yShifted;
            y -= xShifted;
            remaining += FX_CORDIC_ATAN_TURN32[i];
        }
    }
    FxSinCos result = {y, x};
    if (quadrant == 1) {
        result.sine = x;
        result.cosine = -y;
    }
    if (quadrant == 2) {
        result.sine = -y;
        result.cosine = -x;
    }
    if (quadrant == 3) {
        result.sine = -x;
        result.cosine = y;
    }
    return result;
}

// --- 乱数(カウンタ型。R6: 状態を持たず、入力だけで決まる)---------------------------------------
FX_CONST uint64_t FX_GOLDEN_GAMMA = FX_U64(0x9E3779B9u, 0x7F4A7C15u);

// SplitMix64 の仕上げの混ぜ合わせ
FX_FN uint64_t FxMix64(uint64_t value) {
    value = (value ^ (value >> 30)) * FX_U64(0xBF58476Du, 0x1CE4E5B9u);
    value = (value ^ (value >> 27)) * FX_U64(0x94D049BBu, 0x133111EBu);
    return value ^ (value >> 31);
}

FX_FN uint64_t FxHashCombine(uint64_t hash, uint64_t value) {
    return FxMix64((hash + FX_GOLDEN_GAMMA) ^ value);
}

// (世界のシード, 刻み, セル/物の ID, 用途)から 64bit の乱数。同じ引数なら、どこで何回呼んでも同じ値
FX_FN uint64_t FxHash64(uint64_t seed, uint64_t tick, uint64_t id, uint32_t purpose) {
    uint64_t hash = FxMix64(seed + FX_GOLDEN_GAMMA);
    hash = FxHashCombine(hash, tick);
    hash = FxHashCombine(hash, id);
    return FxHashCombine(hash, (uint64_t)purpose);
}

// 乱数から [0, count) の整数(上位 32bit × count の上位。偏りは 2^-32 以下)
FX_FN uint32_t FxRandomBelow(uint64_t hash, uint32_t count) {
    return (uint32_t)(((hash >> 32) * (uint64_t)count) >> 32);
}

FX_NAMESPACE_END

#endif  // BICAMERAL_FIXED_HLSLI
