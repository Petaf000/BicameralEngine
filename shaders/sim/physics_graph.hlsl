// physics_graph.hlsl — 物理の 1 刻みの「広域の選別 → 接触の幾何」と「色ごとの解」を Work Graph で(T-0092。08 §2 の 1〜2・4)。
// 入口は 2 つ: BroadphaseNode(CPU の入力)と SolveBodyNode(色ごとの GPU の入力)。下は広域の選別の説明。
// Compute 版(physics_step.hlsl の Broadphase・Narrowphase)と同じ関数(physics_bindings.hlsli の CollectCandidates・CollideGeometry・BuildSlot)・
// 同じ結果(CPU とビット一致。tests/gpu_physics_test.cpp)。
//
// データの流れ(小刻みに DispatchGraph を 1 回 + Compute 1 パス。engine/src/sim/gpu_physics.cpp の RecordSubstep):
//   CPU の入力 1 件(グループの数)→ BroadphaseNode(1 スレッド = 持ち主の物 1 つ): 近い相手を番号の昇順に枠へ入れ、
//   使わない枠を空にし、枠ごとに 1 件を NarrowphaseNode へ → NarrowphaseNode(1 スレッド = 枠 1 つ): 接触の幾何を g_geometry へ
//   → (グラフの後)Compute の BuildManifolds が枠ごとに組を作って前の小刻みから引き継ぐ。
// 組を作る所をノードに入れない理由: 組(3 KB)を局所に持つ関数をノードの中で走らせると GPU が固まった(RTX 3070 Ti / 32.0.15.9597。
// ノードの局所の配列が約 5〜6 KB を超えると固まる。同じコードは Compute なら動く。T-0092・08 §2)。
// Compute 版との違いは、接触の幾何が「全部の枠」ではなく「相手のいる枠」だけを起動すること。
// 順番に依存しない理由は Compute 版と同じ(組は自分の枠にだけ書く、統計と β の質量は原子的な加算・最大値、物の一覧は読む側が順番に依存しない)。
#include "sim/physics_bindings.hlsli"

// 広域の選別の 1 グループのスレッド数。ブロードキャストのノードの出力は 1 グループ 256 件まで(仕様)なので、
// 16 スレッド × 枠 16 = 256(gpu_physics.cpp の BROADPHASE_THREADS と同じ)
static const uint32_t PHYSICS_BROADPHASE_THREADS = 16;

struct PhysicsGridRecord {
    uint32_t groupCount : SV_DispatchGrid;  // ceil(物の数 / PHYSICS_BROADPHASE_THREADS)
};

struct PhysicsBodyRecord {
    uint32_t body;
};

struct PhysicsPairRecord {
    uint32_t slot;   // 持ち主の枠(持ち主 × slotsPerBody + k)
    uint32_t other;  // 相手の物
};

// clang-format は HLSL のノードの属性を並べ崩すので、属性つきの宣言だけ整形を止める
// clang-format off
// 持ち主の物 1 つ → 相手のいる枠ごとに 1 件(最大 PX_GPU_MAX_SLOTS_PER_BODY 件)。使わない枠は空にする
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(1024, 1, 1)]
[NumThreads(PHYSICS_BROADPHASE_THREADS, 1, 1)]
[NodeIsProgramEntry]

void BroadphaseNode(DispatchNodeInputRecord<PhysicsGridRecord> input, uint3 id : SV_DispatchThreadID,
                    [MaxRecords(PHYSICS_BROADPHASE_THREADS * PX_GPU_MAX_SLOTS_PER_BODY)] [NodeId("NarrowphaseNode")] NodeOutput<PhysicsPairRecord> pairs) {
    const uint32_t i = id.x;
    const bool valid = i < g_bodyCount;
    const uint32_t count = valid ? CollectCandidates(i) : 0;
    if (valid) {
        g_slotCounts[g_currentHalf * g_bodyCount + i] = count;
        for (uint32_t k = count; k < g_slotsPerBody; ++k)
            g_manifolds[ManifoldIndex(g_currentHalf, i * g_slotsPerBody + k)].count = 0;
    }

    // 出力の呼び出しはスレッドグループで一様でなければならないので、全部のスレッドが同じ回数呼ぶ(出さない回は 0 件)。
    // 局所の配列に集めて 1 回で出す形は WARP で数が合わなかった(T-0005)ので、1 件ずつの形にしている
    [unroll] for (uint32_t k = 0; k < PX_GPU_MAX_SLOTS_PER_BODY; ++k) {
        const bool take = k < count;
        ThreadNodeOutputRecords<PhysicsPairRecord> record = pairs.GetThreadNodeOutputRecords(take ? 1 : 0);
        if (take) {
            record.Get().slot = i * g_slotsPerBody + k;
            record.Get().other = g_candidates[i * g_slotsPerBody + k];
        }

        record.OutputComplete();
    }
}

// 枠 1 つ(相手がいる)の接触の幾何
[Shader("node")]
[NodeLaunch("thread")]

void NarrowphaseNode(ThreadNodeInputRecord<PhysicsPairRecord> input) {
    g_geometry[input.Get().slot] = CollideGeometry(input.Get().slot, input.Get().other);
}

// 色ごとの解き方(GpuPhysicsSolver::Graph): 色 c の物の一覧(FinishColoring が作る)を GPU の入力にして、1 レコード = 1 物 = 1 グループ。
// Compute の SolveColor(physics_step.hlsl)と同じ関数(SolveBodyInGroup)。全部の物を起動して色で弾く代わりに、その色の物だけを起動する
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(SOLVE_GROUP_THREADS, 1, 1)]
[NodeIsProgramEntry]

void SolveBodyNode(DispatchNodeInputRecord<PhysicsBodyRecord> input, uint32_t thread : SV_GroupIndex) {
    SolveBodyInGroup(input.Get().body, thread);
}

// clang-format on
