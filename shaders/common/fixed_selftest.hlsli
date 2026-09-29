// fixed_selftest.hlsli — fixed.hlsli の全部の関数を 1 つの入力 (a, b) で呼び、結果を並べる(T-0010)。
// GPU(shaders/sim/fixed_selftest.hlsl)と CPU(tests/fixed_test.cpp)が同じこの関数を通すので、
// 同じ入力の列から同じ結果の列が出ればビット一致(D-307)。GPU で走らせて比べるのは T-0013。
// 各関数の前提(桁あふれしない範囲)は、a と b から作る引数の側で守る。
#ifndef BICAMERAL_FIXED_SELFTEST_HLSLI
#define BICAMERAL_FIXED_SELFTEST_HLSLI

#include "fixed.hlsli"

FX_NAMESPACE_BEGIN

FX_CONST uint32_t FX_SELF_TEST_OUTPUT_COUNT = 18;

struct FxSelfTestOutput {
    uint64_t values[FX_SELF_TEST_OUTPUT_COUNT];
};

// 自己テストの入力 (a, b) を番号から作る。GPU も CPU も同じ番号の列から同じ入力を得る(入力のバッファが要らない)。
// いろいろな桁を試すため、乱数を 0〜63 ビット右へずらす
FX_FN FxU128 FxSelfTestInputs(uint32_t caseIndex) {
    const uint64_t seed = 0x54303130u;  // "T010"
    const uint64_t shifts = FxHash64(seed, 0, caseIndex, 2);
    FxU128 inputs = {FxHash64(seed, 0, caseIndex, 0) >> (shifts & 63),
                     FxHash64(seed, 0, caseIndex, 1) >> ((shifts >> 6) & 63)};
    return inputs;
}

FX_FN FxSelfTestOutput FxSelfTestCase(uint64_t a, uint64_t b) {
    FxSelfTestOutput output;
    const int64_t signedA = (int64_t)a;
    const int64_t signedB = (int64_t)b;

    // --- 積 ---
    const FxU128 product = FxMulU64Full(a, b);
    const FxU128 signedProduct = FxMulS64Full(signedA, signedB);
    output.values[0] = product.hi;
    output.values[1] = product.lo;
    output.values[2] = signedProduct.hi;
    output.values[3] = signedProduct.lo;
    output.values[4] = (uint64_t)FxMulShiftS64(signedA >> 20, signedB >> 20, 30);  // |積| < 2^86 → 結果 < 2^56

    // --- 割り算 ---
    const FxU128 numerator = {b >> 1, a};
    const FxDivResult division = FxDivU128By64(numerator, a | FX_U64(0x80000000u, 0u));  // 上位 < 除数
    output.values[5] = division.quotient;
    output.values[6] = division.remainder;
    const int64_t dividend = (int64_t)(a >> 33) * (((b >> 63) != 0) ? -1 : 1);
    output.values[7] = (uint64_t)FxDivShiftS64(dividend, (int64_t)((b >> 34) | 2), 16);
    output.values[8] = (uint64_t)FxDivS64(signedA, (int64_t)((b >> 1) | 1));  // 除数を正にして商の桁あふれを避ける

    // --- 平方根・log・exp ---
    output.values[9] = FxSqrtU64(a);
    output.values[10] = FxSqrtU32((uint32_t)a);
    output.values[11] = (uint64_t)FxLog2U64(a | 1);
    output.values[12] = FxExp2Q32(signedA >> 26, 31);  // x は ±32 の Q32 → 2^x × 2^31 < 2^64
    output.values[13] = (uint64_t)FxLnU64(a | 1);
    output.values[14] = FxExpQ32(signedA >> 27, 40);  // x は ±16 → e^16 × 2^40 < 2^64

    // --- sin・cos・乱数・部品 ---
    const FxSinCos sinCos = FxSinCosTurn32((uint32_t)a);
    output.values[15] = ((uint64_t)(uint32_t)sinCos.sine << 32) | (uint64_t)(uint32_t)sinCos.cosine;
    output.values[16] = FxHash64(a, b, a ^ b, (uint32_t)b);
    output.values[17] = FxMsbU64(a | 1);
    return output;
}

FX_NAMESPACE_END

#endif  // BICAMERAL_FIXED_SELFTEST_HLSLI
