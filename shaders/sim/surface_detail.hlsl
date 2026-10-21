// surface_detail.hlsl — 表面の細かい形の整数の関数を GPU の Compute で呼ぶ(研究 R-T1・T-0879。23 §7)。
// 式は shaders/common/surface_detail.hlsli(CPU と同じソース)。ここは区画のデータの読み方と、2 つの入口だけ:
//   Contacts: 物の点 1 つ = 1 スレッド。地面とのめり込みと傾き(SdContactAt)を書く(当たりで関数を呼ぶ費用。合格の 1 つ目)
//   Openness: 柱 1 本 = 1 グループ(64 スレッド)。下から 4 つのセルの固い物の割合と面の開き(SdColumnOpennessAt)を書く(計算し直す費用。合格の 2 つ目)
// tests/gpu_surface_detail_test.cpp が CPU と 1 値ずつビット単位で比べ、時間を測る。
StructuredBuffer<uint32_t> g_columnHeights : register(t0);
StructuredBuffer<uint32_t> g_carveBegin : register(t1);
ByteAddressBuffer g_registered : register(t2);  // SdCarve(32 バイト)の並び
StructuredBuffer<uint32_t> g_gridIndex : register(t3);
StructuredBuffer<int32_t> g_gridNodes : register(t4);
ByteAddressBuffer g_config : register(t5);  // SdConfig(64 バイト)
ByteAddressBuffer g_points : register(t6);  // SdProbePoint(16 バイト)の並び

#define SD_COLUMN_HEIGHT(i) g_columnHeights[i]
#define SD_CARVE_BEGIN(i) g_carveBegin[i]
#define SD_CARVE(i) g_registered.Load<SdCarve>((i) * 32)
#define SD_GRID_INDEX(i) g_gridIndex[i]
#define SD_GRID_NODE(i) g_gridNodes[i]

#include "common/surface_detail.hlsli"

RWByteAddressBuffer g_contacts : register(u0);  // SdContact(16 バイト)× 点
RWByteAddressBuffer g_openness : register(u1);  // SdColumnOpenness(64 バイト)× 柱

cbuffer SurfaceDetailConstants : register(b0) {
    uint32_t g_mode;  // SD_MODE_*
    uint32_t g_pointCount;
    uint32_t g_reserved;
    int32_t g_bottomUm;  // 開き具合を測る下のセルの床(µm)
};

[numthreads(64, 1, 1)] void Contacts(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t index = dispatchThreadId.x;
    if (index >= g_pointCount)
        return;

    const SdConfig config = g_config.Load<SdConfig>(0);
    const SdProbePoint probe = g_points.Load<SdProbePoint>(index * 16);
    const SdContact contact = SdContactAt(config, g_mode, probe.xUm, probe.zUm, probe.yUm);

    g_contacts.Store<SdContact>(index * 16, contact);
}

// 1 グループ = 1 柱。64 スレッドが柱の中の 256 点を 4 点ずつ、前の 32 スレッドが面の 32 点を 1 点ずつ受け持ち、
// 埋まりの和を groupshared に原子的に足す(整数の足し算なので順によらない。04 R4)。最後にスレッド 0 が割合にする
groupshared uint32_t gs_sums[16];  // SdOpennessSums の並び(fill 4・openTop 4・faceX 4・faceZ 4)

[numthreads(64, 1, 1)] void Openness(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const SdConfig config = g_config.Load<SdConfig>(0);
    const uint32_t column = groupId.x;
    const uint32_t lane = threadId.x;
    const uint32_t columnX = column % config.columnsX;
    const uint32_t columnZ = column / config.columnsX;
    const int64_t bottom = (int64_t)g_bottomUm;
    if (lane < 16)
        gs_sums[lane] = 0;

    GroupMemoryBarrierWithGroupSync();

    SdOpennessSums sums = SdEmptySums();
    for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t sampleIndex = lane + (k * 64);
        sums = SdAddInterior(sums, SdInteriorGroundUm(config, g_mode, columnX, columnZ, sampleIndex), bottom);
    }

    if (lane < 32) {
        const uint32_t axisZ = lane / 16;
        sums = SdAddFace(sums, axisZ, SdFaceGroundUm(config, g_mode, columnX, columnZ, axisZ, lane % 16), bottom);
    }

    [unroll] for (uint32_t layer = 0; layer < SD_COLUMN_LAYERS; ++layer) {
        InterlockedAdd(gs_sums[layer], sums.fill[layer]);
        InterlockedAdd(gs_sums[4 + layer], sums.openTop[layer]);
        InterlockedAdd(gs_sums[8 + layer], sums.faceX[layer]);
        InterlockedAdd(gs_sums[12 + layer], sums.faceZ[layer]);
    }

    GroupMemoryBarrierWithGroupSync();
    if (lane != 0)
        return;

    SdOpennessSums total;
    [unroll] for (uint32_t i = 0; i < SD_COLUMN_LAYERS; ++i) {
        total.fill[i] = gs_sums[i];
        total.openTop[i] = gs_sums[4 + i];
        total.faceX[i] = gs_sums[8 + i];
        total.faceZ[i] = gs_sums[12 + i];
    }

    g_openness.Store<SdColumnOpenness>(column * 64, SdFinishOpenness(total));
}
