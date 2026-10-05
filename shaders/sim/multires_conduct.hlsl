// multires_conduct.hlsl — 多重解像度の木の上の熱の伝導の Compute の段(T-0107。中身は shaders/sim/multires_conduct.hlsli)。
// 1 グループ = 1 ブロック。全部を刻む時はグループ g = 枠 g、活性の時はグループ g = 伝導の一覧のレコード g + 1
// (数は GPU が決めるので、全部の枠の数だけグループを投げ、数を超えたグループは何もしない)。呼ぶ順は gpu_multires.cpp の RecordConduction。
#include "sim/multires_conduct.hlsli"

// グループ group の受け持つ枠
bool ConductSlotOf(uint32_t group, out uint32_t slot) {
    slot = MR_NO_BLOCK;
    if (!IsListedStep()) {
        slot = group;
        return slot < g_blockCount;
    }

    const uint32_t record = group + 1;
    if (record >= g_graphInput.Load(MR_GRAPH_INPUT_CONDUCT_HEADER + 4))
        return false;

    slot = g_graphInput.Load(ConductRecordsOffset() + 4 * record);

    return slot < g_blockCount;
}

// 伝導の一覧を空にし、観察の枠を全部入れる(活性の刻みの初め。1 グループ)
[numthreads(CONDUCT_THREADS, 1, 1)] void ConductBegin(uint32_t thread : SV_GroupIndex) {
    const uint32_t observers = g_blockCount - g_worldBlocks;
    for (uint32_t i = thread; i < observers; i += CONDUCT_THREADS)
        g_graphInput.Store(ConductRecordsOffset() + 4 * (i + 1), g_worldBlocks + i);

    if (thread != 0)
        return;

    g_graphInput.Store(ConductRecordsOffset(), MR_NO_BLOCK);  // レコードを 0 件にしない
    g_graphInput.Store(MR_GRAPH_INPUT_CONDUCT_HEADER + 4, observers + 1);
}

    // clang-format off
[numthreads(CONDUCT_THREADS, 1, 1)]
void ConductMark(uint3 group : SV_GroupID, uint32_t thread : SV_GroupIndex) {
    uint32_t slot;
    if (ConductSlotOf(group.x, slot))
        ConductMarkBlock(slot, thread);
}

[numthreads(CONDUCT_THREADS, 1, 1)]
void ConductPrepare(uint3 group : SV_GroupID, uint32_t thread : SV_GroupIndex) {
    uint32_t slot;
    if (ConductSlotOf(group.x, slot))
        ConductPrepareBlock(slot, thread);
}

[numthreads(CONDUCT_THREADS, 1, 1)]
void ConductFlows(uint3 group : SV_GroupID, uint32_t thread : SV_GroupIndex) {
    uint32_t slot;
    if (ConductSlotOf(group.x, slot))
        ConductFlowsBlock(slot, thread);
}

[numthreads(CONDUCT_THREADS, 1, 1)]
void ConductApply(uint3 group : SV_GroupID, uint32_t thread : SV_GroupIndex) {
    uint32_t slot;
    if (ConductSlotOf(group.x, slot))
        ConductApplyBlock(slot, thread);
}
// clang-format on
