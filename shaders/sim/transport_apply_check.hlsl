// transport_apply_check.hlsl — 当てる段(common/transport_apply.hlsli の TxFlowCell)と伝導の種類(transport_conduction.hlsli)が
// HLSL でもコンパイルできることを確かめるだけのシェーダー。どの段も読み込まない(実行しない)。GPU の伝導の段が当てる段を呼ぶようになったら消す。
#include "common/transport_conduction.hlsli"

StructuredBuffer<MrBlock> g_blocks : register(t0);
StructuredBuffer<CvCellView> g_views : register(t1);
StructuredBuffer<MrFaceNeighbor> g_neighbors : register(t2);
RWStructuredBuffer<MrEnergyDelta> g_deltas : register(u0);
RWStructuredBuffer<uint32_t> g_moved : register(u1);

cbuffer RootConstants : register(b0) {
    uint32_t g_cellCount;
    int32_t g_subcycleBaseLevel;
    uint32_t g_maxSubcycleGap;
};

// 世界の約束(transport_apply.hlsli の先頭)。面の隣は前もって探した表から読む(確かめるだけ)
struct CheckWorld {
    uint32_t unused;

    MrFaceNeighbor Neighbor(uint32_t slot, MrBlock block, uint32_t index, uint32_t face) {
        return g_neighbors[(slot * MR_BLOCK_CELLS + index) * MR_FACES + face];
    }

    CvCellView View(uint32_t slot, uint32_t index) { return g_views[slot * MR_BLOCK_CELLS + index]; }
    int32_t Level(uint32_t slot) { return g_blocks[slot].level; }
    bool HasFraction(uint32_t slot) { return g_blocks[slot].fraction != MR_NO_FRACTION; }
    TxLimit Limit(uint32_t slot, uint32_t index) { return TxNoLimit(); }
    void FollowFields(uint32_t slot, uint32_t index) {}

    // 開いた世界の端(T-0721): 外のセルの見立て(確かめるだけなので見立ての表の行 row)・帳簿(確かめるだけなので書かない)
    CvCellView Outside(uint32_t row, int32_t level) { return g_views[row]; }
    void AddEdge(uint32_t slot, uint32_t index, int64_t flow) {}

    // 確かめるだけなので書き足しは順に依存する形(GPU の段は 64bit の atomic の足し算にする)
    void Add(uint32_t slot, uint32_t index, MrEnergyDelta delta) {
        const uint32_t address = slot * MR_BLOCK_CELLS + index;
        g_deltas[address] = MrAddEnergyDelta(g_deltas[address], delta);
    }
};

[numthreads(64, 1, 1)] void Main(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= g_cellCount)
        return;

    CheckWorld world;
    world.unused = 0;
    TxConduction kind = TxMakeConduction(g_subcycleBaseLevel, g_maxSubcycleGap);
    const uint32_t slot = thread.x / MR_BLOCK_CELLS;
    const uint32_t index = thread.x % MR_BLOCK_CELLS;
    g_moved[thread.x] = TxFlowCell(kind, world, slot, g_blocks[slot], index) ? 1u : 0u;
}
