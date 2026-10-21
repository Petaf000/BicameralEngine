// equilibrium.hlsl — 釣り合う家族の共通部品(T-0853・T-0915)の、部品が受け持つ Compute の段。CPU リファレンスは engine/src/sim/equilibrium.cpp
// (段を作る BuildImplicitGrid の Coarsen・FinishLevel と残差の計器 MeterResiduals)。呼ぶ順は engine/src/sim/gpu_equilibrium.cpp:
//   (法則の段が段 0 を仕上げる: equilibrium_core.hlsli の EqGpuFinishNode・EqGpuSetRhs)
//   → EquilibriumLevel(段 1 から 1 段 1 Dispatch。節): 粗い段の容量・面の係数・重み・子の縮約の重み
//   → V サイクル(GpuImplicit::RecordSolve。決まった回数)→ EquilibriumMeter(段 0 の節): 残差の計器(式の単位)と最大
// 段の形(節・隣・親子の番号)は CPU の BuildImplicitGrid が作ったものを GpuImplicit に写してある(値だけを毎刻み作り直す)。
// どの節も同じレベルの系だけ(縮約はいつも親のセル: 子の値は ÷ 8、面の係数はさらに ÷ 2〔galerkinCoarse = false〕)。
// 浮動小数点は使わない(04 §4)。
#include "common/implicit_conduction.hlsli"

// --- 結び付け(gpu_equilibrium.cpp の ROOT_LAYOUT と同じ順)---
// u0 節 / u1 隣 / u2 子の一覧 / u3 作業場(GpuImplicit)/ u4 作業用の 128bit(common/equilibrium.hlsli の EqGpu*Word)
RWStructuredBuffer<ImGpuNode> g_eqNodes : register(u0);
RWStructuredBuffer<ImGpuLink> g_eqLinks : register(u1);
RWStructuredBuffer<uint32_t> g_eqLists : register(u2);
RWStructuredBuffer<int64_t> g_eqWork : register(u3);
RWStructuredBuffer<uint64_t> g_eqScratch : register(u4);

cbuffer EquilibriumConstants : register(b0) {
    uint32_t g_eqMaxNodes;    // GpuImplicit の節の上限(作業場の番地)
    uint32_t g_eqMaxLinks;    // 隣の上限(作業用の語の番地)
    uint32_t g_eqLevelStart;  // EquilibriumLevel: この段の節の始まり(全体の番号)
    uint32_t g_eqLevelCount;  // この段の節の数
    uint32_t g_eqCellCount;   // 段 0 の節の数(EquilibriumMeter)
};

#define EQ_MAX_NODES g_eqMaxNodes
#define EQ_MAX_LINKS g_eqMaxLinks
#include "sim/equilibrium_core.hlsli"

// --- 粗い段(節。implicit_conduction.cpp の Coarsen・FinishLevel)---
// 行(隣の親)は EQ_GPU_ROW_MAX まで(段の形を写す時に GpuEquilibrium::Supports が確かめる)。子の隣を 1 回だけ回し、隣の親で行を探して足す
[numthreads(64, 1, 1)] void EquilibriumLevel(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_eqLevelCount)
        return;

    const uint32_t parent = g_eqLevelStart + thread;
    const ImGpuNode node = g_eqNodes[parent];
    const uint32_t rowCount = node.linkEnd - node.linkStart;
    uint32_t rowNeighbors[EQ_GPU_ROW_MAX];
    FxU128 rowSums[EQ_GPU_ROW_MAX];
    FX_UNROLL for (uint32_t r = 0; r < EQ_GPU_ROW_MAX; ++r) {
        rowNeighbors[r] = r < rowCount ? g_eqLinks[node.linkStart + r].neighbor : EQ_GPU_NO_NEIGHBOR;
        rowSums[r] = ImWide(0);
    }

    // --- 容量 = 子の容量の和。隣の親との面の係数 = 子の隣のうち、その親の子へ向かうものの和 ---
    FxU128 capacity = ImWide(0);
    for (uint32_t c = node.childStart; c < node.childEnd; ++c) {
        const uint32_t childIndex = g_eqLists[c];
        capacity = ImWideAdd(capacity, ImWideShiftRight(EqGpuCapacity(childIndex), IM_ENERGY_BITS_PER_LEVEL));
        const ImGpuNode child = g_eqNodes[childIndex];
        for (uint32_t m = child.linkStart; m < child.linkEnd; ++m) {
            const uint32_t other = g_eqNodes[g_eqLinks[m].neighbor].parent;
            if (other == parent)
                continue;

            const FxU128 coefficient = ImWideShiftRight(EqGpuCoefficient(m), IM_ENERGY_BITS_PER_LEVEL);
            FX_UNROLL for (uint32_t r = 0; r < EQ_GPU_ROW_MAX; ++r) {
                if (rowNeighbors[r] == other)
                    rowSums[r] = ImWideAdd(rowSums[r], coefficient);
            }
        }
    }

    FxU128 diagonal = capacity;
    FX_UNROLL for (uint32_t r = 0; r < EQ_GPU_ROW_MAX; ++r) {
        rowSums[r] = ImWideShiftRight(rowSums[r], 1);
        if (r < rowCount)
            diagonal = ImWideAdd(diagonal, rowSums[r]);
    }

    // --- 重み(自分・隣)と、子の縮約の重み(子の D〔親の単位〕÷ 親の D)---
    EqGpuStoreWide(EqGpuCapacityWord(parent), capacity);
    EqGpuStoreWide(EqGpuDiagonalWord(EQ_MAX_NODES, parent), diagonal);
    g_eqNodes[parent].selfWeight = ImWeight(capacity, diagonal);
    FX_UNROLL for (uint32_t r = 0; r < EQ_GPU_ROW_MAX; ++r) {
        if (r < rowCount) {
            EqGpuStoreWide(EqGpuCoefficientWord(EQ_MAX_NODES, node.linkStart + r), rowSums[r]);
            g_eqLinks[node.linkStart + r].weight = ImWeight(rowSums[r], diagonal);
        }
    }

    for (uint32_t c = node.childStart; c < node.childEnd; ++c) {
        const uint32_t child = g_eqLists[c];
        g_eqNodes[child].restrictWeight = ImWeight(ImWideShiftRight(EqGpuDiagonal(child), IM_ENERGY_BITS_PER_LEVEL),
                                                   diagonal);
    }
}

    // --- 残差の計器(段 0 の節。equilibrium.cpp の MeterResiduals): |右辺 + Σ 隣の重み × x − x| × D ÷ 2^48(式の単位)と最大 ---
    [numthreads(64, 1, 1)] void EquilibriumMeter(uint32_t node : SV_DispatchThreadID) {
    if (node >= g_eqCellCount)
        return;

    const ImGpuNode entry = g_eqNodes[node];
    int64_t relaxed = g_eqWork[EqGpuRhs(node)];
    for (uint32_t k = entry.linkStart; k < entry.linkEnd; ++k) {
        const ImGpuLink link = g_eqLinks[k];
        relaxed += ImMulWeight(link.weight, g_eqWork[EqGpuValueA(link.neighbor)]);
    }

    const uint64_t magnitude = EqResidualMagnitude(relaxed - EqGpuValue(node), EqGpuDiagonal(node));
    g_eqScratch[EqGpuResidualWord(EQ_MAX_NODES, EQ_MAX_LINKS, node)] = magnitude;
    InterlockedMax(g_eqScratch[EqGpuWorstResidualWord(EQ_MAX_NODES, EQ_MAX_LINKS)], magnitude);
}
