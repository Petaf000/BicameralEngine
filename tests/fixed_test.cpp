// fixed_test.cpp — shaders/common/fixed.hlsli(整数の数学ライブラリ)の単体テスト(T-0010)。CPU だけで走る。
// 積と割り算は MSVC の 128bit 組み込み関数とビット一致、平方根は定義どおり、exp・log・sin・cos は double との誤差の最大値を測る。
// 誤差の最大値は毎回表示する(精度の仕様: docs/design/04-numerics-determinism.md §3・「研究」)。
// double を使うのはテストの参照値だけ。シミュのコードは浮動小数点を使わない(R1)。
#include "common/fixed.hlsli"
#include "common/fixed_selftest.hlsli"
#include "common/units.hlsli"

#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace bicameral::fx;

// --- コンパイル時に確かめられるもの(C++ では関数が constexpr)------------------------------------
static_assert(FxMulU64Full(~0ull, ~0ull).hi == 0xFFFFFFFFFFFFFFFEull);
static_assert(FxMulU64Full(~0ull, ~0ull).lo == 1);
static_assert(FxSqrtU64(144) == 12 && FxSqrtU64(~0ull) == 0xFFFFFFFFu && FxSqrtU32(~0u) == 0xFFFFu);
static_assert(FxLog2U64(1) == 0 && FxLog2U64(1ull << 40) == (40ll << 32));
static_assert(FxExp2Q32(0, 32) == (1ull << 32) && FxExp2Q32(5ll << 32, 0) == 32);
static_assert(FxMix64(FX_GOLDEN_GAMMA) == 0xE220A8397B1DCDAFull);  // SplitMix64(シード 0)の最初の出力
static_assert(FxMix64(FX_GOLDEN_GAMMA * 2) == 0x6E789E6AA1B965F4ull);
static_assert(FxMulShiftS64(-3, 5, 1) == -7 && FxMulShiftS64(3, 5, 1) == 7);  // 0 方向の切り捨て
static_assert(FxDivShiftS64(-7, 2, 0) == -3 && FxDivS64(-7, 2) == -3);
static_assert(FxDivRecipU64(100, FxMakeRecipU64(7)) == 14 && FxDivRecipU32(100, FxMakeRecipU32(7)) == 14);
static_assert(FxDivRecipS64(-7, FxMakeRecipS64(2)) == -3 && FxDivRecipS64(7, FxMakeRecipS64(-2)) == -3);

namespace {

    int failureCount = 0;

    // 128÷64 の割り算の参照値。MSVC は組み込み関数 _udiv128 を使う。clang(CI の clang-tidy が解析するとき)は
    // unsigned __int128 で求める。clang の intrin.h には _udiv128 が無い版がある(pip の clang-tidy 22.1.8。2026-09-30)
    uint64_t ReferenceDivide128(uint64_t high, uint64_t low, uint64_t divisor, uint64_t* remainder) {
#if defined(__clang__)
        const unsigned __int128 numerator = (static_cast<unsigned __int128>(high) << 64) | low;
        *remainder = static_cast<uint64_t>(numerator % divisor);
        return static_cast<uint64_t>(numerator / divisor);
#else
        return _udiv128(high, low, divisor, remainder);
#endif
    }

    void Expect(bool condition, const char* text, int line) {
        if (condition) return;
        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

    constexpr int RANDOM_CASES = 200000;
    constexpr uint32_t FIXED_SELF_TEST_CASES = 65536;  // GPU で 64 スレッド × 1024 グループ
    constexpr double TWO_POW_32 = 4294967296.0;
    constexpr double TWO_POW_30 = 1073741824.0;
    constexpr double PI = 3.14159265358979323846;

    // 乱数の値をいろいろな桁に散らす(小さい値・大きい値の両方を試す)
    uint64_t SpreadBits(std::mt19937_64& random) {
        const uint64_t value = random();
        return value >> (random() % 64);
    }

}  // namespace

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

// --- 積 ---------------------------------------------------------------------------------------------
static void TestMultiply(std::mt19937_64& random) {
    for (int i = 0; i < RANDOM_CASES; ++i) {
        const uint64_t a = SpreadBits(random);
        const uint64_t b = SpreadBits(random);
        uint64_t expectedHigh = 0;
        const uint64_t expectedLow = _umul128(a, b, &expectedHigh);
        const FxU128 product = FxMulU64Full(a, b);
        EXPECT(product.hi == expectedHigh && product.lo == expectedLow);

        const auto signedA = static_cast<int64_t>(random());
        const auto signedB = static_cast<int64_t>(SpreadBits(random)) * ((random() & 1) != 0 ? -1 : 1);
        int64_t expectedSignedHigh = 0;
        const int64_t expectedSignedLow = _mul128(signedA, signedB, &expectedSignedHigh);
        const FxU128 signedProduct = FxMulS64Full(signedA, signedB);
        EXPECT(signedProduct.hi == static_cast<uint64_t>(expectedSignedHigh) &&
               signedProduct.lo == static_cast<uint64_t>(expectedSignedLow));
    }
    // Q 形式の積: 符号を反転すると結果の符号だけが反転する(0 方向の切り捨ての対称性)
    for (int i = 0; i < RANDOM_CASES; ++i) {
        const auto a = static_cast<int64_t>(random() >> 20);
        const auto b = static_cast<int64_t>(random() >> 20);
        const uint32_t shift = 26 + static_cast<uint32_t>(random() % 38);  // 積 < 2^88 なので結果が 63bit に収まる範囲
        const int64_t positive = FxMulShiftS64(a, b, shift);
        EXPECT(FxMulShiftS64(-a, b, shift) == -positive && FxMulShiftS64(a, -b, shift) == -positive);
        uint64_t high = 0;
        const uint64_t low = _umul128(static_cast<uint64_t>(a), static_cast<uint64_t>(b), &high);
        EXPECT(static_cast<uint64_t>(positive) == __shiftright128(low, high, static_cast<uint8_t>(shift)));
    }
    EXPECT(FxMulShiftS32(-65536, 98304, 16) == -98304);  // -1.0 × 1.5(Q16.16)
    EXPECT(FxMulShiftS32(-1, 1, 1) == 0);                // -0.5 は 0 へ
}

// --- 割り算 -----------------------------------------------------------------------------------------
static void TestDivide(std::mt19937_64& random) {
    for (int i = 0; i < RANDOM_CASES; ++i) {
        const uint64_t divisor = SpreadBits(random) | 1;
        const FxU128 numerator = {.hi = random() % divisor, .lo = random()};
        uint64_t expectedRemainder = 0;
        const uint64_t expectedQuotient = ReferenceDivide128(numerator.hi, numerator.lo, divisor, &expectedRemainder);
        const FxDivResult result = FxDivU128By64(numerator, divisor);
        EXPECT(result.quotient == expectedQuotient && result.remainder == expectedRemainder);

        const auto a = static_cast<int64_t>(SpreadBits(random) >> 1) * ((random() & 1) != 0 ? -1 : 1);
        const auto b = static_cast<int64_t>(SpreadBits(random) >> 1 | 1) * ((random() & 1) != 0 ? -1 : 1);
        EXPECT(FxDivS64(a, b) == a / b);  // C++ の / も 0 方向の切り捨て
    }
    // Q 形式の割り算: 商が収まる範囲で、(a × 2^shift) / b の切り捨てと一致
    for (int i = 0; i < RANDOM_CASES; ++i) {
        const auto shift = static_cast<uint32_t>(random() % 33);
        const auto a = static_cast<int64_t>(random() >> 32);
        const auto b = static_cast<int64_t>((random() >> 34) | 2);  // 2 以上なら商が 63bit に収まる
        const bool negative = (random() & 1) != 0;
        uint64_t remainder = 0;
        const uint64_t high = shift == 0 ? 0 : static_cast<uint64_t>(a) >> (64 - shift);
        const uint64_t expected =
            ReferenceDivide128(high, static_cast<uint64_t>(a) << shift, static_cast<uint64_t>(b), &remainder);
        const int64_t result = FxDivShiftS64(negative ? -a : a, b, shift);
        EXPECT(result == (negative ? -static_cast<int64_t>(expected) : static_cast<int64_t>(expected)));
    }
}

// --- 平方根 -----------------------------------------------------------------------------------------
static bool IsFloorSqrt64(uint64_t value, uint64_t root) {
    uint64_t high = 0;
    const uint64_t low = _umul128(root + 1, root + 1, &high);
    const bool nextIsLarger = high != 0 || low > value;
    return root * root <= value && nextIsLarger;
}

static void TestSquareRoot(std::mt19937_64& random) {
    for (uint32_t value = 0; value < (1u << 22); ++value) {
        const uint32_t root = FxSqrtU32(value);
        EXPECT(root * root <= value && (root + 1) * (root + 1) > value);
        if (failureCount > 10) return;
    }
    for (int i = 0; i < RANDOM_CASES; ++i) {
        const uint64_t value = SpreadBits(random);
        EXPECT(IsFloorSqrt64(value, FxSqrtU64(value)));
        const auto value32 = static_cast<uint32_t>(random());
        EXPECT(FxSqrtU32(value32) == FxSqrtU64(value32));
    }
}

// --- log2・exp2・ln・exp ---------------------------------------------------------------------------
static void TestLogExp(std::mt19937_64& random) {
    double maxLog2Error = 0;  // Q32 の単位(2^-32)
    double maxLnError = 0;
    for (int i = 0; i < RANDOM_CASES; ++i) {
        const uint64_t value = SpreadBits(random) | 1;
        const double exactLog2 = std::log2(static_cast<double>(value));
        maxLog2Error =
            std::fmax(maxLog2Error, std::fabs(static_cast<double>(FxLog2U64(value)) - exactLog2 * TWO_POW_32));
        maxLnError = std::fmax(maxLnError, std::fabs(static_cast<double>(FxLnU64(value)) -
                                                     std::log(static_cast<double>(value)) * TWO_POW_32));
    }

    double maxExp2Relative = 0;
    double maxExpRelative = 0;
    for (int i = 0; i < RANDOM_CASES; ++i) {
        // x を [-40, 40) の Q32 に。出力の桁は、結果が 2^59〜2^63 に入るように x ごとに選ぶ(最後の切り捨ての誤差を測らないため)
        const auto x = static_cast<int64_t>(random() % (80ull << 32)) - (40ll << 32);
        const double xReal = static_cast<double>(x) / TWO_POW_32;
        const auto outBits2 = static_cast<uint32_t>(61 - static_cast<int>(std::floor(xReal)));
        const double exact2 = std::exp2(xReal + outBits2);
        maxExp2Relative =
            std::fmax(maxExp2Relative, std::fabs(static_cast<double>(FxExp2Q32(x, outBits2)) - exact2) / exact2);
        const int64_t xNatural = x / 2;
        const double xNaturalReal = static_cast<double>(xNatural) / TWO_POW_32;
        const auto outBitsE =
            static_cast<uint32_t>(61 - static_cast<int>(std::floor(xNaturalReal * 1.4426950408889634)));
        const double exactE = std::exp(xNaturalReal) * std::exp2(static_cast<double>(outBitsE));
        maxExpRelative =
            std::fmax(maxExpRelative, std::fabs(static_cast<double>(FxExpQ32(xNatural, outBitsE)) - exactE) / exactE);
    }
    std::printf("log2: max error %.2f (2^-32)  ln: max error %.2f (2^-32)\n", maxLog2Error, maxLnError);
    std::printf("exp2: max relative error %.3e  exp: max relative error %.3e\n", maxExp2Relative, maxExpRelative);
    EXPECT(maxLog2Error <= 4);         // 2^-30 以内(2026-09-30 の実測 1.0)
    EXPECT(maxLnError <= 4);           // 実測 1.7
    EXPECT(maxExp2Relative <= 1e-14);  // 04「研究」の目標 1e-4 に対して十分な余裕
    EXPECT(maxExpRelative <= 1e-9);    // x × log2 e の Q32 への切り捨て(2^-32)が効く
    EXPECT(FxLog2Q(3ull << 16, 16) == FxLog2U64(3));
    EXPECT(FxExp2Q32(-(64ll << 32), 0) == 0);  // 小さすぎる値は 0 へ切り捨て
}

// --- sin・cos ---------------------------------------------------------------------------------------
static void TestSinCos(std::mt19937_64& random) {
    double maxError = 0;  // Q30 の単位
    for (int i = 0; i < RANDOM_CASES; ++i) {
        const auto angle = static_cast<uint32_t>(random());
        const double radians = static_cast<double>(angle) / TWO_POW_32 * 2 * PI;
        const FxSinCos result = FxSinCosTurn32(angle);
        maxError = std::fmax(maxError, std::fabs(result.sine - std::sin(radians) * TWO_POW_30));
        maxError = std::fmax(maxError, std::fabs(result.cosine - std::cos(radians) * TWO_POW_30));
    }
    std::printf("sin/cos: max error %.2f (2^-30)\n", maxError);
    EXPECT(maxError <= 32);  // 3e-8 以内(実測 17.6)
    // 90° ごとの点
    const FxSinCos right = FxSinCosTurn32(1u << 30);
    EXPECT(std::abs(right.sine - ROTATION_ONE) <= 64 && std::abs(right.cosine) <= 64);
    const FxSinCos half = FxSinCosTurn32(1u << 31);
    EXPECT(std::abs(half.sine) <= 64 && std::abs(half.cosine + ROTATION_ONE) <= 64);
}

// --- 乱数 -------------------------------------------------------------------------------------------
static void TestHash(std::mt19937_64& random) {
    // 雪崩: 入力の 1 ビットを変えると、出力のおよそ半分(32 ビット)が変わる
    double flippedBits = 0;
    int samples = 0;
    for (int i = 0; i < 20000; ++i) {
        const uint64_t seed = random();
        const uint64_t tick = random() % 100000;
        const uint64_t id = random();
        const uint64_t base = FxHash64(seed, tick, id, 7);
        const auto bit = static_cast<uint32_t>(random() % 64);
        flippedBits += static_cast<double>(__popcnt64(base ^ FxHash64(seed, tick, id ^ (1ull << bit), 7)));
        flippedBits += static_cast<double>(__popcnt64(base ^ FxHash64(seed, tick ^ (1ull << (bit % 20)), id, 7)));
        samples += 2;
    }
    const double average = flippedBits / samples;
    std::printf("hash: average flipped bits %.3f / 64\n", average);
    EXPECT(average > 31.5 && average < 32.5);
    EXPECT(FxHash64(1, 2, 3, 4) != FxHash64(1, 2, 3, 5) && FxHash64(1, 2, 3, 4) == FxHash64(1, 2, 3, 4));

    // [0, 10) がほぼ一様(カイ 2 乗、自由度 9 の 99.9% 点は 27.9)
    constexpr int BUCKETS = 10;
    constexpr int DRAWS = 1000000;
    int counts[BUCKETS] = {};
    for (int i = 0; i < DRAWS; ++i) {
        ++counts[FxRandomBelow(FxHash64(42, 0, static_cast<uint64_t>(i), 0), BUCKETS)];
    }
    double chiSquare = 0;
    for (const int count : counts) {
        const double difference = count - DRAWS / static_cast<double>(BUCKETS);
        chiSquare += difference * difference / (DRAWS / static_cast<double>(BUCKETS));
    }
    std::printf("hash: chi-square %.2f (df 9)\n", chiSquare);
    EXPECT(chiSquare < 27.9);
}

// --- 逆数の掛け算(ふつうの割り算とビット一致)----------------------------------------------------------
// 境目になりやすい除数(1・2 のべき乗とその前後・最大値)と、それぞれの境目の被除数(0・d − 1・d・d + 1・最大値)を全部試し、
// 残りは乱数で試す
static void CheckRecipU64(uint64_t numerator, uint64_t divisor) {
    EXPECT(FxDivRecipU64(numerator, FxMakeRecipU64(divisor)) == numerator / divisor);
}

static void CheckRecipU32(uint32_t numerator, uint32_t divisor) {
    EXPECT(FxDivRecipU32(numerator, FxMakeRecipU32(divisor)) == numerator / divisor);
}

// 1 つの除数について、境目の被除数をすべて試す
static void CheckRecipEdges(uint64_t divisor) {
    const bool fits32 = divisor <= 0xFFFFFFFFull;
    for (const uint64_t numerator : {0ull, 1ull, divisor - 1, divisor, divisor + 1, ~0ull, ~0ull - 1}) {
        CheckRecipU64(numerator, divisor);
        if (fits32) {
            CheckRecipU32(static_cast<uint32_t>(numerator), static_cast<uint32_t>(divisor));
        }
    }
}

static void TestReciprocalEdges() {
    for (uint32_t bit = 0; bit < 64; ++bit) {
        const uint64_t power = 1ull << bit;
        for (const uint64_t divisor : {power - 1, power, power + 1, ~0ull >> (63 - bit)}) {
            if (divisor != 0) CheckRecipEdges(divisor);
        }
    }
    for (uint32_t divisor = 1; divisor <= 4096; ++divisor) {
        for (const uint32_t numerator : {0u, divisor - 1, divisor, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x80000000u}) {
            CheckRecipU32(numerator, divisor);
        }
    }
    EXPECT(FxDivRecipS64(INT64_MIN, FxMakeRecipS64(INT64_MIN)) == 1);
    EXPECT(FxDivRecipS64(INT64_MAX, FxMakeRecipS64(INT64_MIN)) == 0);
    EXPECT(FxDivRecipS64(INT64_MIN, FxMakeRecipS64(3)) == INT64_MIN / 3);
}

static void TestReciprocalRandom(std::mt19937_64& random) {
    for (int i = 0; i < RANDOM_CASES; ++i) {
        const uint64_t divisor = SpreadBits(random) | 1;
        const uint64_t numerator = SpreadBits(random);
        CheckRecipU64(numerator, divisor);
        CheckRecipU64(numerator, divisor + 1);  // 偶数の除数も
        CheckRecipU32(static_cast<uint32_t>(numerator), static_cast<uint32_t>(divisor));
        const auto signedNumerator = static_cast<int64_t>(SpreadBits(random) >> 1) * ((random() & 1) != 0 ? -1 : 1);
        const auto signedDivisor = static_cast<int64_t>(SpreadBits(random) >> 1 | 1) * ((random() & 1) != 0 ? -1 : 1);
        EXPECT(FxDivRecipS64(signedNumerator, FxMakeRecipS64(signedDivisor)) ==
               FxDivS64(signedNumerator, signedDivisor));
    }
}

// --- 自己テスト(GPU と比べる列)---------------------------------------------------------------------
// shaders/sim/fixed_selftest.hlsl と同じ関数を同じ番号の列で呼び、結果の要約を表示する。T-0013 で GPU の要約と比べる
static void TestSelfTestDigest() {
    uint64_t digest = 0;
    for (uint32_t caseIndex = 0; caseIndex < FIXED_SELF_TEST_CASES; ++caseIndex) {
        const FxU128 inputs = FxSelfTestInputs(caseIndex);
        const FxSelfTestOutput output = FxSelfTestCase(inputs.hi, inputs.lo);
        for (const uint64_t value : output.values) {
            digest = FxHashCombine(digest, value);
        }
    }
    std::printf("selftest: %u cases, digest %016llx\n", FIXED_SELF_TEST_CASES, static_cast<unsigned long long>(digest));
    EXPECT(FxSelfTestCase(3, 5).values[1] == 15 && FxSelfTestCase(144, 0).values[9] == 12);
}

// --- 小さな部品 -------------------------------------------------------------------------------------
static void TestHelpers() {
    for (uint32_t bit = 0; bit < 64; ++bit) {
        EXPECT(FxMsbU64(1ull << bit) == bit);
        EXPECT(FxMsbU64(~0ull >> (63 - bit)) == bit);
    }
    EXPECT(FxAbsU64(INT64_MIN) == (1ull << 63));
    EXPECT(FxAddOverflowsS64(INT64_MAX, 1) && FxAddOverflowsS64(INT64_MIN, -1));
    EXPECT(!FxAddOverflowsS64(INT64_MAX, -1) && !FxAddOverflowsS64(-5, 3));
}

int main() {
    std::mt19937_64 random(20260930);  // 毎回同じ入力(失敗を再現できるように)
    TestHelpers();
    TestMultiply(random);
    TestDivide(random);
    TestReciprocalEdges();
    TestReciprocalRandom(random);
    TestSquareRoot(random);
    TestLogExp(random);
    TestSinCos(random);
    TestHash(random);
    TestSelfTestDigest();
    if (failureCount != 0) {
        std::printf("fixed_test: %d failure(s)\n", failureCount);
        return 1;
    }
    std::printf("fixed_test: OK\n");
    return 0;
}
