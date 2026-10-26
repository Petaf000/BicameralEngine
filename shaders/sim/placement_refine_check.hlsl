// placement_refine_check.hlsl — 細かくする時の「置かれ方で配る」を GPU で走らせ、CPU リファレンスと比べるための入口(T-1159)。
// 1 グループ = 親のセル 8 つ・1 スレッド = 子 1 つ(子の番号は MrChildCell の j)。回しは shaders/sim/placement_refine.hlsli、
// 式は shaders/common/state_field.hlsli(どちらもこの入口が DXC で通ることで HLSL として確かめる)。
// tests/gpu_placement_refine_test.cpp が sim::PlaceRefineCell・PlaceChildWeights と 1 値ずつビット単位で比べる。
//   t0: 物質(12 バイト = 種類・密度・詰まり)× 物質の数
//   t1: 親のセル((物質の数 + 3) × 8 バイト = 物を含む・x の傾き・z の傾き・物質ごとの量 mg)× セルの数
//   u0: 子((物質の数 + 3) × 8 バイト = 寄せたか・凝縮相の量・空きの体積・物質ごとの量 mg)× 子 8 × セルの数
ByteAddressBuffer g_materials : register(t0);
ByteAddressBuffer g_parents : register(t1);
RWByteAddressBuffer g_children : register(u0);

cbuffer PlacementConstants : register(b0) {
    uint32_t g_materialCount;
    uint32_t g_cellCount;
    uint32_t g_childVolumeLow;   // 子 1 つの体積(mm³)の下の 32bit
    uint32_t g_childVolumeHigh;  // 上の 32bit
};

static uint32_t s_parentBase;  // 今のスレッドの親のセルの番地(物質の量の先頭)

#define PR_MATERIAL_COUNT g_materialCount
#define PR_MATERIAL(m) g_materials.Load3((m) * 12)
#define PR_PARENT_MASS(m) g_parents.Load<int64_t>(s_parentBase + ((m) * 8))

#include "sim/placement_refine.hlsli"

static const uint32_t CELLS_PER_GROUP = 8;
groupshared uint64_t gs_open[CELLS_PER_GROUP * 8];

[numthreads(CELLS_PER_GROUP * 8, 1, 1)] void Main(uint32_t thread : SV_GroupIndex, uint3 group : SV_GroupID) {
    const uint32_t cellIndex = (group.x * CELLS_PER_GROUP) + (thread >> 3);
    const uint32_t j = thread & 7u;
    const bool active = cellIndex < g_cellCount;
    const uint32_t stride = (g_materialCount + 3) * 8;
    const uint32_t parentAddress = (active ? cellIndex : 0) * stride;
    s_parentBase = parentAddress + 24;

    // --- 配り方と子 j の重み ---
    const uint64_t childVolume = ((uint64_t)g_childVolumeHigh << 32) | (uint64_t)g_childVolumeLow;
    const bool blocked = g_parents.Load<uint64_t>(parentAddress) != 0;
    const PrCell cell = PrMakeCell(blocked, g_parents.Load<int64_t>(parentAddress + 8),
                                   g_parents.Load<int64_t>(parentAddress + 16), childVolume);
    const PrColumn column = PrMakeColumn(cell, j & 1u, j >> 2);
    const PrChildWeights weights = PrChildWeightsOf(cell, column, j);
    gs_open[thread] = weights.openMm3;
    GroupMemoryBarrierWithGroupSync();
    if (!active)
        return;

    // --- 子 8 つの空きの体積(同じ親のセルの子はグループの中で並んでいる)---
    const uint32_t first = thread & ~7u;
    uint64_t openSum = 0;
    FX_UNROLL
    for (uint32_t k = 0; k < 8; ++k)
        openSum += gs_open[first + k];

    // --- 書く: 寄せたか・重み・物質ごとの量 ---
    const uint32_t childAddress = ((cellIndex * 8) + j) * stride;
    g_children.Store<uint64_t>(childAddress, cell.settled ? 1u : 0u);
    g_children.Store<uint64_t>(childAddress + 8, weights.condensedMg);
    g_children.Store<uint64_t>(childAddress + 16, weights.openMm3);
    for (uint32_t m = 0; m < g_materialCount; ++m) {
        int64_t mass = PrChildMass(cell, column, m, j);
        if (cell.settled && PR_MATERIAL(m).x == SF_SETTLE_GAS) {
            const int64_t parentMass = PR_PARENT_MASS(m);
            int64_t othersGiven = 0;
            if (openSum != 0) {
                FX_UNROLL
                for (uint32_t k = 0; k < 7; ++k)
                    othersGiven += FxMulDivS64(parentMass, gs_open[first + k], openSum);
            }

            mass = PrGasShare(mass, parentMass, weights.openMm3, openSum, othersGiven, j);
        }

        g_children.Store<int64_t>(childAddress + 24 + (m * 8), mass);
    }
}
