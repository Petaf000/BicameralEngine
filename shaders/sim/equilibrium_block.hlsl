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

// --- 解き: 共役勾配(EquilibriumBlockOptions::lineSearch。T-1085・ADR-0455。equilibrium_block.cpp の ConjugateStep と同じ式)---
// 段 0 のセル(節 0〜セルの数 − 1。セルの数は g_eqbList)だけ。V の 1 回ごとに
//   ConjugateSave → (V サイクル)→ ConjugateResidual → ConjugateBeta → ConjugateDirection → ConjugatePush → ConjugateAlpha → ConjugateUpdate。
// 内積は 128bit の和(2^128 を法として厳密。足す順によらない)を 1 つのグループで集める。g_eqbFirst = 何回目の V か(0 なら前の向きを捨てる)
static const uint32_t EQB_REDUCE_THREADS = 256;

groupshared uint64_t s_eqbHighA[EQB_REDUCE_THREADS];
groupshared uint64_t s_eqbLowA[EQB_REDUCE_THREADS];
groupshared uint64_t s_eqbHighB[EQB_REDUCE_THREADS];
groupshared uint64_t s_eqbLowB[EQB_REDUCE_THREADS];

uint32_t ConjugateWord(uint32_t which, uint32_t cell) {
    return EqbConjugateWord(EQB_COMPONENTS, g_eqbNodeCapacity, g_eqbList, which, cell);
}

uint32_t ConjugateScalarWord(uint32_t index) {
    return EqbConjugateScalarWord(EQB_COMPONENTS, g_eqbNodeCapacity, g_eqbList, index);
}

FxU128 LoadWide(uint32_t index) {
    FxU128 value;
    value.hi = (uint64_t)g_eqbWork[ConjugateScalarWord(index)];
    value.lo = (uint64_t)g_eqbWork[ConjugateScalarWord(index + 1)];

    return value;
}

void StoreWide(uint32_t index, FxU128 value) {
    g_eqbWork[ConjugateScalarWord(index)] = (int64_t)value.hi;
    g_eqbWork[ConjugateScalarWord(index + 1)] = (int64_t)value.lo;
}

// 段 0 の節 node の rhs − A x(x は作業場の base から節 × k で並ぶ。withRhs でなければ −A x。NodeResidual と同じ和)
EqbVector NodeResidualOf(uint32_t node, uint32_t base, bool withRhs) {
    const EqbGpuNode entry = g_eqbNodes[node];
    EqbWideVector sums = EqbZeroWideVector();
    sums = EqbAddProduct(sums, LoadBlock(EqbDiagonalWord(EQB_COMPONENTS, node)), LoadWork(base + node * EQB_COMPONENTS),
                         EQB_COMPONENTS);
    for (uint32_t link = entry.rowStart; link < entry.rowEnd; ++link) {
        const uint32_t neighbor = g_eqbIndices[link];
        sums = EqbAddProduct(sums, LoadBlock(EqbBlockWord(EQB_COMPONENTS, g_eqbNodeCapacity, link)),
                             LoadWork(base + neighbor * EQB_COMPONENTS), EQB_COMPONENTS);
    }

    EqbVector rhs = EqbZeroVector();
    if (withRhs)
        rhs = LoadWork(EqbRhsWord(EQB_COMPONENTS, g_eqbNodeCapacity, node));

    return EqbResidual(rhs, sums, EQB_COMPONENTS, g_eqbShift);
}

// グループの 2 つの和(A・B)を足し合わせ、スレッド 0 に残す
void ReduceTwo(uint32_t thread, FxU128 sumA, FxU128 sumB) {
    s_eqbHighA[thread] = sumA.hi;
    s_eqbLowA[thread] = sumA.lo;
    s_eqbHighB[thread] = sumB.hi;
    s_eqbLowB[thread] = sumB.lo;
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t stride = EQB_REDUCE_THREADS / 2; stride > 0; stride >>= 1) {
        if (thread < stride) {
            FxU128 mine;
            FxU128 other;
            mine.hi = s_eqbHighA[thread];
            mine.lo = s_eqbLowA[thread];
            other.hi = s_eqbHighA[thread + stride];
            other.lo = s_eqbLowA[thread + stride];
            const FxU128 addedA = EqbWideAdd(mine, other);
            s_eqbHighA[thread] = addedA.hi;
            s_eqbLowA[thread] = addedA.lo;
            mine.hi = s_eqbHighB[thread];
            mine.lo = s_eqbLowB[thread];
            other.hi = s_eqbHighB[thread + stride];
            other.lo = s_eqbLowB[thread + stride];
            const FxU128 addedB = EqbWideAdd(mine, other);
            s_eqbHighB[thread] = addedB.hi;
            s_eqbLowB[thread] = addedB.lo;
        }

        GroupMemoryBarrierWithGroupSync();
    }
}

FxU128 ReducedA() {
    FxU128 value;
    value.hi = s_eqbHighA[0];
    value.lo = s_eqbLowA[0];

    return value;
}

FxU128 ReducedB() {
    FxU128 value;
    value.hi = s_eqbHighB[0];
    value.lo = s_eqbLowB[0];

    return value;
}

FxU128 ZeroWide() {
    FxU128 value;
    value.hi = 0;
    value.lo = 0;

    return value;
}

// V の前の値を控える(1 回目の V なら前の向きを捨てる。刻みをまたぐ時は KEEP の印で捨てない)
[numthreads(64, 1, 1)] void EquilibriumBlockConjugateSave(uint32_t thread : SV_DispatchThreadID) {
    if (thread == 0 && (g_eqbFirst & EQB_CONJUGATE_CYCLE_MASK) == 0 && (g_eqbFirst & EQB_CONJUGATE_KEEP) == 0)
        g_eqbWork[ConjugateScalarWord(0)] = 0;

    if (thread >= g_eqbCount)
        return;

    StoreWork(ConjugateWord(EQB_CONJUGATE_BEFORE, thread), LoadWork(EqbValueWord(EQB_COMPONENTS, thread)));
}

    // z = V の後 − V の前(向きの置き場へ)・r = b − A × V の前
    [numthreads(64, 1, 1)] void EquilibriumBlockConjugateResidual(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_eqbCount)
        return;

    const EqbVector after = LoadWork(EqbValueWord(EQB_COMPONENTS, thread));
    const EqbVector before = LoadWork(ConjugateWord(EQB_CONJUGATE_BEFORE, thread));
    EqbVector direction = EqbZeroVector();
    FX_UNROLL for (uint32_t c = 0; c < EQB_MAX_COMPONENTS; ++c) {
        if (c < EQB_COMPONENTS)
            direction.v[c] = after.v[c] - before.v[c];
    }

    StoreWork(ConjugateWord(EQB_CONJUGATE_DIRECTION, thread), direction);
    StoreWork(ConjugateWord(EQB_CONJUGATE_RESIDUAL, thread),
              NodeResidualOf(thread, ConjugateWord(EQB_CONJUGATE_BEFORE, 0), true));
}

// zᵀr と zᵀ(r − r_前)→ β = zᵀ(r − r_前) ÷ z_前ᵀr_前(前の向きが使える時だけ。Polak–Ribière)
[numthreads(256, 1, 1)] void EquilibriumBlockConjugateBeta(uint32_t thread : SV_GroupThreadID) {
    const uint32_t words = g_eqbCount * EQB_COMPONENTS;
    const uint32_t direction = ConjugateWord(EQB_CONJUGATE_DIRECTION, 0);
    const uint32_t residual = ConjugateWord(EQB_CONJUGATE_RESIDUAL, 0);
    const uint32_t previous = ConjugateWord(EQB_CONJUGATE_RESIDUAL_PREVIOUS, 0);
    FxU128 zr = ZeroWide();
    FxU128 change = ZeroWide();
    for (uint32_t i = thread; i < words; i += EQB_REDUCE_THREADS) {
        const int64_t z = g_eqbWork[direction + i];
        const int64_t r = g_eqbWork[residual + i];
        zr = EqbWideAdd(zr, FxMulS64Full(z, r));
        change = EqbWideAdd(change, FxMulS64Full(z, r - g_eqbWork[previous + i]));
    }

    ReduceTwo(thread, zr, change);
    if (thread != 0)
        return;

    const bool valid = g_eqbWork[ConjugateScalarWord(0)] != 0;
    int64_t beta = 0;
    if (valid)
        beta = EqbSignedRatio(ReducedB(), LoadWide(1));

    g_eqbWork[ConjugateScalarWord(3)] = beta;
    StoreWide(6, ReducedA());
}

    // p = z + β p_前(前の向きが使える時だけ)
    [numthreads(64, 1, 1)] void EquilibriumBlockConjugateDirection(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_eqbCount || g_eqbWork[ConjugateScalarWord(0)] == 0)
        return;

    const int64_t beta = g_eqbWork[ConjugateScalarWord(3)];
    const EqbVector previous = LoadWork(ConjugateWord(EQB_CONJUGATE_DIRECTION_PREVIOUS, thread));
    EqbVector direction = LoadWork(ConjugateWord(EQB_CONJUGATE_DIRECTION, thread));
    FX_UNROLL for (uint32_t c = 0; c < EQB_MAX_COMPONENTS; ++c) {
        if (c < EQB_COMPONENTS)
            direction.v[c] += EqbScaleQ16(previous.v[c], beta);
    }

    StoreWork(ConjugateWord(EQB_CONJUGATE_DIRECTION, thread), direction);
}

// −A p
[numthreads(64, 1, 1)] void EquilibriumBlockConjugatePush(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_eqbCount)
        return;

    StoreWork(ConjugateWord(EQB_CONJUGATE_PUSHED, thread),
              NodeResidualOf(thread, ConjugateWord(EQB_CONJUGATE_DIRECTION, 0), false));
}

    // rᵀp と pᵀAp → 当てるか・歩幅 α = rᵀp ÷ pᵀAp・次の歩で前の向きが使えるか
    [numthreads(256, 1, 1)] void EquilibriumBlockConjugateAlpha(uint32_t thread : SV_GroupThreadID) {
    const uint32_t words = g_eqbCount * EQB_COMPONENTS;
    const uint32_t direction = ConjugateWord(EQB_CONJUGATE_DIRECTION, 0);
    const uint32_t residual = ConjugateWord(EQB_CONJUGATE_RESIDUAL, 0);
    const uint32_t pushed = ConjugateWord(EQB_CONJUGATE_PUSHED, 0);
    FxU128 along = ZeroWide();
    FxU128 energy = ZeroWide();
    for (uint32_t i = thread; i < words; i += EQB_REDUCE_THREADS) {
        const int64_t p = g_eqbWork[direction + i];
        along = EqbWideAdd(along, FxMulS64Full(p, g_eqbWork[residual + i]));
        energy = EqbWideAdd(energy, EqbWideNegate(FxMulS64Full(p, g_eqbWork[pushed + i])));
    }

    ReduceTwo(thread, along, energy);
    if (thread != 0)
        return;

    const FxU128 alongSum = ReducedA();
    const FxU128 energySum = ReducedB();
    const bool accept = !EqbWideIsNegative(alongSum) && !EqbWideIsNegative(energySum) &&
                        EqbWideBitLength(alongSum) != 0 && EqbWideBitLength(energySum) != 0;
    // 次の β の分母: 刻みの中だけなら zᵀr・刻みをまたぐなら pᵀr(厳密な線探索で同じ値。持つ向きと残差から数え直せる。T-1086)
    const FxU128 zr = LoadWide(6);
    FxU128 remembered = zr;  // 構造体は条件演算子で選べない(HLSL)
    if ((g_eqbFirst & EQB_CONJUGATE_ACROSS) != 0)
        remembered = alongSum;

    bool valid = false;
    int64_t alpha = 0;
    if (accept) {
        alpha = (int64_t)EqbStepRatio(alongSum, energySum);
        valid = !EqbWideIsNegative(remembered) && EqbWideBitLength(remembered) != 0;
        StoreWide(1, remembered);
    }

    g_eqbWork[ConjugateScalarWord(0)] = valid ? 1 : 0;
    g_eqbWork[ConjugateScalarWord(4)] = alpha;
    g_eqbWork[ConjugateScalarWord(5)] = accept ? 1 : 0;
}

// 当てる歩なら 値 = V の前 + α p、向きと残差を前の歩として控える
[numthreads(64, 1, 1)] void EquilibriumBlockConjugateUpdate(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_eqbCount || g_eqbWork[ConjugateScalarWord(5)] == 0)
        return;

    const uint64_t alpha = (uint64_t)g_eqbWork[ConjugateScalarWord(4)];
    const EqbVector before = LoadWork(ConjugateWord(EQB_CONJUGATE_BEFORE, thread));
    const EqbVector direction = LoadWork(ConjugateWord(EQB_CONJUGATE_DIRECTION, thread));
    EqbVector values = EqbZeroVector();
    FX_UNROLL for (uint32_t c = 0; c < EQB_MAX_COMPONENTS; ++c) {
        if (c < EQB_COMPONENTS)
            values.v[c] = before.v[c] + FxMulDivS64(direction.v[c], alpha, EQB_STEP_ONE);
    }

    StoreWork(EqbValueWord(EQB_COMPONENTS, thread), values);
    StoreWork(ConjugateWord(EQB_CONJUGATE_DIRECTION_PREVIOUS, thread), direction);
    StoreWork(ConjugateWord(EQB_CONJUGATE_RESIDUAL_PREVIOUS, thread),
              LoadWork(ConjugateWord(EQB_CONJUGATE_RESIDUAL, thread)));
}
