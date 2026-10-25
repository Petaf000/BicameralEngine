// equilibrium_block.hlsli — 釣り合う家族の共通部品の「成分を持つ節」(T-0919・ADR-0418)の式。HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。
// CPU の回し方は engine/src/sim/equilibrium_block.cpp、GPU の回し方は engine/src/sim/gpu_equilibrium_block.cpp と shaders/sim/equilibrium_block.hlsl
// (T-0970・ADR-0422)。どちらもこの関数を呼ぶので、同じ入力からビット単位で同じ値になる(1 つの源。20 §2.7)。
// 持つもの: 節の対角のブロックの LDLᵀ(L は Q30)と、その解き・k × k のブロック × k の値の 128bit の和(残差・縮約・延ばし方・固定した面の右辺)。
// 成分の数 k は C++ では実行時の値、HLSL では EQB_K(コンパイル時の定数)を渡す。回しは決まった数(EQB_MAX_COMPONENTS)+ FX_UNROLL で、
// 構造体の配列はいつも決まった添字で読む(T-0397)。k を超える所は 0 のまま。
// 128bit の和は 2 の補数で桁を回したまま足す(和が厳密なので足す順は結果に効かない。R3)。最後に 1 回だけ 0 方向へ切り捨ててずらす。
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_EQUILIBRIUM_BLOCK_HLSLI
#define BICAMERAL_EQUILIBRIUM_BLOCK_HLSLI

#include "fixed.hlsli"

#ifdef __cplusplus
#include <array>
#define EQB_NAMESPACE_BEGIN                  \
    namespace bicameral::equilibrium_block { \
        using namespace ::bicameral::fx;
#define EQB_NAMESPACE_END }
#else
#define EQB_NAMESPACE_BEGIN
#define EQB_NAMESPACE_END
#endif

EQB_NAMESPACE_BEGIN

// --- 定数 --------------------------------------------------------------------------------------
FX_CONST uint32_t EQB_MAX_COMPONENTS = 6;                                     // 3 次元の支え(変位 3 + 回り 3)まで
FX_CONST uint32_t EQB_BLOCK_WORDS = EQB_MAX_COMPONENTS * EQB_MAX_COMPONENTS;  // k × k の置き場([行 × 6 + 列])
FX_CONST uint32_t EQB_PROLONG_SHIFT = 16;                                     // 延ばし方の行列の Q
FX_CONST uint32_t EQB_OCTANTS = 8;        // 子の八分割(x の奇偶 + 2 × y の奇偶 + 4 × z の奇偶)= 8 色
FX_CONST uint32_t EQB_FACTOR_SHIFT = 30;  // LDLᵀ の L の Q
// 消去の後の対角が元の対角のこの分の 1 より小さい向きは、支えの無い向きとして解かない
FX_CONST uint32_t EQB_PIVOT_FLOOR_SHIFT = 20;

// --- 型(C++ は std::array で、sim/equilibrium_block.h の EquilibriumBlock・EquilibriumVector と同じ型)---
#ifdef __cplusplus
using EqbBlock = std::array<int64_t, EQB_BLOCK_WORDS>;
using EqbVector = std::array<int64_t, EQB_MAX_COMPONENTS>;
using EqbWideVector = std::array<FxU128, EQB_MAX_COMPONENTS>;
#define EQB_AT(array, i) array[i]
#else
struct EqbBlock {
    int64_t v[EQB_BLOCK_WORDS];
};

struct EqbVector {
    int64_t v[EQB_MAX_COMPONENTS];
};

struct EqbWideVector {
    FxU128 v[EQB_MAX_COMPONENTS];
};

#define EQB_AT(array, i) array.v[i]
#endif

// 節の対角のブロックの LDLᵀ(L の対角は 1。pivots が 0 の成分は解かない = 支えの無い向き)
struct EqbFactor {
    EqbBlock lower;  // Q30
    EqbVector pivots;
};

// 0 で埋めた値(C++ は値の初期化・HLSL は 0 の型変換)
#ifdef __cplusplus
FX_FN EqbBlock EqbZeroBlock() {
    return EqbBlock{};
}

FX_FN EqbVector EqbZeroVector() {
    return EqbVector{};
}

FX_FN EqbWideVector EqbZeroWideVector() {
    return EqbWideVector{};
}
#else
EqbBlock EqbZeroBlock() {
    return (EqbBlock)0;
}

EqbVector EqbZeroVector() {
    return (EqbVector)0;
}

EqbWideVector EqbZeroWideVector() {
    return (EqbWideVector)0;
}
#endif

// --- 符号つきの 128bit(2 の補数。桁を回したまま足し、最後に 1 回だけずらす)---
FX_FN FxU128 EqbWideAdd(FxU128 a, FxU128 b) {
    FxU128 sum;
    sum.lo = a.lo + b.lo;
    sum.hi = a.hi + b.hi + (sum.lo < a.lo ? (uint64_t)1 : (uint64_t)0);

    return sum;
}

FX_FN FxU128 EqbWideNegate(FxU128 value) {
    FxU128 negated;
    negated.lo = ~value.lo + 1;
    negated.hi = ~value.hi + (negated.lo == 0 ? (uint64_t)1 : (uint64_t)0);

    return negated;
}

// 右へ shift ずらして int64 へ(0 方向へ切り捨て)
FX_FN int64_t EqbWideShift(FxU128 value, uint32_t shift) {
    const bool negative = (value.hi >> 63) != 0;
    FxU128 magnitude = value;  // 構造体の三項演算子は HLSL に無い
    if (negative)
        magnitude = EqbWideNegate(value);

    return FxApplySign(FxShiftRightU128(magnitude, shift), negative);
}

// sums[行] += Σ_列 block[行, 列] × values[列](k × k。128bit のまま)
FX_FN EqbWideVector EqbAddProduct(EqbWideVector sums, FX_IN(EqbBlock) block, FX_IN(EqbVector) values, uint32_t k) {
    FX_UNROLL for (uint32_t row = 0; row < EQB_MAX_COMPONENTS; ++row) {
        FX_UNROLL for (uint32_t column = 0; column < EQB_MAX_COMPONENTS; ++column) {
            const int64_t coefficient = EQB_AT(block, row * EQB_MAX_COMPONENTS + column);
            // 0 の積は足さない(和は同じ。CPU の速さのため)
            if (row < k && column < k && coefficient != 0 && EQB_AT(values, column) != 0) {
                const FxU128 product = FxMulS64Full(coefficient, EQB_AT(values, column));
                EQB_AT(sums, row) = EqbWideAdd(EQB_AT(sums, row), product);
            }
        }
    }

    return sums;
}

// 和を ÷ 2^shift して k 成分の値へ
FX_FN EqbVector EqbNarrow(FX_IN(EqbWideVector) sums, uint32_t k, uint32_t shift) {
    EqbVector result = EqbZeroVector();
    FX_UNROLL for (uint32_t row = 0; row < EQB_MAX_COMPONENTS; ++row) {
        if (row < k)
            EQB_AT(result, row) = EqbWideShift(EQB_AT(sums, row), shift);
    }

    return result;
}

// 残差 r = b − A x ÷ 2^shift(sums = Σ ブロック × 値。右辺の単位)
FX_FN EqbVector EqbResidual(FX_IN(EqbVector) rhs, FX_IN(EqbWideVector) sums, uint32_t k, uint32_t shift) {
    EqbVector residual = EqbZeroVector();
    FX_UNROLL for (uint32_t row = 0; row < EQB_MAX_COMPONENTS; ++row) {
        if (row < k)
            EQB_AT(residual, row) = EQB_AT(rhs, row) - EqbWideShift(EQB_AT(sums, row), shift);
    }

    return residual;
}

// 延ばし方: P × 親の値 ÷ 2^16(縮約の和も同じ形で、Pᵀ × 残差を子の分だけ足してから ÷ 2^16)
FX_FN EqbVector EqbProlong(FX_IN(EqbBlock) prolongation, FX_IN(EqbVector) parentValues, uint32_t k) {
    return EqbNarrow(EqbAddProduct(EqbZeroWideVector(), prolongation, parentValues, k), k, EQB_PROLONG_SHIFT);
}

// --- 節の対角のブロックの LDLᵀ(段を作る時。u_ij = L_ij × p_j は係数の単位)---
FX_FN EqbFactor EqbFactorize(FX_IN(EqbBlock) diagonal, uint32_t k) {
    EqbFactor factor;
    factor.lower = EqbZeroBlock();
    factor.pivots = EqbZeroVector();
    EqbBlock scaled = EqbZeroBlock();
    FX_UNROLL for (uint32_t i = 0; i < EQB_MAX_COMPONENTS; ++i) {
        if (i < k) {
            FX_UNROLL for (uint32_t j = 0; j < EQB_MAX_COMPONENTS; ++j) {
                if (j < i) {
                    int64_t value = EQB_AT(diagonal, i * EQB_MAX_COMPONENTS + j);
                    FX_UNROLL for (uint32_t m = 0; m < EQB_MAX_COMPONENTS; ++m) {
                        if (m < j)
                            value -= FxMulShiftS64(EQB_AT(factor.lower, i * EQB_MAX_COMPONENTS + m),
                                                   EQB_AT(scaled, j * EQB_MAX_COMPONENTS + m), EQB_FACTOR_SHIFT);
                    }

                    EQB_AT(scaled, i * EQB_MAX_COMPONENTS + j) = value;
                    // 支えの無い向き(pivot 0)は L も 0(割り算を三項演算子に入れない: HLSL では両側を計算しうる)
                    const int64_t pivot = EQB_AT(factor.pivots, j);
                    if (pivot != 0)
                        EQB_AT(factor.lower, i * EQB_MAX_COMPONENTS + j) = FxDivShiftS64(value, pivot,
                                                                                         EQB_FACTOR_SHIFT);
                }
            }

            int64_t pivot = EQB_AT(diagonal, i * EQB_MAX_COMPONENTS + i);
            FX_UNROLL for (uint32_t m = 0; m < EQB_MAX_COMPONENTS; ++m) {
                if (m < i)
                    pivot -= FxMulShiftS64(EQB_AT(factor.lower, i * EQB_MAX_COMPONENTS + m),
                                           EQB_AT(scaled, i * EQB_MAX_COMPONENTS + m), EQB_FACTOR_SHIFT);
            }

            const int64_t pivotMinimum = EQB_AT(diagonal, i * EQB_MAX_COMPONENTS + i) >> EQB_PIVOT_FLOOR_SHIFT;
            EQB_AT(factor.pivots, i) = pivot > 0 && pivot > pivotMinimum ? pivot : 0;
        }
    }

    return factor;
}

// D δ = r を解く(r は右辺の単位・δ は値の単位。coefficientShift は A × x ÷ 2^この値 = 右辺の単位)
FX_FN EqbVector EqbSolveFactor(FX_IN(EqbFactor) factor, FX_IN(EqbVector) residual, uint32_t k,
                               uint32_t coefficientShift) {
    EqbVector forward = EqbZeroVector();
    FX_UNROLL for (uint32_t i = 0; i < EQB_MAX_COMPONENTS; ++i) {
        if (i < k) {
            int64_t value = EQB_AT(residual, i);
            FX_UNROLL for (uint32_t j = 0; j < EQB_MAX_COMPONENTS; ++j) {
                if (j < i)
                    value -= FxMulShiftS64(EQB_AT(factor.lower, i * EQB_MAX_COMPONENTS + j), EQB_AT(forward, j),
                                           EQB_FACTOR_SHIFT);
            }

            EQB_AT(forward, i) = value;
        }
    }

    // 後ろから(i = k − 1 … 0)
    EqbVector delta = EqbZeroVector();
    FX_UNROLL for (uint32_t back = 0; back < EQB_MAX_COMPONENTS; ++back) {
        const uint32_t i = EQB_MAX_COMPONENTS - 1 - back;
        const int64_t pivot = EQB_AT(factor.pivots, i);
        if (i < k && pivot != 0) {
            int64_t value = FxDivShiftS64(EQB_AT(forward, i), pivot, coefficientShift);
            FX_UNROLL for (uint32_t j = 0; j < EQB_MAX_COMPONENTS; ++j) {
                if (j > i && j < k)
                    value -= FxMulShiftS64(EQB_AT(factor.lower, j * EQB_MAX_COMPONENTS + i), EQB_AT(delta, j),
                                           EQB_FACTOR_SHIFT);
            }

            EQB_AT(delta, i) = value;
        }
    }

    return delta;
}

// --- Galerkin の粗い段(Pᵀ_a A P_b を 128bit で。段を作る時)------------------------------------------
#ifdef __cplusplus
using EqbWideBlock = std::array<FxU128, EQB_BLOCK_WORDS>;
#else
struct EqbWideBlock {
    FxU128 v[EQB_BLOCK_WORDS];
};
#endif

#ifdef __cplusplus
FX_FN EqbWideBlock EqbZeroWideBlock() {
    return EqbWideBlock{};
}
#else
EqbWideBlock EqbZeroWideBlock() {
    return (EqbWideBlock)0;
}
#endif

// 128bit × 64bit(どちらも符号つき。2^128 を法として。真の値が 128bit に入れば厳密 = CPU の SignedMul と同じ値)
FX_FN FxU128 EqbWideMul(FxU128 a, int64_t b) {
    FxU128 product = FxMulU64Full(a.lo, (uint64_t)b);
    product.hi += a.hi * (uint64_t)b;
    if (b < 0)
        product.hi -= a.lo;

    return product;
}

// sum += Pᵀ_a × A × P_b(2 段: T = A P_b、sum += P_aᵀ T。和は 2^128 を法として厳密なので、CPU の AddTripleProduct の順と同じ値)
FX_FN EqbWideBlock EqbAddTriple(EqbWideBlock sum, FX_IN(EqbBlock) left, FX_IN(EqbBlock) middle, FX_IN(EqbBlock) right,
                                uint32_t k) {
    EqbWideBlock inner = EqbZeroWideBlock();
    FX_UNROLL for (uint32_t m = 0; m < EQB_MAX_COMPONENTS; ++m) {
        FX_UNROLL for (uint32_t l = 0; l < EQB_MAX_COMPONENTS; ++l) {
            FX_UNROLL for (uint32_t n = 0; n < EQB_MAX_COMPONENTS; ++n) {
                if (m < k && l < k && n < k) {
                    const FxU128 product = FxMulS64Full(EQB_AT(middle, m * EQB_MAX_COMPONENTS + n),
                                                        EQB_AT(right, n * EQB_MAX_COMPONENTS + l));
                    EQB_AT(inner, m * EQB_MAX_COMPONENTS + l) = EqbWideAdd(EQB_AT(inner, m * EQB_MAX_COMPONENTS + l),
                                                                           product);
                }
            }
        }
    }

    FX_UNROLL for (uint32_t row = 0; row < EQB_MAX_COMPONENTS; ++row) {
        FX_UNROLL for (uint32_t l = 0; l < EQB_MAX_COMPONENTS; ++l) {
            FX_UNROLL for (uint32_t m = 0; m < EQB_MAX_COMPONENTS; ++m) {
                if (row < k && l < k && m < k) {
                    const FxU128 product = EqbWideMul(EQB_AT(inner, m * EQB_MAX_COMPONENTS + l),
                                                      EQB_AT(left, m * EQB_MAX_COMPONENTS + row));
                    EQB_AT(sum, row * EQB_MAX_COMPONENTS + l) = EqbWideAdd(EQB_AT(sum, row * EQB_MAX_COMPONENTS + l),
                                                                           product);
                }
            }
        }
    }

    return sum;
}

FX_FN EqbBlock EqbNarrowBlock(FX_IN(EqbWideBlock) wide, uint32_t k, uint32_t shift) {
    EqbBlock block = EqbZeroBlock();
    FX_UNROLL for (uint32_t i = 0; i < EQB_BLOCK_WORDS; ++i) {
        if (i / EQB_MAX_COMPONENTS < k && i % EQB_MAX_COMPONENTS < k)
            EQB_AT(block, i) = EqbWideShift(EQB_AT(wide, i), shift);
    }

    return block;
}

// --- 置き場の番地(int64 の語。k × k のブロックは詰めて k² 語。CPU の詰めと GPU の読みが同じ関数)---------------
// 係数のバッファ: 延ばし方 P(8 × k²)・縮約 Pᵀ(8 × k²)・対角(節 × k²)・面のブロック(隣 × k²)・LDLᵀ(節 × (k² + k))・
//   値を固定した面(件 × (k² + k)。ブロックと外の値)
FX_FN uint32_t EqbProlongWord(uint32_t k, uint32_t octant) {
    return octant * k * k;
}

FX_FN uint32_t EqbRestrictWord(uint32_t k, uint32_t octant) {
    return (EQB_OCTANTS + octant) * k * k;
}

FX_FN uint32_t EqbDiagonalWord(uint32_t k, uint32_t node) {
    return (2 * EQB_OCTANTS + node) * k * k;
}

FX_FN uint32_t EqbBlockWord(uint32_t k, uint32_t nodeCapacity, uint32_t link) {
    return (2 * EQB_OCTANTS + nodeCapacity + link) * k * k;
}

FX_FN uint32_t EqbFactorWord(uint32_t k, uint32_t nodeCapacity, uint32_t linkCapacity, uint32_t node) {
    return (2 * EQB_OCTANTS + nodeCapacity + linkCapacity) * k * k + node * (k * k + k);
}

FX_FN uint32_t EqbFixedWord(uint32_t k, uint32_t nodeCapacity, uint32_t linkCapacity, uint32_t fixedFace) {
    return EqbFactorWord(k, nodeCapacity, linkCapacity, nodeCapacity) + fixedFace * (k * k + k);
}

FX_FN uint32_t EqbCoefficientWords(uint32_t k, uint32_t nodeCapacity, uint32_t linkCapacity, uint32_t fixedCapacity) {
    return EqbFixedWord(k, nodeCapacity, linkCapacity, fixedCapacity);
}

// 作業場: 値(全部の段の節 × k。段 0 は温めた始まり・粗い段は直し)・右辺(同じ)・段 0 の右辺の項(セル × k。法則が書く)
FX_FN uint32_t EqbValueWord(uint32_t k, uint32_t node) {
    return node * k;
}

FX_FN uint32_t EqbRhsWord(uint32_t k, uint32_t nodeCapacity, uint32_t node) {
    return (nodeCapacity + node) * k;
}

FX_FN uint32_t EqbSourceWord(uint32_t k, uint32_t nodeCapacity, uint32_t cell) {
    return (2 * nodeCapacity + cell) * k;
}

FX_FN uint32_t EqbWorkWords(uint32_t k, uint32_t nodeCapacity, uint32_t cells) {
    return EqbSourceWord(k, nodeCapacity, cells);
}

// 共役勾配(EquilibriumBlockOptions::lineSearch。T-1068・T-1085・ADR-0455)の作業場: 段 0 のセル × k の並び 6 本と、刻みの中の数 8 語
//   並び: 0 = V の前の値・1 = 向き p(初めは V の直し z)・2 = 残差 r・3 = 前の向き・4 = 前の残差・5 = −A p
//   数: 0 = 前の向きが使えるか(0 / 1)・1〜2 = 前の zᵀr(128bit の上・下)・3 = β(Q16)・4 = α(Q16)・5 = この歩を当てるか・6〜7 = 今の zᵀr
FX_CONST uint32_t EQB_CONJUGATE_BEFORE = 0;
FX_CONST uint32_t EQB_CONJUGATE_DIRECTION = 1;
FX_CONST uint32_t EQB_CONJUGATE_RESIDUAL = 2;
FX_CONST uint32_t EQB_CONJUGATE_DIRECTION_PREVIOUS = 3;
FX_CONST uint32_t EQB_CONJUGATE_RESIDUAL_PREVIOUS = 4;
FX_CONST uint32_t EQB_CONJUGATE_PUSHED = 5;
FX_CONST uint32_t EQB_CONJUGATE_ARRAYS = 6;
FX_CONST uint32_t EQB_CONJUGATE_SCALARS = 8;

FX_FN uint32_t EqbConjugateWord(uint32_t k, uint32_t nodeCapacity, uint32_t cells, uint32_t which, uint32_t cell) {
    return EqbWorkWords(k, nodeCapacity, cells) + (which * cells + cell) * k;
}

FX_FN uint32_t EqbConjugateScalarWord(uint32_t k, uint32_t nodeCapacity, uint32_t cells, uint32_t index) {
    return EqbWorkWords(k, nodeCapacity, cells) + EQB_CONJUGATE_ARRAYS * cells * k + index;
}

FX_FN uint32_t EqbConjugateWorkWords(uint32_t k, uint32_t nodeCapacity, uint32_t cells) {
    return EqbConjugateScalarWord(k, nodeCapacity, cells, EQB_CONJUGATE_SCALARS);
}

// --- 共役勾配の数(CPU の SolveEquilibriumBlocks と GPU で同じ式)---
FX_CONST uint64_t EQB_STEP_ONE = 65536;  // α・β の Q16 の 1
// 共役勾配の段の「何回目の V か」(GPU の g_eqbFirst)に足す印(T-1086・D-649)。ACROSS = 刻みをまたぐ(β の分母に pᵀr を覚える)・
// KEEP = 1 回目の V でも前の向きを捨てない(写した直後の最初の解きは付けない = CPU の「向きなしで始める」と同じ)
FX_CONST uint32_t EQB_CONJUGATE_ACROSS = 0x80000000u;
FX_CONST uint32_t EQB_CONJUGATE_KEEP = 0x40000000u;
FX_CONST uint32_t EQB_CONJUGATE_CYCLE_MASK = 0x3fffffffu;
FX_CONST uint64_t EQB_STEP_MAX = (uint64_t)65536 * (uint64_t)4096;  // 大きさの上限(4096 倍)

FX_FN bool EqbWideIsNegative(FxU128 value) {
    return (value.hi >> 63) != 0;
}

// 128bit の値の桁の数(0 なら 0)
FX_FN uint32_t EqbWideBitLength(FxU128 value) {
    if (value.hi != 0)
        return 65 + FxMsbU64(value.hi);

    return value.lo == 0 ? 0 : 1 + FxMsbU64(value.lo);
}

// 正の num ÷ den を Q16 で(上限 EQB_STEP_MAX)。den の上の 47 bit で割る
FX_FN uint64_t EqbStepRatio(FxU128 num, FxU128 den) {
    const uint32_t length = EqbWideBitLength(den);
    const uint32_t drop = length > 47 ? length - 47 : 0;
    const uint64_t denominator = FxShiftRightU128(den, drop);
    if (denominator == 0 || EqbWideBitLength(num) > drop + 60)
        return EQB_STEP_MAX;

    const uint64_t numerator = FxShiftRightU128(num, drop);
    if (numerator / denominator >= (EQB_STEP_MAX >> 16))
        return EQB_STEP_MAX;

    return FxDivU128By64(FxMulU64Full(numerator, EQB_STEP_ONE), denominator).quotient;
}

// 符号つきの num ÷ den(den > 0)を Q16 で。大きさは EQB_STEP_MAX まで
FX_FN int64_t EqbSignedRatio(FxU128 num, FxU128 den) {
    const bool negative = EqbWideIsNegative(num);
    FxU128 magnitude = num;  // 構造体の三項演算子は HLSL に無い
    if (negative)
        magnitude = EqbWideNegate(num);

    const uint64_t ratio = EqbStepRatio(magnitude, den);

    return negative ? -(int64_t)ratio : (int64_t)ratio;
}

// value × ratio(Q16。符号つき)を 0 方向へ切り捨て
FX_FN int64_t EqbScaleQ16(int64_t value, int64_t ratio) {
    const int64_t scaled = FxMulDivS64(value, FxAbsU64(ratio), EQB_STEP_ONE);

    return ratio < 0 ? -scaled : scaled;
}

// --- GPU の置き場の形(gpu_equilibrium_block.cpp が詰め、equilibrium_block.hlsl が読む)---
// 節(全部の段を続けた番号)。番地はどれもバッファの中の絶対の位置
struct EqbGpuNode {
    uint32_t rowStart;    // 隣の一覧(添字のバッファ)の始まり
    uint32_t rowEnd;      // 終わり
    uint32_t parent;      // 次の段の親(最も粗い段は使わない)
    uint32_t octant;      // 親の中の八分割(延ばし方 P・縮約 Pᵀ の選び)
    uint32_t childStart;  // 子の一覧(添字のバッファ)の始まり(段 1 から)
    uint32_t childEnd;
    uint32_t fixedStart;  // 値を固定した面の番号の始まり(段 0 だけ。係数のバッファの fixedBase + 番号 × (k × k + k))
    uint32_t fixedEnd;
};

EQB_NAMESPACE_END

#endif  // BICAMERAL_EQUILIBRIUM_BLOCK_HLSLI
