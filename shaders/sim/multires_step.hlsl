// multires_step.hlsl — 多重解像度の入れ子の刻む(反応の)パス(T-0017。待ちの丸め: ADR-0018・T-0121)。1 グループ = 1 枠(512 セル)。
// 刻むのは本物の葉のセルと影のセル(MrIsSteppedCell)。セルどうしは独立(隣へは書かない)なので、その場で書き換える。
// 全部を刻む時は WakeDue(起こす段。つつかれたブロックの印を直す)→ StepWait → TreeExpand → StepExpandedWaitPass の順
// (gpu_multires.cpp の RecordStep)。一様なブロック(T-0102)は値 1 つで変わるかを調べ、変わるなら頁に広げる印を付ける。
// TreeExpand(multires_tree.hlsl)が枠の順に頁を配り、StepExpandedWaitPass が埋めて刻む。活性の刻みでは WakeDue が
// 起こす刻みの来たブロックを種の一覧にも足す(RecordStepActive)。中身は multires_wait_step.hlsli(CPU の StepBlocks と RecordWaitResults)。
// CPU リファレンスは sim::StepNest(engine/src/sim/multires_nest.cpp)。
#include "sim/multires_wait_step.hlsli"

// 起こす段(1 スレッド = 1 枠、全部の枠): 見出しを全部なめ、つつかれたブロックを「この刻みに変わった」にし(CPU の ResolvePokes と
// RecordWaitResults が書く値)、活性の刻みなら起こす刻み(wakeTick)が来た世界の本物のブロックを種の一覧へ足す(CPU の StepActive の種)。
// 起こす刻みは書き換える前の値で見る(CPU も種を集めてから ResolvePokes)
[numthreads(64, 1, 1)] void WakeDue(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t slot = dispatchThreadId.x;
    if (slot >= g_blockCount)
        return;

    const MrBlock block = g_blocks[slot];
    const uint64_t mark = CurrentChangeMark();
    if (block.busyTick == MR_BUSY_POKED) {
        g_blocks[slot].busyTick = mark;
        g_blocks[slot].wakeTick = mark + 1;
    }

    const bool seeds = (g_stepFlags & MR_STEP_WAKE_SEEDS) != 0;
    if (seeds && slot < g_worldBlocks && block.kind == MR_BLOCK_REAL && block.wakeTick <= mark)
        AppendActivity(slot);
}

// 刻む枠か(使っている枠 = 写しと空き以外)
bool IsSteppedSlot(uint32_t slot) {
    const uint32_t kind = g_blocks[slot].kind;

    return kind != MR_BLOCK_UNUSED && kind != MR_BLOCK_MIRROR;
}

// 全部の枠を待ちの丸めで刻む(1 グループ = 1 枠。CPU の StepNest。眠っているブロックは評価を省く。T-0123)
[numthreads(WAIT_STEP_THREADS, 1, 1)] void StepWait(uint3 group : SV_GroupID, uint32_t thread : SV_GroupIndex) {
    const uint32_t slot = group.x;
    if (slot >= g_blockCount)
        return;

    if (!IsSteppedSlot(slot))
        return;

    StepBlockWait(slot, thread);
}

// グループ g が埋めるレコード(g + 1。レコード 0 は空)
uint32_t ExpandRecordOfGroup(uint32_t group) {
    return group + 1;
}

// TreeExpand が頁を配ったブロックを埋めて待ちの丸めで刻む(1 グループ = 1 ブロック。グループ g はレコード g + 1)
[numthreads(WAIT_STEP_THREADS, 1, 1)] void StepExpandedWaitPass(uint3 group : SV_GroupID,
                                                                uint32_t thread : SV_GroupIndex) {
    const uint32_t record = ExpandRecordOfGroup(group.x);
    if (record >= ExpandRecordCount())
        return;

    StepExpandedWait(ExpandRecord(record), thread);
}
