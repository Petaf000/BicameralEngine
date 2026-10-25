// equilibrium_upload.hlsl — 釣り合う家族の部品(GpuEquilibrium)の段 0 を、CPU が組んだ系(EquilibriumSystem)の写しから仕上げる法則の段(T-1111)。
// 時間を飛ばす試作(sim/time_step_tree)の列の系は CPU が組むので、GPU で解き比べる間はこの段が法則の段 0 の代わりをする
// (本物の GPU 版では法則のシェーダーが VRAM の状態から同じ段 0 を組む。T-1048)。CPU リファレンスは engine/src/sim/equilibrium.cpp の
// Diagonals・SourceRhs・MakeLocal(閉じない道。固定した面の項も右辺の項と同じ EqSourceTerm)と BuildImplicitGrid の段 0。
// 写しの並び(64bit の語。engine/src/sim/gpu_equilibrium_upload.cpp の EQ_UPLOAD_*):
//   節ごとに EQ_UPLOAD_NODE_WORDS 語 = 自分だけの係数(C_i + Σ 固定した面の係数。hi・lo)・6 面の係数(hi・lo)・隣の節(32bit × 6 を 3 語)・
//   右辺の項の始まりと数(下 32bit・上 32bit)。その後に右辺の項が 3 語ずつ(値・係数の hi・lo)
// 呼ぶ順: この段(節)→ 部品の段(shaders/sim/equilibrium.hlsl の粗い段 → V サイクル)。浮動小数点は使わない(04 §4)。
#include "common/equilibrium.hlsli"
#include "common/implicit_conduction.hlsli"

// --- 結び付け(gpu_equilibrium_upload.cpp の ROOT_LAYOUT と同じ順。u0〜u4 は部品と同じもの)---
RWStructuredBuffer<ImGpuNode> g_eqNodes : register(u0);
RWStructuredBuffer<ImGpuLink> g_eqLinks : register(u1);
RWStructuredBuffer<uint32_t> g_eqLists : register(u2);  // 子の一覧(この段では読まない)
RWStructuredBuffer<int64_t> g_eqWork : register(u3);
RWStructuredBuffer<uint64_t> g_eqScratch : register(u4);
StructuredBuffer<uint64_t> g_upStage : register(t0);  // 段 0 の写し(既定のヒープ)

cbuffer EquilibriumUploadConstants : register(b0) {
    uint32_t g_upMaxNodes;   // GpuImplicit の節の上限(作業場の番地)
    uint32_t g_upMaxLinks;   // 隣の上限(作業用の語の番地)
    uint32_t g_upNodeCount;  // 段 0 の節の数
    uint32_t g_upTermStart;  // 右辺の項の始まり(語)
    uint32_t g_upRounding;   // 右辺の項の割り方(EQ_ROUNDING_*)
};

#define EQ_MAX_NODES g_upMaxNodes
#define EQ_MAX_LINKS g_upMaxLinks
#include "sim/equilibrium_core.hlsli"

static const uint32_t EQ_UPLOAD_NODE_WORDS = 18;
static const uint32_t EQ_UPLOAD_COEFFICIENTS = 2;  // 6 面の係数の始まり
static const uint32_t EQ_UPLOAD_NEIGHBORS = 14;    // 隣の節の始まり
static const uint32_t EQ_UPLOAD_TERMS = 17;        // 右辺の項の始まりと数
static const uint32_t EQ_UPLOAD_TERM_WORDS = 3;

FxU128 UpLoadWide(uint32_t word) {
    FxU128 value;
    value.hi = g_upStage[word];
    value.lo = g_upStage[word + 1];

    return value;
}

[numthreads(64, 1, 1)] void EquilibriumUploadNode(uint32_t node : SV_DispatchThreadID) {
    if (node >= g_upNodeCount)
        return;

    // --- 自分だけの係数・6 面の係数と隣 → 部品の段 0(対角 D・重み)---
    const uint32_t base = node * EQ_UPLOAD_NODE_WORDS;
    uint32_t neighbors[EQ_GPU_FACES_MAX];
    FxU128 coefficients[EQ_GPU_FACES_MAX];
    FX_UNROLL for (uint32_t f = 0; f < EQ_GPU_FACES_MAX; ++f) {
        coefficients[f] = UpLoadWide(base + EQ_UPLOAD_COEFFICIENTS + (2 * f));
        const uint64_t packed = g_upStage[base + EQ_UPLOAD_NEIGHBORS + (f / 2)];
        neighbors[f] = (f & 1) == 0 ? (uint32_t)packed : (uint32_t)(packed >> 32);
    }

    const FxU128 diagonal = EqGpuFinishNode(node, UpLoadWide(base), neighbors, coefficients);

    // --- 右辺 = Σ 項 ÷ D(CPU の SourceRhs・MakeLocal の固定した面の項と同じ関数。足す順は結果に効かない)---
    const uint64_t terms = g_upStage[base + EQ_UPLOAD_TERMS];
    const uint32_t first = (uint32_t)terms;
    const uint32_t count = (uint32_t)(terms >> 32);
    int64_t rhs = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t word = g_upTermStart + (EQ_UPLOAD_TERM_WORDS * (first + k));
        rhs += EqSourceTerm(g_upRounding, (int64_t)g_upStage[word], UpLoadWide(word + 1), diagonal);
    }

    EqGpuSetRhs(node, rhs);
}
