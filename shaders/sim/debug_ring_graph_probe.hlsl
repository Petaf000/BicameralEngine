// debug_ring_graph_probe.hlsl — デバッグのリングを Work Graphs のノードから書けるかを確かめる(T-0003)。
// Root(ブロードキャスト、64 スレッド × ROOT_GROUP_COUNT)の各スレッドが Leaf(スレッド起動)へ 1 から始まる値を渡し、
// Leaf がその値を DEBUG_PRINT する。CPU(tests/gpu_debug_ring_test.cpp)が 1〜256 が 1 回ずつ出たかを見る。
// グローバルのルート署名は u0 space1(リング)だけ。
#define BICAMERAL_GPU_DEBUG 1  // Release でもリングを使う(このシェーダーはリングの確認そのもの)
#include "common/debug_ring.hlsli"

#define ROOT_GROUP_COUNT 4

struct RootRecord {
    uint32_t unused;
};

struct LeafRecord {
    uint32_t value;
};

// clang-format は HLSL のノードの属性を並べ崩すので、属性つきの宣言だけ整形を止める
// clang-format off
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeDispatchGrid(ROOT_GROUP_COUNT, 1, 1)]
[NumThreads(64, 1, 1)]
void Root(DispatchNodeInputRecord<RootRecord> input, uint32_t dispatchThreadId : SV_DispatchThreadID,
          [MaxRecords(64)] [NodeId("Leaf")] NodeOutput<LeafRecord> leafOutput) {
    ThreadNodeOutputRecords<LeafRecord> record = leafOutput.GetThreadNodeOutputRecords(1);
    record.Get().value = dispatchThreadId + 1;
    record.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Leaf(ThreadNodeInputRecord<LeafRecord> input) {
    DEBUG_PRINT(DebugFormat::DebugRingGraphLeaf, input.Get().value);
}
// clang-format on
