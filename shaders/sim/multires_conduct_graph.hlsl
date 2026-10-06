// multires_conduct_graph.hlsl — 熱の伝導の段(埋める・流れ・足す・小刻みの終わり〔T-0109〕)の Work Graph 版(T-0107。D-302 で Compute 版と比べる)。
// 活性の刻みで、伝導の一覧(MR_GRAPH_INPUT_CONDUCT_*。GPU の入力)のブロックを 1 グループずつ受け持つ。中身は Compute 版と同じ
// shaders/sim/multires_conduct.hlsli。入口ごとに見出しが別(同じレコードを指す)なので、段ごとに DispatchGraph を 1 回
// (段の間は UAV のバリア。流れは刻みの初めのセルから・足すのは流れが全部出揃ってから)。
// 選ぶのは GpuMultiresOptions::conductionGraph(gpu_multires.cpp の RecordConduction)。
#include "sim/multires_conduct.hlsli"

struct MrConductRecord {
    uint32_t slot;  // MR_NO_BLOCK なら何もしない(レコード 0)
};

// clang-format off

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(CONDUCT_LIGHT_THREADS, 1, 1)]
[NodeIsProgramEntry]
void ConductPrepareNode(DispatchNodeInputRecord<MrConductRecord> input, uint32_t thread : SV_GroupIndex) {
    const uint32_t slot = input.Get().slot;
    if (slot < g_blockCount)
        ConductPrepareBlock(slot, thread);
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(CONDUCT_THREADS, 1, 1)]
[NodeIsProgramEntry]
void ConductFlowsNode(DispatchNodeInputRecord<MrConductRecord> input, uint32_t thread : SV_GroupIndex) {
    const uint32_t slot = input.Get().slot;
    if (slot < g_blockCount)
        ConductFlowsBlock(slot, thread);
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(CONDUCT_THREADS, 1, 1)]
[NodeIsProgramEntry]
void ConductApplyNode(DispatchNodeInputRecord<MrConductRecord> input, uint32_t thread : SV_GroupIndex) {
    const uint32_t slot = input.Get().slot;
    if (slot < g_blockCount)
        ConductApplyBlock(slot, thread);
}

// 小刻みの終わり(T-0109)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(CONDUCT_LIGHT_THREADS, 1, 1)]
[NodeIsProgramEntry]
void ConductEndNode(DispatchNodeInputRecord<MrConductRecord> input, uint32_t thread : SV_GroupIndex) {
    const uint32_t slot = input.Get().slot;
    if (slot < g_blockCount)
        ConductEndBlock(slot, thread);
}

// clang-format on
