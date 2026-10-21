// equilibrium_block.hlsl — 釣り合う家族の共通部品の「成分を持つ節」(T-0919・ADR-0418)の GPU の段(Compute。T-0970・ADR-0422)。
// CPU リファレンスは engine/src/sim/equilibrium_block.cpp(BuildEquilibriumBlockGrid・SolveEquilibriumBlocks)。式は common/equilibrium_block.hlsli で共有。
// 呼ぶ順は engine/src/sim/gpu_equilibrium_block.cpp:
//   準備(係数が変わった時): EquilibriumBlockFinish(段 0 の節: LDLᵀ・右辺 = 右辺の項 + 固定した面)
//     → EquilibriumBlockCoarsen(段 1 から 1 段 1 Dispatch: Galerkin の Pᵀ A P を 128bit で足して 1 回ずらす・LDLᵀ)
//   解き(刻みごと): V サイクルをちょうど cycles 回。1 回 = EquilibriumBlockSmooth(段・色ごとに 1 Dispatch。8 色)
//     → EquilibriumBlockRestrict(粗い段の節: 子の残差 × Pᵀ の和 → 右辺・直しを 0 に)→ 粗い段へ → EquilibriumBlockProlong(細かい段の節: + P × 直し)→ Smooth
// 成分の数 k は EQB_K(コンパイル時。shaders/CMakeLists.txt が k = 1・2・3・6 で別々に作る)。回しは決まった数 + FX_UNROLL で、
// 構造体の配列を動的な添字で読まない(T-0397)。節の番号は全部の段を続けたもの(段 0 が先)。同じ色の節どうしは隣でないので、
// 色の中の順は結果に効かない(R3)。浮動小数点は使わない(04 §4)。
#include "common/equilibrium_block.hlsli"

#ifndef EQB_K
#define EQB_K 3
#endif

static const uint32_t EQB_COMPONENTS = EQB_K;
static const uint32_t EQB_COMPONENTS_SQUARED = EQB_K * EQB_K;

// --- 結び付け(gpu_equilibrium_block.cpp の ROOT_LAYOUT と同じ順)---
// u0 係数(延ばし方・縮約・対角・面のブロック・LDLᵀ・固定した面)/ u1 作業場(値・右辺・右辺の項)/ t0 節 / t1 添字(隣・子・色の一覧)
RWStructuredBuffer<int64_t> g_eqbCoefficients : register(u0);
RWStructuredBuffer<int64_t> g_eqbWork : register(u1);
StructuredBuffer<EqbGpuNode> g_eqbNodes : register(t0);
StructuredBuffer<uint32_t> g_eqbIndices : register(t1);

cbuffer EquilibriumBlockConstants : register(b0) {
    uint32_t g_eqbNodeCapacity;  // 節の数(全部の段。番地の元)
    uint32_t g_eqbLinkCapacity;  // 隣の数(全部の段)
    uint32_t g_eqbFirst;         // この段の節の始まり(Finish・Coarsen・Restrict・Prolong)
    uint32_t g_eqbCount;         // スレッドの数(段の節の数・色の一覧の長さ)
    uint32_t g_eqbList;          // Smooth: 色の一覧の添字のバッファの始まり
    uint32_t g_eqbShift;         // A × x ÷ 2^この値 = 右辺の単位(coefficientShift)
};

// --- 読み書き(k × k は詰めて k² 語。置き場の [行 × 6 + 列] へ広げる)---
EqbBlock LoadBlock(uint32_t word) {
    EqbBlock block = EqbZeroBlock();
    FX_UNROLL for (uint32_t row = 0; row < EQB_MAX_COMPONENTS; ++row) {
        FX_UNROLL for (uint32_t column = 0; column < EQB_MAX_COMPONENTS; ++column) {
            if (row < EQB_COMPONENTS && column < EQB_COMPONENTS)
                block.v[row * EQB_MAX_COMPONENTS + column] = g_eqbCoefficients[word + row * EQB_COMPONENTS + column];
        }
    }

    return block;
}

void StoreBlock(uint32_t word, EqbBlock block) {
    FX_UNROLL for (uint32_t row = 0; row < EQB_MAX_COMPONENTS; ++row) {
        FX_UNROLL for (uint32_t column = 0; column < EQB_MAX_COMPONENTS; ++column) {
            if (row < EQB_COMPONENTS && column < EQB_COMPONENTS)
                g_eqbCoefficients[word + row * EQB_COMPONENTS + column] = block.v[row * EQB_MAX_COMPONENTS + column];
        }
    }
}

EqbVector LoadCoefficientVector(uint32_t word) {
    EqbVector result = EqbZeroVector();
    FX_UNROLL for (uint32_t c = 0; c < EQB_MAX_COMPONENTS; ++c) {
        if (c < EQB_COMPONENTS)
            result.v[c] = g_eqbCoefficients[word + c];
    }

    return result;
}

EqbVector LoadWork(uint32_t word) {
    EqbVector result = EqbZeroVector();
    FX_UNROLL for (uint32_t c = 0; c < EQB_MAX_COMPONENTS; ++c) {
        if (c < EQB_COMPONENTS)
            result.v[c] = g_eqbWork[word + c];
    }

    return result;
}

void StoreWork(uint32_t word, EqbVector result) {
    FX_UNROLL for (uint32_t c = 0; c < EQB_MAX_COMPONENTS; ++c) {
        if (c < EQB_COMPONENTS)
            g_eqbWork[word + c] = result.v[c];
    }
}

uint32_t FactorWord(uint32_t node) {
    return EqbFactorWord(EQB_COMPONENTS, g_eqbNodeCapacity, g_eqbLinkCapacity, node);
}

EqbFactor LoadFactor(uint32_t node) {
    const uint32_t word = FactorWord(node);
    EqbFactor factor;
    factor.lower = LoadBlock(word);
    factor.pivots = LoadCoefficientVector(word + EQB_COMPONENTS_SQUARED);

    return factor;
}

void StoreFactor(uint32_t node, EqbFactor factor) {
    const uint32_t word = FactorWord(node);
    StoreBlock(word, factor.lower);
    FX_UNROLL for (uint32_t c = 0; c < EQB_MAX_COMPONENTS; ++c) {
        if (c < EQB_COMPONENTS)
            g_eqbCoefficients[word + EQB_COMPONENTS_SQUARED + c] = factor.pivots.v[c];
    }
}

// 節の残差 r = b − A x(equilibrium_block.cpp の NodeResidual と同じ和)
EqbVector NodeResidual(uint32_t node) {
    const EqbGpuNode entry = g_eqbNodes[node];
    EqbWideVector sums = EqbZeroWideVector();
    sums = EqbAddProduct(sums, LoadBlock(EqbDiagonalWord(EQB_COMPONENTS, node)),
                         LoadWork(EqbValueWord(EQB_COMPONENTS, node)), EQB_COMPONENTS);
    for (uint32_t link = entry.rowStart; link < entry.rowEnd; ++link) {
        const uint32_t neighbor = g_eqbIndices[link];
        sums = EqbAddProduct(sums, LoadBlock(EqbBlockWord(EQB_COMPONENTS, g_eqbNodeCapacity, link)),
                             LoadWork(EqbValueWord(EQB_COMPONENTS, neighbor)), EQB_COMPONENTS);
    }

    const EqbVector rhs = LoadWork(EqbRhsWord(EQB_COMPONENTS, g_eqbNodeCapacity, node));

    return EqbResidual(rhs, sums, EQB_COMPONENTS, g_eqbShift);
}

// --- 準備: 段 0 の節(LDLᵀ と右辺。SolveEquilibriumBlocks の右辺 = 右辺の項 + Σ 固定した面の block × value ÷ 2^shift)---
[numthreads(64, 1, 1)] void EquilibriumBlockFinish(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_eqbCount)
        return;

    const uint32_t node = thread;
    const EqbGpuNode entry = g_eqbNodes[node];
    StoreFactor(node, EqbFactorize(LoadBlock(EqbDiagonalWord(EQB_COMPONENTS, node)), EQB_COMPONENTS));

    EqbVector rhs = LoadWork(EqbSourceWord(EQB_COMPONENTS, g_eqbNodeCapacity, node));
    for (uint32_t fixedFace = entry.fixedStart; fixedFace < entry.fixedEnd; ++fixedFace) {
        const uint32_t word = EqbFixedWord(EQB_COMPONENTS, g_eqbNodeCapacity, g_eqbLinkCapacity, fixedFace);
        const EqbWideVector sums = EqbAddProduct(EqbZeroWideVector(), LoadBlock(word),
                                                 LoadCoefficientVector(word + EQB_COMPONENTS_SQUARED), EQB_COMPONENTS);
        const EqbVector added = EqbNarrow(sums, EQB_COMPONENTS, g_eqbShift);
        FX_UNROLL for (uint32_t c = 0; c < EQB_MAX_COMPONENTS; ++c) {
            if (c < EQB_COMPONENTS)
                rhs.v[c] += added.v[c];
        }
    }

    StoreWork(EqbRhsWord(EQB_COMPONENTS, g_eqbNodeCapacity, node), rhs);
}

// --- 準備: 粗い段(Galerkin。親 parent の子の、相手の親が target の項の Pᵀ_子 A P_相手 の和)---
// target == parent なら対角(子の対角の項も)。equilibrium_block.cpp の Coarsen と同じ項を 2^128 を法として足す(順は効かない)
EqbWideBlock CoarseSum(EqbGpuNode entry, uint32_t parent, uint32_t target) {
    EqbWideBlock sum = EqbZeroWideBlock();
    for (uint32_t c = entry.childStart; c < entry.childEnd; ++c) {
        const uint32_t child = g_eqbIndices[c];
        const EqbGpuNode childEntry = g_eqbNodes[child];
        const EqbBlock left = LoadBlock(EqbProlongWord(EQB_COMPONENTS, childEntry.octant));
        if (target == parent)
            sum = EqbAddTriple(sum, left, LoadBlock(EqbDiagonalWord(EQB_COMPONENTS, child)), left, EQB_COMPONENTS);

        for (uint32_t link = childEntry.rowStart; link < childEntry.rowEnd; ++link) {
            const EqbGpuNode otherEntry = g_eqbNodes[g_eqbIndices[link]];
            if (otherEntry.parent == target) {
                const EqbBlock right = LoadBlock(EqbProlongWord(EQB_COMPONENTS, otherEntry.octant));
                const EqbBlock middle = LoadBlock(EqbBlockWord(EQB_COMPONENTS, g_eqbNodeCapacity, link));
                sum = EqbAddTriple(sum, left, middle, right, EQB_COMPONENTS);
            }
        }
    }

    return sum;
}

[numthreads(64, 1, 1)] void EquilibriumBlockCoarsen(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_eqbCount)
        return;

    const uint32_t parent = g_eqbFirst + thread;
    const EqbGpuNode entry = g_eqbNodes[parent];
    const EqbBlock diagonal = EqbNarrowBlock(CoarseSum(entry, parent, parent), EQB_COMPONENTS, 2 * EQB_PROLONG_SHIFT);
    StoreBlock(EqbDiagonalWord(EQB_COMPONENTS, parent), diagonal);
    StoreFactor(parent, EqbFactorize(diagonal, EQB_COMPONENTS));
    for (uint32_t link = entry.rowStart; link < entry.rowEnd; ++link) {
        const EqbWideBlock sum = CoarseSum(entry, parent, g_eqbIndices[link]);
        StoreBlock(EqbBlockWord(EQB_COMPONENTS, g_eqbNodeCapacity, link),
                   EqbNarrowBlock(sum, EQB_COMPONENTS, 2 * EQB_PROLONG_SHIFT));
    }
}

    // --- 解き: 1 色の節のブロックの Gauss–Seidel(節の LDLᵀ で δ を解いて足す)---
    [numthreads(64, 1, 1)] void EquilibriumBlockSmooth(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_eqbCount)
        return;

    const uint32_t node = g_eqbIndices[g_eqbList + thread];
    const EqbVector residual = NodeResidual(node);
    const EqbVector delta = EqbSolveFactor(LoadFactor(node), residual, EQB_COMPONENTS, g_eqbShift);
    const uint32_t word = EqbValueWord(EQB_COMPONENTS, node);
    EqbVector values = LoadWork(word);
    FX_UNROLL for (uint32_t c = 0; c < EQB_MAX_COMPONENTS; ++c) {
        if (c < EQB_COMPONENTS)
            values.v[c] += delta.v[c];
    }

    StoreWork(word, values);
}

// --- 解き: 粗い段の右辺 = Σ 子の Pᵀ × 子の残差 ÷ 2^16。粗い段の値(直し)を 0 から ---
[numthreads(64, 1, 1)] void EquilibriumBlockRestrict(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_eqbCount)
        return;

    const uint32_t parent = g_eqbFirst + thread;
    const EqbGpuNode entry = g_eqbNodes[parent];
    EqbWideVector sums = EqbZeroWideVector();
    for (uint32_t c = entry.childStart; c < entry.childEnd; ++c) {
        const uint32_t child = g_eqbIndices[c];
        const EqbBlock restriction = LoadBlock(EqbRestrictWord(EQB_COMPONENTS, g_eqbNodes[child].octant));
        sums = EqbAddProduct(sums, restriction, NodeResidual(child), EQB_COMPONENTS);
    }

    StoreWork(EqbRhsWord(EQB_COMPONENTS, g_eqbNodeCapacity, parent),
              EqbNarrow(sums, EQB_COMPONENTS, EQB_PROLONG_SHIFT));
    StoreWork(EqbValueWord(EQB_COMPONENTS, parent), EqbZeroVector());
}

    // --- 解き: 細かい段の値 += P × 親の直し ÷ 2^16 ---
    [numthreads(64, 1, 1)] void EquilibriumBlockProlong(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_eqbCount)
        return;

    const uint32_t node = g_eqbFirst + thread;
    const EqbGpuNode entry = g_eqbNodes[node];
    const EqbVector added = EqbProlong(LoadBlock(EqbProlongWord(EQB_COMPONENTS, entry.octant)),
                                       LoadWork(EqbValueWord(EQB_COMPONENTS, entry.parent)), EQB_COMPONENTS);
    const uint32_t word = EqbValueWord(EQB_COMPONENTS, node);
    EqbVector values = LoadWork(word);
    FX_UNROLL for (uint32_t c = 0; c < EQB_MAX_COMPONENTS; ++c) {
        if (c < EQB_COMPONENTS)
            values.v[c] += added.v[c];
    }

    StoreWork(word, values);
}
