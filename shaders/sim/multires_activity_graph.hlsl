// multires_activity_graph.hlsl — 多重解像度の木の上の活性: 種とその面の隣だけを刻む Work Graph(T-0100。17 §5「活性」)。
// 入口は 3 つ: ActivitySeedNode(GPU の入力 = この刻みの種の一覧。→ WakeFaceNode〔面を細かい側へたどる再帰〕→ ActivityStepNode)・
// ObserverStepNode(CPU の入力。観察の枠を全部刻む)・ExpandStepNode(GPU の入力 = TreeExpand が頁を配った一様なブロック。T-0102)。
// 一様なブロックは ActivityStepNode が値 1 つで反応が進むかを調べ、進むなら頁に広げる印を付けるだけ(頁は枠の順に配るので、
// 順の決まらないこのグラフの中では配らない)。TreeExpand の後、ExpandStepNode が埋めて刻む。CPU リファレンスは engine/src/sim/multires_activity.cpp の StepActive(同じ関数)。
// 刻むノードは進める反応の規則があったブロックを次の刻みの種の一覧(u13)へ書き足す。
// 細かいレベルの熱の刻み(T-0109)では、小刻みの終わりに変わったブロックの一覧を GPU の入力にして ActivitySeedNode をもう一度投げ、
// 面の隣を起こす(stepFlags の MR_STEP_SUBSTEP_WAKE。CPU の WakeChangedBlocks)。
// 熱の伝導を入れる刻み(T-0107)では、刻むノードは伝導の一覧(MR_GRAPH_INPUT_CONDUCT_*)に足すだけで、刻むのは multires_conduct.hlsl の段。
// セルが変わったブロックと、つつかれた種には忙しさの印(busyTick)を書く(静かな葉を粗くする要求の元。T-0101)。
// 木の管理のグラフ(multires_graph.hlsl)と分けたのは、反応の核を含んで大きく(debug の GPU-based validation の計装が数分かかる)、
// 活性を使わない所(覗き窓・木の管理のテスト)に作らせないため。
//
// 順番に依存しない理由: 刻む印は交換で付け、初めて付けたグループだけが刻む(1 刻みに 1 回)。刻むのはそのブロックのセルだけ(隣へ書かない)。
#include "sim/multires_bindings.hlsli"

struct MrSlotRecord {
    uint32_t slot;  // 種の一覧では MR_NO_BLOCK なら何もしない
};

struct MrFaceRecord {
    uint32_t slot;  // 面の向き face に進んで入ったブロック
    uint32_t face;
};

struct MrObserverRecord {
    uint3 grid : SV_DispatchGrid;  // 観察の枠の数
    uint32_t firstSlot;
};

groupshared uint32_t gs_possible;
groupshared uint32_t gs_changed;

uint32_t CurrentMark() {
    return MrActivityMark(FX_U64(g_tickHigh, g_tickLow));
}

// 刻むノードの 1 グループのスレッドの数(1 スレッド 8 セル。512 = 1 スレッド 1 セルとも測ったが差は揺れの中だった。perf.md)
static const uint32_t ACTIVITY_STEP_THREADS = 64;

// StepBlockCells の結果(グループで一様)
static const uint32_t STEP_POSSIBLE = 1;  // 進める規則があったセルが 1 つでもある(次の刻みの種)
static const uint32_t STEP_CHANGED = 2;   // 変わったセルが 1 つでもある(忙しい。T-0101)

// ブロックの刻むセルを 1 刻み
uint32_t StepBlockCells(uint32_t slot, uint32_t thread) {
    if (thread == 0) {
        gs_possible = 0;
        gs_changed = 0;
    }

    GroupMemoryBarrierWithGroupSync();
    const MrBlock block = g_blocks[slot];
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / ACTIVITY_STEP_THREADS; ++k) {
        const uint32_t index = thread + (ACTIVITY_STEP_THREADS * k);
        if (!MrIsSteppedCell(block, index))
            continue;

        const uint32_t address = PageCellAddress(block.page, index);
        const RxCell before = g_cells[address];
        const RxCellStep step = MrStepCellDetailed(MakeTable(), before, seed, tick, block, index);
        g_cells[address] = step.cell;
        if (step.possible != 0)
            InterlockedOr(gs_possible, 1u);

        if (MrCellChanged(before, step.cell))
            InterlockedOr(gs_changed, 1u);
    }

    GroupMemoryBarrierWithGroupSync();

    return (gs_possible != 0 ? STEP_POSSIBLE : 0u) | (gs_changed != 0 ? STEP_CHANGED : 0u);
}

// 刻んだ結果を見出しと種に(スレッド 0)
void FinishBlock(uint32_t slot, uint32_t result) {
    if ((result & STEP_CHANGED) != 0)
        g_blocks[slot].busyTick = CurrentMark();  // 変わったので忙しい(T-0101)

    if ((result & STEP_POSSIBLE) != 0)
        AppendActivity(slot);
}

// 頁を配ったばかりの一様なブロックの頁を一様の値で埋める(StepBlockCells と同じスレッドの受け持ちなので、その後に同期は要らない)
void FillExpandedPage(uint32_t slot, uint32_t thread) {
    const MrBlock block = g_blocks[slot];
    const RxCell value = g_cells[slot];
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / ACTIVITY_STEP_THREADS; ++k) {
        const uint32_t index = thread + (ACTIVITY_STEP_THREADS * k);
        g_cells[PageCellAddress(block.page, index)] = MrUniformCell(block, value, index);
    }
}

// clang-format off

// 活性の種 1 つ(1 スレッド = (八分の一, 面) の組 1 つ、スレッド MR_WAKE_CHECKS = 自分): 刻む印を付け、面の隣を起こす。
// 起こしたブロックは初めて印を付けた時だけ刻む。細かい側へは WakeFaceNode でたどる
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
[NodeIsProgramEntry]
void ActivitySeedNode(DispatchNodeInputRecord<MrSlotRecord> input, uint32_t thread : SV_GroupIndex,
                      [MaxRecords(MR_WAKE_CHECKS + 1)] [NodeId("ActivityStepNode")] NodeOutput<MrSlotRecord> step,
                      [MaxRecords(MR_WAKE_CHECKS)] [NodeId("WakeFaceNode")] NodeOutput<MrFaceRecord> faces) {
    const uint32_t slot = input.Get().slot;
    const bool valid = slot < g_worldBlocks && g_blocks[min(slot, g_worldBlocks - 1)].kind == MR_BLOCK_REAL;
    const uint32_t face = thread % MR_FACES;
    MrWake wake;
    wake.schedule = MR_NO_BLOCK;
    wake.descend = MR_NO_BLOCK;
    if (valid && thread == MR_WAKE_CHECKS) {
        wake.schedule = slot;
        const bool substepWake = (g_stepFlags & MR_STEP_SUBSTEP_WAKE) != 0;  // 小刻みの終わりに起こす時は直さない(T-0109)
        if (!substepWake && g_blocks[slot].busyTick == MR_BUSY_POKED)
            g_blocks[slot].busyTick = CurrentMark();  // つつかれた刻みの印にする(T-0101)
    } else if (valid && thread < MR_WAKE_CHECKS)
        wake = MrWakeAcross(MakeTree(), g_blocks[slot], thread / MR_FACES, face, g_rootLevel);

    // --- 出力(全部のスレッドが 0 件か 1 件ずつ)---
    const bool newly = wake.schedule != MR_NO_BLOCK && ScheduleBlock(wake.schedule, CurrentMark());
    ThreadNodeOutputRecords<MrSlotRecord> stepOutput = step.GetThreadNodeOutputRecords(newly ? 1 : 0);
    if (newly)
        stepOutput.Get().slot = wake.schedule;

    stepOutput.OutputComplete();

    const bool descend = wake.descend != MR_NO_BLOCK;
    ThreadNodeOutputRecords<MrFaceRecord> faceOutput = faces.GetThreadNodeOutputRecords(descend ? 1 : 0);
    if (descend) {
        faceOutput.Get().slot = wake.descend;
        faceOutput.Get().face = face;
    }

    faceOutput.OutputComplete();
}

// 面の向き face に進んで入ったブロック: 手前の面に接する八分の一が覆われていなければ自分を起こし、覆われていれば子へたどる
[Shader("node")]
[NodeLaunch("thread")]
[NodeMaxRecursionDepth(MR_MAX_WAKE_DEPTH)]
void WakeFaceNode(ThreadNodeInputRecord<MrFaceRecord> input,
                  [MaxRecords(1)] [NodeId("ActivityStepNode")] NodeOutput<MrSlotRecord> step,
                  [MaxRecords(MR_FACE_OCTANTS)] [NodeId("WakeFaceNode")] NodeOutput<MrFaceRecord> next) {
    const MrFaceRecord record = input.Get();
    const MrBlock block = g_blocks[record.slot];
    const bool canRecurse = GetRemainingRecursionLevels() > 0;
    bool wakeSelf = false;

    // --- 手前の 4 つの八分の一(展開して 0 件か 1 件ずつ出す。まとめて出すと WARP で予定がずれた)---
    [unroll]
    for (uint32_t i = 0; i < MR_FACE_OCTANTS; ++i) {
        const uint32_t child = block.children[MrNearOctant(record.face, i)];
        wakeSelf = wakeSelf || child == MR_NO_BLOCK;
        if (child != MR_NO_BLOCK && !canRecurse)
            InterlockedAdd(g_counters[MR_COUNTER_WAKE_TOO_DEEP], 1u);

        const bool descend = child != MR_NO_BLOCK && canRecurse;
        ThreadNodeOutputRecords<MrFaceRecord> output = next.GetThreadNodeOutputRecords(descend ? 1 : 0);
        if (descend) {
            output.Get().slot = child;
            output.Get().face = record.face;
        }

        output.OutputComplete();
    }

    const bool newly = wakeSelf && ScheduleBlock(record.slot, CurrentMark());
    ThreadNodeOutputRecords<MrSlotRecord> stepOutput = step.GetThreadNodeOutputRecords(newly ? 1 : 0);
    if (newly)
        stepOutput.Get().slot = record.slot;

    stepOutput.OutputComplete();
}

// 印を付けた世界のブロックを刻む(1 グループ = 1 ブロック)。セルが変わったら忙しさの印、進める規則があれば次の刻みの種(u13)へ
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(ACTIVITY_STEP_THREADS, 1, 1)]
void ActivityStepNode(DispatchNodeInputRecord<MrSlotRecord> input, uint32_t thread : SV_GroupIndex) {
    const uint32_t slot = input.Get().slot;
    if (thread == 0)
        InterlockedAdd(g_counters[MR_COUNTER_SCHEDULED], 1u);

    // --- 伝導を入れる刻み: ここでは刻まず、伝導の一覧に足すだけ(面の流れは刻みの初めのセルから。Compute の段が刻む。T-0107)---
    if ((g_stepFlags & MR_STEP_CONDUCTION) != 0) {
        if (thread == 0)
            ConductAppend(slot);

        return;
    }

    // --- 一様: 反応が進む時だけ頁に広げる印(T-0102。グループで一様な分岐)---
    const MrBlock block = g_blocks[slot];
    if (MrIsUniform(block)) {
        const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
        const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
        if (thread == 0 && MrUniformWouldChange(MakeTable(), g_cells[slot], seed, tick, block))
            g_blocks[slot].page = MR_PAGE_WANTED;

        return;
    }

    const uint32_t result = StepBlockCells(slot, thread);
    if (thread == 0)
        FinishBlock(slot, result);
}

// TreeExpand が頁を配った一様なブロックを埋めて刻む(1 グループ = 1 ブロック。レコード MR_NO_BLOCK は何もしない。T-0102)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(ACTIVITY_STEP_THREADS, 1, 1)]
[NodeIsProgramEntry]
void ExpandStepNode(DispatchNodeInputRecord<MrSlotRecord> input, uint32_t thread : SV_GroupIndex) {
    const uint32_t slot = input.Get().slot;
    if (slot == MR_NO_BLOCK)
        return;

    FillExpandedPage(slot, thread);
    const uint32_t result = StepBlockCells(slot, thread);
    if (thread == 0)
        FinishBlock(slot, result | STEP_CHANGED);  // 頁に広げたのも忙しい(畳めるかを N 刻み後に調べる。T-0103)
}

// 観察の枠(影)を全部刻む(1 グループ = 1 枠。活性に入れない。D-403)
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(65535, 1, 1)]
[NumThreads(ACTIVITY_STEP_THREADS, 1, 1)]
[NodeIsProgramEntry]
void ObserverStepNode(DispatchNodeInputRecord<MrObserverRecord> input, uint3 group : SV_GroupID,
                      uint32_t thread : SV_GroupIndex) {
    StepBlockCells(input.Get().firstSlot + group.x, thread);
}

// clang-format on
