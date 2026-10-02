// fixed_bench.hlsl — 整数の数学ライブラリ(fixed.hlsli)のルーチンの費用を測るマイクロベンチ(T-0010、04 §6)。
// 1 つのソースを BENCH_OP(演算の番号)ごとにコンパイルする(shaders/CMakeLists.txt の BICAMERAL_BENCH_OPERATIONS)。
// 各スレッドが「x = 演算(x) → 混ぜる」を反復回数 × BENCH_UNROLL 回、前の結果に依存させて繰り返し、最後の x を書く。
// 時間は tests/gpu_fixed_bench.cpp がタイムスタンプで測り、「混ぜるだけ」(base32 / base64)を引いて演算 1 回の費用にする。
//
// 混ぜる(x ^= x >> k、定数との xor)理由: 前の結果に依存させないと、DXC が足し算・掛け算の連鎖をまとめて消してしまう。
// 割り算の結果は小さくなるので、定数の xor で上位のビットを戻す(64bit の割り算は、上位が 0 だと速い道を通る機種がある)。
// 計測専用なので浮動小数点を使ってよい(shaders/bench/)。シミュのコードからは使わない。
#include "common/fixed.hlsli"
#include "common/physics_math.hlsli"

#ifndef BENCH_OP
#define BENCH_OP 0
#endif
#define BENCH_UNROLL 4

// 反復回数は CPU がルート定数で渡す(定数にするとコンパイラがループを消せてしまう)。結果はスレッドごとに 64bit
RWByteAddressBuffer results : register(u0);
cbuffer BenchConstants : register(b0) {
    uint32_t iterationCount;
};

FX_CONST uint32_t MIX32 = 0x9E3779B9u;
FX_CONST uint64_t MIX64 = FX_U64(0xC2B2AE3Du, 0x27D4EB4Fu);

uint32_t Mix32(uint32_t value) {
    return (value ^ (value >> 13)) ^ MIX32;
}

uint64_t Mix64(uint64_t value) {
    return (value ^ (value >> 29)) ^ MIX64;
}

// --- 演算の番号(CMake の BICAMERAL_BENCH_OPERATIONS と同じ順)--------------------------------------------
// 0 base32 / 1 base64 / 2 add32 / 3 mul32 / 4 div32 / 5 add64 / 6 mul64 / 7 div64 / 8 fadd / 9 fmul / 10 fdiv /
// 11 mulshift32 / 12 mulshift64 / 13 mulfull128 / 14 divs64 / 15 divshift64 / 16 div128 / 17 recip32 / 18 recip64 /
// 19 recips64 / 20 makerecip64 / 21 sqrt32 / 22 sqrt64 / 23 exp2 / 24 log2 / 25 exp / 26 ln / 27 sincos / 28 hash64 /
// 29 solve6(物理の 6×6 の連立方程式 PxSolveSymmetric6。T-0093)
#if BENCH_OP == 0 || (BENCH_OP >= 2 && BENCH_OP <= 4) || BENCH_OP == 11 || BENCH_OP == 17 || BENCH_OP == 21 || \
    BENCH_OP == 27
#define BENCH_WIDTH 32
typedef uint32_t Value;
#elif BENCH_OP >= 8 && BENCH_OP <= 10
#define BENCH_WIDTH 0  // 浮動小数点(混ぜない)
typedef float Value;
#else
#define BENCH_WIDTH 64
typedef uint64_t Value;
#endif

struct Operand {
    Value y;
    uint64_t divisor;
    FxRecip32 recip32;
    FxRecip64 recip64;
    FxRecipS64 recipSigned;
};

Operand MakeOperand(uint64_t seed) {
    Operand operand;
    const uint64_t random = FxHash64(seed, 0, 1, 0);
#if BENCH_WIDTH == 0
    // 実行時の値にする(定数だと、ドライバが定数での割り算を逆数の掛け算に変え、連鎖をまとめてしまう)
    operand.y = 1.0f + (float)(uint32_t)(random & 0xFFu) * (1.0f / 65536.0f);
#elif BENCH_WIDTH == 32
    operand.y = (uint32_t)random | 1u;
#else
    operand.y = random | 1;
#endif
    // 除数: 大きめの値(64bit の割り算が速い道を通らないように上位にもビットを立てる)
    operand.divisor = (random >> 20) | FX_U64(0x00000100u, 0u) | 1;
    operand.recip32 = FxMakeRecipU32((uint32_t)random | 0x10001u);
    operand.recip64 = FxMakeRecipU64(operand.divisor);
    operand.recipSigned = FxMakeRecipS64(-(int64_t)operand.divisor);

    return operand;
}

#if BENCH_OP == 29
// x から対称正定値の 6×6(対角が優位)と右辺を作って解き、解を 1 つの値に畳む。
// 対角は 2^40 前後・非対角は ±2^15 で、物理の H(硬さと質量の項)と同じく尺度合わせ → Cholesky → 代入を全部通る
uint64_t Solve6(uint64_t x) {
    PxMat6 a;
    PxVec6 g;
    for (uint32_t i = 0; i < 6; ++i) {
        for (uint32_t j = 0; j < 6; ++j) {
            const uint32_t low = min(i, j);
            const uint32_t high = max(i, j);
            const int64_t offDiagonal = (int64_t)((x >> (low * 6 + high)) & 0xFFFFu) - 0x8000;
            a.m[i * 6 + j] = i == j ? ((int64_t)1 << 40) + (int64_t)((x >> (i * 4)) & 0xFFFFFu) : offDiagonal;
        }

        g.v[i] = (int64_t)((x >> (i * 8)) & 0xFFFFFFFu) - 0x8000000;
    }

    const PxSolveResult solved = PxSolveSymmetric6(a, g, 24);
    uint64_t folded = 0;
    for (uint32_t k = 0; k < 6; ++k)
        folded ^= (uint64_t)solved.x.v[k] << (k * 7);

    return folded;
}
#endif

// 演算 1 回(混ぜる前)
Value Operate(Value x, Operand operand) {
#if BENCH_OP == 0 || BENCH_OP == 1
    return x;
#elif BENCH_OP == 2 || BENCH_OP == 5 || BENCH_OP == 8
    return x + operand.y;
#elif BENCH_OP == 3 || BENCH_OP == 6 || BENCH_OP == 9
    return x * operand.y;
#elif BENCH_OP == 4
    return (uint32_t)operand.divisor / (x | 1u);  // 除数を毎回変える(ループの外で逆数を作られないように)
#elif BENCH_OP == 7
    return operand.divisor / (x | 1);
#elif BENCH_OP == 10
    return operand.y / x;  // x と y / x を行き来するので値が発散しない
#elif BENCH_OP == 11
    return (uint32_t)FxMulShiftS32((int32_t)x, (int32_t)operand.y, 16);
#elif BENCH_OP == 12
    return (uint64_t)FxMulShiftS64((int64_t)x, (int64_t)(operand.y >> 24), 32);
#elif BENCH_OP == 13
    const FxU128 product = FxMulU64Full(x, operand.y);
    return product.hi ^ product.lo;
#elif BENCH_OP == 14
    return (uint64_t)FxDivS64((int64_t)operand.divisor, (int64_t)(x | 1));
#elif BENCH_OP == 15
    // 除数を毎回変える(T-0084 から、除数が同じだと逆数を作る所がループの外へ出されてしまう)
    return (uint64_t)FxDivShiftS64((int64_t)(operand.divisor >> 8), (int64_t)((x >> 1) | FX_U64(0x100u, 0u)), 16);
#elif BENCH_OP == 16
    const FxU128 numerator = {x >> 40, operand.y};  // 上位 < 2^24 < 除数(除数は 2^40 以上で毎回変わる)
    return FxDivU128By64(numerator, x | FX_U64(0x100u, 0u)).quotient;
#elif BENCH_OP == 17
    return FxDivRecipU32(x, operand.recip32);
#elif BENCH_OP == 18
    return FxDivRecipU64(x, operand.recip64);
#elif BENCH_OP == 19
    return (uint64_t)FxDivRecipS64((int64_t)x, operand.recipSigned);
#elif BENCH_OP == 20
    return FxMakeRecipU64(x | 1).multiplier;
#elif BENCH_OP == 21
    return FxSqrtU32(x);
#elif BENCH_OP == 22
    return FxSqrtU64(x);
#elif BENCH_OP == 23
    return FxExp2Q32((int64_t)(x >> 27) - (int64_t)FX_U64(0x10u, 0u), 31);  // x は ±16(Q32)
#elif BENCH_OP == 24
    return (uint64_t)FxLog2U64(x | 1);
#elif BENCH_OP == 25
    return FxExpQ32((int64_t)(x >> 28) - (int64_t)FX_U64(0x8u, 0u), 40);  // x は ±8(Q32)
#elif BENCH_OP == 26
    return (uint64_t)FxLnU64(x | 1);
#elif BENCH_OP == 27
    const FxSinCos sinCos = FxSinCosTurn32(x);
    return (uint32_t)sinCos.sine ^ (uint32_t)sinCos.cosine;
#elif BENCH_OP == 28
    return FxHash64(x, operand.y, 1, 2);
#elif BENCH_OP == 29
    return Solve6(x ^ operand.y);
#endif
}

Value Step(Value x, Operand operand) {
#if BENCH_WIDTH == 0
    return Operate(x, operand);
#elif BENCH_WIDTH == 32
    return Mix32(Operate(x, operand));
#else
    return Mix64(Operate(x, operand));
#endif
}

uint64_t ToBits(Value x) {
#if BENCH_WIDTH == 0
    return (uint64_t)asuint(x);
#else
    return (uint64_t)x;
#endif
}

[numthreads(256, 1, 1)] void Main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint64_t seed = FxHash64(0x54303130u, 0, dispatchThreadId.x, 0);
    const Operand operand = MakeOperand(seed);
#if BENCH_WIDTH == 0
    Value x = 1.0f + (float)(dispatchThreadId.x & 7) * 0.125f;
#else
    Value x = (Value)seed;
#endif
    for (uint32_t i = 0; i < iterationCount; ++i)
        [unroll] for (uint32_t j = 0; j < BENCH_UNROLL; ++j) x = Step(x, operand);

    results.Store<uint64_t>(dispatchThreadId.x * 8, ToBits(x));
}
