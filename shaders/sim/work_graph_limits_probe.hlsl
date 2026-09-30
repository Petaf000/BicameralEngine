// work_graph_limits_probe.hlsl — Work Graphs のノードのカウンタと「上限の手前で止める」仕組みの試験(T-0008、16 §1)。
// CPU(tests/gpu_work_graph_stats_test.cpp)が Spawn へレコードを渡し、わざと上限に近づけたり越えさせたりして、
// カウンタ(common/work_graph_stats.hlsli)が CPU の予想と一致し、止めた分が数えられ、ノードの printf が場所つきで出るかを見る。
//
//   Spawn(スレッド起動・入口): 1 レコード → Fan へ 1 件、Chain へ 1 件
//   Fan(スレッド起動): 出したい数 request の Leaf を出す。宣言の上限 FAN_MAX_RECORDS を越える分は出さずに数える(越えると未定義の動作)
//   Leaf(スレッド起動): 数えるだけ
//   Chain(スレッド起動・再帰): remaining が残っていれば自分へ 1 件。深さの上限 CHAIN_MAX_DEPTH にいたら止めて数える
// カウンタの番号(NODE_*・GAUGE_*)と上限はテストの GraphStatsLayout と同じ。グローバルのルート署名は u0 space1(リング)と u1 space1(カウンタ)。
#define BICAMERAL_GPU_DEBUG 1  // Release でもリングを使う(ノードの printf の場所も確かめる)
#include "common/debug_ring.hlsli"
#include "common/work_graph_stats.hlsli"

#define NODE_SPAWN 0
#define NODE_FAN 1
#define NODE_LEAF 2
#define NODE_CHAIN 3
#define GAUGE_FAN_REQUEST 0
#define FAN_MAX_RECORDS 4
#define CHAIN_MAX_DEPTH 8

struct SpawnRecord {
    uint32_t id;
    uint32_t fanRequest;    // Fan が出したがる Leaf の数
    uint32_t chainRequest;  // Chain が自分へ出したがる回数
};

struct FanRecord {
    uint32_t id;
    uint32_t request;
};

struct LeafRecord {
    uint32_t id;
};

struct ChainRecord {
    uint32_t id;
    uint32_t remaining;  // まだ自分へ出したい回数
};

// clang-format は HLSL のノードの属性を並べ崩すので、属性つきの宣言だけ整形を止める
// clang-format off
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]

void Spawn(ThreadNodeInputRecord<SpawnRecord> input,
           [MaxRecords(1)] [NodeId("Fan")] NodeOutput<FanRecord> fanOutput,
           [MaxRecords(1)] [NodeId("Chain")] NodeOutput<ChainRecord> chainOutput) {
    const SpawnRecord spawn = input.Get();
    WgCountLaunch(NODE_SPAWN, 1);
    WgCountOutputs(NODE_SPAWN, 2, 2);

    ThreadNodeOutputRecords<FanRecord> fan = fanOutput.GetThreadNodeOutputRecords(1);
    fan.Get().id = spawn.id;
    fan.Get().request = spawn.fanRequest;
    fan.OutputComplete();

    ThreadNodeOutputRecords<ChainRecord> chain = chainOutput.GetThreadNodeOutputRecords(1);
    chain.Get().id = spawn.id;
    chain.Get().remaining = spawn.chainRequest;
    chain.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]

void Fan(ThreadNodeInputRecord<FanRecord> input,
         [MaxRecords(FAN_MAX_RECORDS)] [NodeId("Leaf")] NodeOutput<LeafRecord> leafOutput) {
    const FanRecord fan = input.Get();
    WgCountLaunch(NODE_FAN, 1);
    WgGaugePeak(GAUGE_FAN_REQUEST, fan.request);

    // --- 上限の手前で止める(越えた分は出さない。CPU が Warning を出す)---
    const uint32_t granted = WgGrantOutputs(NODE_FAN, fan.request, FAN_MAX_RECORDS);
    if (granted < fan.request)
        DEBUG_PRINT(DebugFormat::WgLimitsFanRefused, fan.id, fan.request, (uint32_t)FAN_MAX_RECORDS);

    ThreadNodeOutputRecords<LeafRecord> leaves = leafOutput.GetThreadNodeOutputRecords(granted);
    for (uint32_t index = 0; index < granted; ++index)
        leaves.Get(index).id = fan.id;

    leaves.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Leaf(ThreadNodeInputRecord<LeafRecord> input) {
    WgCountLaunch(NODE_LEAF, 1);
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeMaxRecursionDepth(CHAIN_MAX_DEPTH)]

void Chain(ThreadNodeInputRecord<ChainRecord> input,
           [MaxRecords(1)] [NodeId("Chain")] NodeOutput<ChainRecord> chainOutput) {
    const ChainRecord chain = input.Get();
    WgCountLaunch(NODE_CHAIN, 1);

    // --- 深さの上限にいたら自分へ出さない(越えると未定義の結果)---
    const uint32_t remainingLevels = GetRemainingRecursionLevels();
    const bool wants = chain.remaining > 0;
    const bool recurse = WgTryRecurse(NODE_CHAIN, remainingLevels, CHAIN_MAX_DEPTH, wants);
    if (wants && !recurse)
        DEBUG_PRINT(DebugFormat::WgLimitsChainRefused, chain.id, (uint32_t)CHAIN_MAX_DEPTH - remainingLevels);

    WgCountOutputs(NODE_CHAIN, recurse ? 1u : 0u, recurse ? 1u : 0u);

    ThreadNodeOutputRecords<ChainRecord> next = chainOutput.GetThreadNodeOutputRecords(recurse ? 1 : 0);
    if (recurse) {
        next.Get().id = chain.id;
        next.Get().remaining = chain.remaining - 1;
    }

    next.OutputComplete();
}

// clang-format on
