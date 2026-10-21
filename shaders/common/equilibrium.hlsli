// equilibrium.hlsli — 釣り合う家族の共通部品(docs/design/20-transport-substrate.md §9・§13.1。T-0853・ADR-0397)の式。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU の回し方は engine/src/sim/equilibrium.cpp、GPU の回し方は
// engine/src/sim/gpu_equilibrium.cpp と shaders/sim/equilibrium.hlsl・equilibrium_core.hlsli(T-0915)。どちらもこの関数を呼ぶ(1 つの源。20 §2.7)。
//
// 解く式(段 0 の節 i。x は法則の値〔圧力・温度・電位など。レベルによらない強さの量〕):
//   (C_i + Σ g_f + Σ g_固定) x_i − Σ g_f x_j = Σ 源の項 + Σ g_固定 × x_固定
// 右辺は「対角 D_i で割った値」で持つ(多重格子の重みの形。implicit_conduction.hlsli と同じ)。源の項の割り方は 2 つ:
//   - そのまま(EQ_ROUNDING_EXACT): 値 × 係数 ÷ D を 128bit の積から 1 回の割り算で(気体の陰の圧力の右辺。T-0845 と同じ)
//   - 重み(EQ_ROUNDING_WEIGHT): (係数 ÷ D の Q48 の重み)× 値(熱の陰解法の右辺・値を固定した面。ImEdgeRhsTerm と同じ)
// 面の値(面の形の出力)は F = g (x_a − x_b)(a = 細かい側。ImFaceFlow と同じ)。当てる段が両側に足すので、解けきらなくても保存は厳密(20 §9.1)。
// 閉じた領域(20 §9.3)の距離は位置の単位(2^-20 m)の箱どうしの Chebyshev の隙間。長さは法則が物理の単位(mm)で渡す。
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_EQUILIBRIUM_HLSLI
#define BICAMERAL_EQUILIBRIUM_HLSLI

#include "implicit_conduction.hlsli"
#include "units.hlsli"

MR_NAMESPACE_BEGIN

// --- 定数 --------------------------------------------------------------------------------------
FX_CONST uint32_t EQ_ROUNDING_EXACT = 0;   // 値 × 係数 ÷ D(1 回の割り算)
FX_CONST uint32_t EQ_ROUNDING_WEIGHT = 1;  // (係数 ÷ D の重み)× 値
FX_CONST uint32_t EQ_RESIDUAL_SHIFT = 48;  // 残差 × D の Q(式の単位へ。気体の mg と同じ桁)
FX_CONST uint64_t EQ_MILLIMETERS_PER_METER = 1000;

// --- 右辺の項 ---------------------------------------------------------------------------------

// 128bit ÷ 128bit(商は 64bit に入ること)。分母が 64bit に入れば厳密、超える時は両方の下位を落とす(ImWideRatio と同じ考え)
FX_FN uint64_t EqDivWide(FxU128 numerator, FxU128 denominator) {
    FX_ASSERT(!ImWideIsZero(denominator));
    if (denominator.hi == 0) {
        FX_ASSERT(numerator.hi < denominator.lo);
        return FxDivU128By64(numerator, denominator.lo).quotient;
    }

    const uint32_t drop = ImWideMsb(denominator) - 63;

    return FxDivU128By64(ImWideShiftRight(numerator, drop), ImWideShiftRight(denominator, drop).lo).quotient;
}

// そのまま: 値 × 係数 ÷ D(0 方向へ切り捨て。係数は 64bit)
FX_FN int64_t EqExactTerm(int64_t value, uint64_t factor, FxU128 diagonal) {
    const uint64_t magnitude = EqDivWide(FxMulU64Full(FxAbsU64(value), factor), diagonal);

    return FxApplySign(magnitude, value < 0);
}

// 重み: (係数 ÷ D の Q48)× 値
FX_FN int64_t EqWeightTerm(int64_t value, FxU128 factor, FxU128 diagonal) {
    return ImMulWeight(ImWeight(factor, diagonal), value);
}

FX_FN int64_t EqSourceTerm(uint32_t rounding, int64_t value, FxU128 factor, FxU128 diagonal) {
    if (rounding == EQ_ROUNDING_EXACT) {
        FX_ASSERT(factor.hi == 0);
        return EqExactTerm(value, factor.lo, diagonal);
    }

    return EqWeightTerm(value, factor, diagonal);
}

// --- 面の値と残差 -----------------------------------------------------------------------------

// 面の値 F = g (x_a − x_b)(g は a の単位。a → b が正)
FX_FN int64_t EqFaceValue(FxU128 coefficient, int64_t valueA, int64_t valueB) {
    return ImFaceFlow(coefficient, valueA - valueB);
}

// 残差(値の単位)を式の単位へ: |残差| × D ÷ 2^48(残差の計器。気体なら刻みの終わりの質量のずれ mg)
FX_FN uint64_t EqResidualMagnitude(int64_t residual, FxU128 diagonal) {
    const FxU128 product = ImWideMul(diagonal, FxAbsU64(residual));
    const FxU128 shifted = ImWideShiftRight(product, EQ_RESIDUAL_SHIFT);
    FX_ASSERT(shifted.hi == 0);

    return shifted.lo;
}

// --- GPU の作業用の 128bit の語の並び(T-0915。uint64 の 2 語で 128bit、上・下の順)-------------------
// [容量 × 節][対角 × 節][面の係数 × 隣][残差の計器の最大 1 語][残差の計器 × 段 0 の節]。節・隣の数は GpuImplicit の上限
// (作業場の値 A・B・右辺の番地と同じ数え方)。法則の段(陰の圧力なら GasProjectCell)が段 0 の容量と隣の係数を書き、
// 部品の段(shaders/sim/equilibrium.hlsl・equilibrium_core.hlsli)が対角・重み・粗い段・残差の計器を作る。

FX_FN uint32_t EqGpuCapacityWord(uint32_t node) {
    return 2 * node;
}

FX_FN uint32_t EqGpuDiagonalWord(uint32_t maxNodes, uint32_t node) {
    return (2 * maxNodes) + (2 * node);
}

FX_FN uint32_t EqGpuCoefficientWord(uint32_t maxNodes, uint32_t link) {
    return (4 * maxNodes) + (2 * link);
}

FX_FN uint32_t EqGpuWorstResidualWord(uint32_t maxNodes, uint32_t maxLinks) {
    return (4 * maxNodes) + (2 * maxLinks);
}

FX_FN uint32_t EqGpuResidualWord(uint32_t maxNodes, uint32_t maxLinks, uint32_t node) {
    return EqGpuWorstResidualWord(maxNodes, maxLinks) + 1 + node;
}

// 作業用の語の数(段 0 の節 cells)
FX_FN uint64_t EqGpuScratchWords(uint32_t maxNodes, uint32_t maxLinks, uint32_t cells) {
    return (uint64_t)EqGpuResidualWord(maxNodes, maxLinks, 0) + cells;
}

// --- 閉じた領域(物理の長さ)-------------------------------------------------------------------

// レベル level のセルの一辺(位置の単位 2^-20 m。0 = 0.5 m・細かいほど半分・負は粗い)
FX_FN int64_t EqCellEdgeUnits(int32_t level) {
    if (level >= 0) {
        FX_ASSERT(level < 19);
        return CELL_SIZE_LEVEL0 >> (uint32_t)level;
    }

    FX_ASSERT(-level < 24);
    return CELL_SIZE_LEVEL0 << (uint32_t)(-level);
}

// 長さ(mm)を位置の単位へ(切り捨て)
FX_FN int64_t EqLengthUnits(uint64_t millimeters) {
    FX_ASSERT(millimeters < FX_U64(0x100u, 0u));  // 2^40 mm(× 2^20 が 64bit に入る)

    return (int64_t)((millimeters << POSITION_FRACTION_BITS) / EQ_MILLIMETERS_PER_METER);
}

// 1 つの軸の、半開区間 [aLow, aLow + aEdge) と [bLow, bLow + bEdge) の隙間(重なり・接していれば 0)
FX_FN int64_t EqAxisGap(int64_t aLow, int64_t aEdge, int64_t bLow, int64_t bEdge) {
    const int64_t right = bLow - (aLow + aEdge);
    const int64_t left = aLow - (bLow + bEdge);
    int64_t gap = right > left ? right : left;
    if (gap < 0)
        gap = 0;

    return gap;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_EQUILIBRIUM_HLSLI
