// equilibrium_core.hlsli — 釣り合う家族の共通部品(T-0853・T-0915。20 §9・§13.1)の GPU の回し方のうち、法則の段の同じスレッドが呼ぶ所と
// 部品の段(shaders/sim/equilibrium.hlsl)が使う所。式は common/equilibrium.hlsli・implicit_conduction.hlsli(CPU の engine/src/sim/equilibrium.cpp と同じ関数)。
// 法則の段(例: gas_projection.hlsl の GasProjectCell)は節 1 つにつき 1 スレッドで:
//   1. 自分だけの係数(C_i + Σ 値を固定した面の係数)と、隣の節との面の係数(EQ_GPU_FACES_MAX まで。無い所は EQ_GPU_NO_NEIGHBOR)を作り、
//      EqGpuFinishNode で対角 D・段 0 の重み・面の係数を部品の作業用の語へ(行の順は GpuImplicit の隣の一覧のまま。隣の節で面を探す)
//   2. 右辺の項を EqSourceTerm(値 × 係数 ÷ D。固定した面の値の項も同じ形)で足し、EqGpuSetRhs で右辺と温めた始まりを置く
// その後は部品の段: 粗い段(EquilibriumLevel)→ V サイクル(GpuImplicit::RecordSolve)→ 残差の計器(EquilibriumMeter)。解いた値は作業場の値 A。
// 含める側が先に決めるもの: 結び付け g_eqNodes(ImGpuNode)・g_eqLinks(ImGpuLink)・g_eqLists(子の一覧)・g_eqWork(int64 の作業場)・
// g_eqScratch(uint64 の作業用の語。並びは common/equilibrium.hlsli の EqGpu*Word)と、マクロ EQ_MAX_NODES・EQ_MAX_LINKS(GpuImplicit の上限)。
// 今の GPU 版は節がどれも同じレベルの系だけ(縮約はいつも親のセル。粗い段の行は EQ_GPU_ROW_MAX まで)。違うレベルの節は T-0939。
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_EQUILIBRIUM_CORE_HLSLI
#define BICAMERAL_EQUILIBRIUM_CORE_HLSLI

#include "common/equilibrium.hlsli"
#include "common/implicit_conduction.hlsli"

static const uint32_t EQ_GPU_FACES_MAX = 6;  // 段 0 の節の面の数の上限(箱のセルの 6 面)
static const uint32_t EQ_GPU_ROW_MAX = 6;    // 粗い段の行(隣の親)の上限(gpu_equilibrium.cpp の ROW_MAX)
static const uint32_t EQ_GPU_NO_NEIGHBOR = 0xFFFFFFFFu;

// --- 作業場(GpuImplicit の implicit_conduct.hlsl と同じ番地)と作業用の 128bit ---

uint32_t EqGpuValueA(uint32_t node) {
    return node;
}

uint32_t EqGpuValueB(uint32_t node) {
    return EQ_MAX_NODES + node;
}

uint32_t EqGpuRhs(uint32_t node) {
    return (2 * EQ_MAX_NODES) + node;
}

FxU128 EqGpuLoadWide(uint32_t word) {
    FxU128 value;
    value.hi = g_eqScratch[word];
    value.lo = g_eqScratch[word + 1];

    return value;
}

void EqGpuStoreWide(uint32_t word, FxU128 value) {
    g_eqScratch[word] = value.hi;
    g_eqScratch[word + 1] = value.lo;
}

FxU128 EqGpuCapacity(uint32_t node) {
    return EqGpuLoadWide(EqGpuCapacityWord(node));
}

FxU128 EqGpuDiagonal(uint32_t node) {
    return EqGpuLoadWide(EqGpuDiagonalWord(EQ_MAX_NODES, node));
}

FxU128 EqGpuCoefficient(uint32_t link) {
    return EqGpuLoadWide(EqGpuCoefficientWord(EQ_MAX_NODES, link));
}

// --- 1. 段 0 の節を仕上げる(CPU の Diagonals・MakeCellLevel の FinishLevel と同じ和と重み)。対角 D を返す ---
// own = 自分だけの係数(C_i + Σ 値を固定した面の係数)、coefficients[f] = 隣 neighbors[f] との面の係数(自分の単位)
FxU128 EqGpuFinishNode(uint32_t node, FxU128 own, uint32_t neighbors[EQ_GPU_FACES_MAX],
                       FxU128 coefficients[EQ_GPU_FACES_MAX]) {
    FxU128 diagonal = own;
    FX_UNROLL for (uint32_t f = 0; f < EQ_GPU_FACES_MAX; ++f) {
        if (neighbors[f] != EQ_GPU_NO_NEIGHBOR)
            diagonal = ImWideAdd(diagonal, coefficients[f]);
    }

    EqGpuStoreWide(EqGpuCapacityWord(node), own);
    EqGpuStoreWide(EqGpuDiagonalWord(EQ_MAX_NODES, node), diagonal);
    g_eqNodes[node].selfWeight = ImWeight(own, diagonal);

    // --- 隣の重み(行の順は隣の一覧のまま。隣の節で面を探す。最初に当たった面)---
    const ImGpuNode entry = g_eqNodes[node];
    for (uint32_t k = entry.linkStart; k < entry.linkEnd; ++k) {
        const uint32_t neighbor = g_eqLinks[k].neighbor;
        FxU128 coefficient = ImWide(0);
        bool found = false;
        FX_UNROLL for (uint32_t f = 0; f < EQ_GPU_FACES_MAX; ++f) {
            if (!found && neighbors[f] == neighbor) {
                coefficient = coefficients[f];
                found = true;
            }
        }

        g_eqLinks[k].weight = ImWeight(coefficient, diagonal);
        EqGpuStoreWide(EqGpuCoefficientWord(EQ_MAX_NODES, k), coefficient);
    }

    // 残差の計器の最大は部品の計器の段(EquilibriumMeter)が足す。その前の段で 0 に
    if (node == 0)
        g_eqScratch[EqGpuWorstResidualWord(EQ_MAX_NODES, EQ_MAX_LINKS)] = 0;

    return diagonal;
}

// --- 2. 右辺(右辺の項 ÷ D の和。値の単位)と温めた始まり(値 B = 値 A = 前の答え)---
void EqGpuSetRhs(uint32_t node, int64_t rhs) {
    g_eqWork[EqGpuRhs(node)] = rhs;
    g_eqWork[EqGpuValueB(node)] = g_eqWork[EqGpuValueA(node)];
}

// 解いた値(V サイクルの後。値 A)
int64_t EqGpuValue(uint32_t node) {
    return g_eqWork[EqGpuValueA(node)];
}

#endif  // BICAMERAL_EQUILIBRIUM_CORE_HLSLI
