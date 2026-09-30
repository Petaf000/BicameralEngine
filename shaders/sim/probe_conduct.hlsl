// probe_conduct.hlsl — 仮の刻みの伝導の単位を Work Graph で(T-0005。06 §2 段 2〜3・07 §1)。
// 「熱が広がっている所だけが次を起動する」伝播の最小形。規則(面の流れ・活性の取り方)は common/probe_sim.hlsli。
//
// データの流れ(1 刻み t に DispatchGraph を 1 回。engine/src/sim/probe_sim.cpp の RecordConduct):
//   活性の一覧 (t & 1)(刻み t − 1 の ConductBlock が足した「変わったブロック」+ 刻み t の適用が足した「つつかれたブロック」)
//   → DispatchGraph が一覧を GPU のメモリから入力として読む(D3D12_DISPATCH_MODE_NODE_GPU_INPUT。数は GPU が数えたもの。CPU は知らない)
//   → WakeBlocks(スレッド起動、1 スレッド = 一覧の 1 件): そのブロックと 6 面の隣を、この刻みでまだ予定していなければ予定する
//   → ConductBlock(1 レコード = 4³ のブロック = 1 グループ): 世代 (t & 1) を読み、世代 ((t + 1) & 1) に書く(gather。ADR-0003)。
//      値が 1 つでも変わったら、そのブロックを一覧 ((t + 1) & 1) へ → 次の刻みの入力
// 1 刻みの中の連鎖の深さは 2 で決まっている(伝播は刻みをまたいで進む。06 §2「深さ 32 まで」に当たらない)。
// ノードの実行の順番は決まらないが、各ブロックは自分のセルにだけ書き、予定と一覧は順番に依存しない(印は同じ値の上書き、一覧は順不同で
// 次の刻みの予定にだけ使う)ので、結果は決定的(04 R1〜R8)。整数だけ(D-205)。
// ノードごとの起動・レコードの数は common/work_graph_stats.hlsli で数える(T-0008。CPU がフレームごとにログへ)。
#include "sim/probe_bindings.hlsli"

struct BlockRecord {
    uint32_t block;  // ブロックの番号(ProbeBlockIndex)
};

// ブロックの座標(ProbeBlockIndex の逆)
uint3 BlockCoordinates(uint32_t block) {
    return uint3(block % PROBE_BLOCKS_PER_AXIS, (block / PROBE_BLOCKS_PER_AXIS) % PROBE_BLOCKS_PER_AXIS,
                 block / (PROBE_BLOCKS_PER_AXIS * PROBE_BLOCKS_PER_AXIS));
}

// この刻みでまだ予定していなければ予定する(印を「刻み + 1」にする。同じ刻みに何度来ても、最初の 1 回だけ true)。
// 予定した数は S(t + 1) の表の欄に数える(CPU のリファレンスが同じ数を予想する)
bool TrySchedule(uint32_t block, uint64_t tick) {
    const uint32_t mark = (uint32_t)tick + 1;
    uint32_t previous;
    InterlockedExchange(blockSchedule[block], mark, previous);
    if (previous == mark) return false;
    uint32_t count;
    hashes.InterlockedAdd(HashEntryAddress(tick + 1) + PROBE_HASH_OFFSET_SCHEDULED, 1, count);
    return true;
}

// 伝導の 1 セルが読む隣(格子の外なら自分 = 断熱)
uint32_t NeighborOrSelf(uint32_t base, int3 cell, int3 offset, uint32_t self) {
    const int3 neighbor = cell + offset;
    if (any(neighbor < 0) || any(neighbor >= (int)PROBE_GRID_SIZE)) return self;
    return world[base + ProbeCellIndex((uint32_t)neighbor.x, (uint32_t)neighbor.y, (uint32_t)neighbor.z)];
}

// 自分(0)と 6 面の隣(1〜6: −x, +x, −y, +y, −z, +z)
int3 FaceOffset(uint32_t index) {
    if (index == 0) return int3(0, 0, 0);
    const uint32_t axis = (index - 1) / 2;
    const int32_t step = ((index - 1) & 1) != 0 ? 1 : -1;
    return int3(axis == 0 ? step : 0, axis == 1 ? step : 0, axis == 2 ? step : 0);
}

groupshared uint32_t g_blockChanged;

// clang-format は HLSL のノードの属性を並べ崩すので、属性つきの宣言だけ整形を止める
// clang-format off

// 一覧の 1 件 → そのブロックと 6 面の隣(格子の中)のうち、まだ予定していないものを ConductBlock へ(最大 7 件)
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void WakeBlocks(ThreadNodeInputRecord<BlockRecord> input,
                [MaxRecords(7)] [NodeId("ConductBlock")] NodeOutput<BlockRecord> conductOutput) {
    const uint64_t tick = CurrentTick();
    const uint32_t block = input.Get().block;
    const bool valid = block < PROBE_BLOCK_COUNT;  // 一覧の先頭の PROBE_NO_BLOCK は何もしない
    DEBUG_ASSERT(valid || block == PROBE_NO_BLOCK, DebugFormat::ProbeBlockOutOfRange, tick, block);
    WgCountLaunch(PROBE_STATS_NODE_WAKE, 1);

    // --- 自分と 6 面の隣のうち、この刻みで初めて来たものを 1 件ずつ出す ---
    // 出力の呼び出しはスレッドグループで一様でなければならないので、7 回とも呼ぶ(出さない回は 0 件)。
    // 局所の配列に集めて 1 回で出す形は WARP で予定の数が合わなかった(T-0005)ので、展開した 1 件ずつの形にしている
    const int3 center = (int3)BlockCoordinates(valid ? block : 0);
    uint32_t emitted = 0;
    [unroll] for (uint32_t index = 0; index < PROBE_WAKE_MAX_RECORDS; ++index) {
        const int3 neighbor = center + FaceOffset(index);
        const bool inside = valid && all(neighbor >= 0) && all(neighbor < (int)PROBE_BLOCKS_PER_AXIS);
        const uint32_t target =
            inside ? ProbeBlockIndex((uint32_t)neighbor.x, (uint32_t)neighbor.y, (uint32_t)neighbor.z) : 0;
        const bool take = inside && TrySchedule(target, tick);
        ThreadNodeOutputRecords<BlockRecord> record = conductOutput.GetThreadNodeOutputRecords(take ? 1 : 0);
        if (take) record.Get().block = target;
        record.OutputComplete();
        emitted += take ? 1 : 0;
    }
    WgCountOutputs(PROBE_STATS_NODE_WAKE, emitted, emitted);  // 出す数は構造で 7 まで(上限を越えようがない)
}

// 1 ブロック(4³ セル)の伝導。値が変わったら、次の刻みの一覧へ
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(PROBE_BLOCK_SIZE, PROBE_BLOCK_SIZE, PROBE_BLOCK_SIZE)]
void ConductBlock(DispatchNodeInputRecord<BlockRecord> input, uint3 groupThreadId : SV_GroupThreadID,
                  uint32_t groupIndex : SV_GroupIndex) {
    const uint64_t tick = CurrentTick();
    const uint32_t block = input.Get().block;
    const uint32_t current = GenerationBase(tick);
    const uint32_t next = GenerationBase(tick + 1);
    const int3 cell = (int3)(BlockCoordinates(block) * PROBE_BLOCK_SIZE + groupThreadId);
    const uint32_t cellIndex = ProbeCellIndex((uint32_t)cell.x, (uint32_t)cell.y, (uint32_t)cell.z);

    if (groupIndex == 0) {
        g_blockChanged = 0;
        WgCountLaunch(PROBE_STATS_NODE_CONDUCT, 1);
    }
    GroupMemoryBarrierWithGroupSync();

    // --- 伝導(前の世代の自分と 6 面の隣から)---
    const uint32_t self = world[current + cellIndex];
    const uint32_t value = ProbeConductValue(self, NeighborOrSelf(current, cell, int3(-1, 0, 0), self),
                                             NeighborOrSelf(current, cell, int3(1, 0, 0), self),
                                             NeighborOrSelf(current, cell, int3(0, -1, 0), self),
                                             NeighborOrSelf(current, cell, int3(0, 1, 0), self),
                                             NeighborOrSelf(current, cell, int3(0, 0, -1), self),
                                             NeighborOrSelf(current, cell, int3(0, 0, 1), self));
    world[next + cellIndex] = value;

    // --- 変わったか(ブロックの中で 1 つでも)---
    if (WaveActiveAnyTrue(value != self) && WaveIsFirstLane()) InterlockedOr(g_blockChanged, 1);
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0 && g_blockChanged != 0) AppendActiveBlock((uint32_t)((tick + 1) & 1), block);
}
// clang-format on
