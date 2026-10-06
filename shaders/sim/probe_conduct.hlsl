// probe_conduct.hlsl — 仮の刻みの伝導と反応の単位を Work Graph で(T-0005・T-0089。06 §2 段 2〜4・07 §1・02 §3・§6)。
// 「熱が広がっている所・反応が進んでいる所だけが次を起動する」伝播の最小形。
// 規則は common/probe_world.hlsli(1 セルの伝導 + 反応)と common/probe_sim.hlsli(活性の取り方)。
//
// データの流れ(1 刻み t に DispatchGraph を 1 回。engine/src/sim/probe_sim.cpp の RecordConduct):
//   活性の一覧 (t & 1)(刻み t − 1 の ConductBlock が足した「変わった・次の刻みに評価の要るブロック」+ 刻み t の適用が足した「つつかれたブロック」
//   + 適用の単位の WakeDueBlocks が足した「次に評価の要る刻みが来たブロック」。待ちの丸め。ADR-0018・T-0122)
//   → DispatchGraph が一覧を GPU のメモリから入力として読む(D3D12_DISPATCH_MODE_NODE_GPU_INPUT。数は GPU が数えたもの。CPU は知らない)
//   → WakeBlocks(スレッド起動、1 スレッド = 一覧の 1 件): そのブロックと 6 面の隣を、この刻みでまだ予定していなければ予定する
//   → ConductBlock(1 レコード = 4³ のブロック = 1 グループ): 世代 (t & 1) のセルと熱のキャッシュを読み、
//      温度の差で熱を受け渡してから反応を評価し、世代 ((t + 1) & 1) に書く(gather。ADR-0003)。
//      値が 1 つでも変わったか、次の刻みに評価が要れば、そのブロックを一覧 ((t + 1) & 1) へ → 次の刻みの入力。
//      そうでなければ、次に評価の要る刻みの印(セルの最小)を書いて眠らせる(WakeDueBlocks がその刻みに起こす)
// 1 刻みの中の連鎖の深さは 2 で決まっている(伝播は刻みをまたいで進む。06 §2「深さ 32 まで」に当たらない)。
// ノードの実行の順番は決まらないが、各ブロックは自分のセルにだけ書き、予定と一覧は順番に依存しない(印は同じ値の上書き、一覧は順不同で
// 次の刻みの予定にだけ使う)ので、結果は決定的(04 R1〜R8)。整数だけ(D-205)。
// ノードごとの起動・レコードの数は common/work_graph_stats.hlsli で数える(T-0008。CPU がフレームごとにログへ)。
// 連鎖のトレース(T-0087): 範囲が有効なら、WakeBlocks は「起こそうとした隣」を全部、ConductBlock は「計算した・変わったか」を書く
// (common/graph_trace.hlsli。種類は probe_sim.hlsli の PROBE_TRACE_*。木に組むのは CPU の sim/probe_trace)。
#include "sim/probe_bindings.hlsli"

struct BlockRecord {
    uint32_t block;  // ブロックの番号(ProbeBlockIndex)
};

// この刻みでまだ予定していなければ予定する(印を「刻み + 1」にする。同じ刻みに何度来ても、最初の 1 回だけ true)。
// 予定した数は S(t + 1) の表の欄に数える(CPU のリファレンスが同じ数を予想する)
bool TrySchedule(uint32_t block, uint64_t tick) {
    const uint32_t mark = (uint32_t)tick + 1;
    uint32_t previous;
    InterlockedExchange(blockSchedule[block], mark, previous);
    if (previous == mark)
        return false;

    uint32_t count;
    hashes.InterlockedAdd(HashEntryAddress(tick + 1) + PROBE_HASH_OFFSET_SCHEDULED, 1, count);

    return true;
}

// 伝導の 1 セルが読む隣の熱のキャッシュ(格子の外なら自分 = 温度の差 0 = 断熱)
HcThermalCache NeighborOrSelf(uint32_t base, int3 cell, int3 offset, HcThermalCache self) {
    const int3 neighbor = cell + offset;
    if (any(neighbor < 0) || any(neighbor >= (int)PROBE_GRID_SIZE))
        return self;

    return thermal[base + ProbeCellIndex((uint32_t)neighbor.x, (uint32_t)neighbor.y, (uint32_t)neighbor.z)];
}

// 自分(0)と 6 面の隣(1〜6: −x, +x, −y, +y, −z, +z)
int3 FaceOffset(uint32_t index) {
    if (index == 0)
        return int3(0, 0, 0);

    const uint32_t axis = (index - 1) / 2;
    const int32_t step = ((index - 1) & 1) != 0 ? 1 : -1;

    return int3(axis == 0 ? step : 0, axis == 1 ? step : 0, axis == 2 ? step : 0);
}

// 格子の中の隣(index 番目。0 = 自分)なら true と番号
bool NeighborBlock(int3 center, uint32_t index, out uint32_t target) {
    const int3 neighbor = center + FaceOffset(index);
    target = 0;
    if (any(neighbor < 0) || any(neighbor >= (int)PROBE_BLOCKS_PER_AXIS))
        return false;

    target = ProbeBlockIndex((uint32_t)neighbor.x, (uint32_t)neighbor.y, (uint32_t)neighbor.z);

    return true;
}

// 連鎖のトレース: 一覧の 1 件 block が起こそうとした隣(格子の中を全部)を書く。どちらかが範囲の箱に入るものだけ。
// ウェーブの全部のレーンが呼ぶ(GtReserve)。書かないレーンは valid = false
void TraceWakes(uint64_t tick, bool valid, uint32_t block, int3 center) {
    const bool sourceWanted = valid && TraceWantsBlock(block);
    uint32_t wantedMask = 0;
    [unroll] for (uint32_t index = 0; index < PROBE_WAKE_MAX_RECORDS; ++index) {
        uint32_t target;
        const bool inside = valid && NeighborBlock(center, index, target);
        if (inside && (sourceWanted || TraceWantsBlock(target)))
            wantedMask |= 1u << index;
    }

    uint32_t slot = GtReserve(countbits(wantedMask));
    [unroll] for (uint32_t index = 0; index < PROBE_WAKE_MAX_RECORDS; ++index) {
        uint32_t target;
        NeighborBlock(center, index, target);
        if ((wantedMask & (1u << index)) != 0)
            GtStore(slot++, tick, PROBE_TRACE_WAKE, block, target);
    }
}

groupshared uint32_t g_blockChanged;
groupshared uint32_t g_wakeHigh;  // 次に評価の要る刻みの印のブロックの最小(上位・下位 32bit。BlockMinTick)
groupshared uint32_t g_wakeLow;

// ブロックの 64bit の最小(全部のスレッドが呼ぶ)。上位 32bit の最小を取り、その上位を持つスレッドの下位の最小を取る
// (どちらも順によらない。64bit の atomic とウェーブの演算を避ける。HW の 64bit の不具合は T-0124。multires_conduct.hlsli の GroupMinTick と同じ形)
uint64_t BlockMinTick(uint32_t groupIndex, uint64_t value) {
    if (groupIndex == 0) {
        g_wakeHigh = 0xFFFFFFFFu;
        g_wakeLow = 0xFFFFFFFFu;
    }

    GroupMemoryBarrierWithGroupSync();
    const uint32_t high = (uint32_t)(value >> 32);
    const uint32_t waveHigh = WaveActiveMin(high);
    if (WaveIsFirstLane())
        InterlockedMin(g_wakeHigh, waveHigh);

    GroupMemoryBarrierWithGroupSync();
    const uint32_t low = high == g_wakeHigh ? (uint32_t)value : 0xFFFFFFFFu;
    const uint32_t waveLow = WaveActiveMin(low);
    if (WaveIsFirstLane())
        InterlockedMin(g_wakeLow, waveLow);

    GroupMemoryBarrierWithGroupSync();

    return ((uint64_t)g_wakeHigh << 32) | (uint64_t)g_wakeLow;
}

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
        const uint3 neighborBlock = (uint3)neighbor;  // 外なら使わない(負の値が大きな数になるだけ)
        const uint32_t target = inside ? ProbeBlockIndex(neighborBlock.x, neighborBlock.y, neighborBlock.z) : 0;
        const bool take = inside && TrySchedule(target, tick);
        ThreadNodeOutputRecords<BlockRecord> record = conductOutput.GetThreadNodeOutputRecords(take ? 1 : 0);
        if (take)
            record.Get().block = target;

        record.OutputComplete();
        emitted += take ? 1 : 0;
    }

    WgCountOutputs(PROBE_STATS_NODE_WAKE, emitted, emitted);  // 出す数は構造で 7 まで(上限を越えようがない)

    // 範囲の刻みはディスパッチの中で同じ(ルート定数と見出し)なので、ここの分岐はウェーブで一様
    if (GtWantsTick(tick))
        TraceWakes(tick, valid, block, center);
}

// 1 ブロック(4³ セル)の伝導と反応。値が変わったか、次の刻みに評価が要れば、次の刻みの一覧へ。そうでなければ起こす刻みを書く
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

    // ブロックが最後に変わった刻みの印(刻みの初めの値。書くのはこのグループだけで、読んだ後のバリアの後に書く)
    const uint64_t changedMark = LoadBlockMark(PROBE_SCHEDULE_CHANGED_WORD, block);

    // --- 伝導と反応(前の世代の自分と 6 面の隣から)---
    const HcThermalCache self = thermal[current + cellIndex];
    const ProbeCellStep step = ProbeStepCell(
        ReactionTable(), cells[current + cellIndex], self, NeighborOrSelf(current, cell, int3(-1, 0, 0), self),
        NeighborOrSelf(current, cell, int3(1, 0, 0), self), NeighborOrSelf(current, cell, int3(0, -1, 0), self),
        NeighborOrSelf(current, cell, int3(0, 1, 0), self), NeighborOrSelf(current, cell, int3(0, 0, -1), self),
        NeighborOrSelf(current, cell, int3(0, 0, 1), self), tick, changedMark, cellIndex);

    cells[next + cellIndex] = step.cell;
    thermal[next + cellIndex] = step.cache;

    // --- 変わったか(ブロックの中で 1 つでも)・次に評価の要る刻み(セルの最小)---
    if (WaveActiveAnyTrue(step.changed != 0) && WaveIsFirstLane())
        InterlockedOr(g_blockChanged, 1);

    const uint64_t wakeTick = BlockMinTick(groupIndex, step.wakeTick);  // 中のバリアで g_blockChanged も出来上がる
    if (groupIndex != 0)
        return;

    const uint64_t nextMark = ProbeChangeMark(tick + 1);
    const bool changed = g_blockChanged != 0;
    const bool possible = wakeTick <= nextMark;
    if (changed)
        StoreBlockMark(PROBE_SCHEDULE_CHANGED_WORD, block, ProbeChangeMark(tick));

    // 次の刻みの一覧に足すブロックは、起こす刻みを「無い」にする(WakeDueBlocks と重ならない。次の刻みに計算して書き直す)
    StoreBlockMark(PROBE_SCHEDULE_WAKE_WORD, block, changed || possible ? RX_WAIT_NEVER : wakeTick);
    if (changed || possible)
        AppendActiveBlock((uint32_t)((tick + 1) & 1), block);

    if (GtWantsTick(tick) && TraceWantsBlock(block))
        GtRecord(tick, PROBE_TRACE_CONDUCT, block,
                 (changed ? PROBE_BLOCK_FLAG_CHANGED : 0) | (possible ? PROBE_BLOCK_FLAG_POSSIBLE : 0));
}

// clang-format on
