// work_graph_probe.hlsl — Work Graphs が動くかを確かめる一番小さなグラフ(T-0013)。
// Root(ブロードキャスト、64 スレッド × ROOT_GROUP_COUNT グループ)の各スレッドが Leaf(スレッド起動)へレコードを 1 つ出し、
// Leaf が 64bit の atomic で合計と個数を足す。CPU 側(tests/gpu_work_graph_test.cpp)が期待値と比べる。
// 確かめること: DispatchGraph が direct / compute のキューと WARP で動くか(06・16 の未確認)、ノード間のレコードの受け渡し、
// raw バッファへの 64bit atomic(SM 6.6 以上で必須)。
//
// results(u0)の中身: [0] Σ (value << VALUE_SHIFT)、[8] Leaf が走った回数

#define ROOT_GROUP_COUNT 16
#define VALUE_SHIFT 20  // 合計を 2^32 より大きくして、64bit の atomic の上位の桁も確かめる

struct RootRecord {
    uint32_t multiplier;
};

struct LeafRecord {
    uint32_t value;
};

RWByteAddressBuffer results : register(u0);

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
    record.Get().value = (dispatchThreadId + 1) * input.Get().multiplier;
    record.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Leaf(ThreadNodeInputRecord<LeafRecord> input) {
    uint64_t original;
    results.InterlockedAdd64(0, (uint64_t)input.Get().value << VALUE_SHIFT, original);
    results.InterlockedAdd64(8, 1, original);
}
// clang-format on
