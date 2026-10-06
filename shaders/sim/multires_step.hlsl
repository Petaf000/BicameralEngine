// multires_step.hlsl — 多重解像度の入れ子の刻む(反応の)パス(T-0017)。1 スレッド = 1 セル(枠 × 512 セル)。
// 刻むのは本物の葉のセルと影のセル(MrIsSteppedCell)。セルどうしは独立(隣へは書かない)なので、その場で書き換える。
// 一様なブロック(T-0102)はセル 0 のスレッドが値 1 つで反応が進むかを調べ、進むなら頁に広げる印(MR_PAGE_WANTED)を付ける。
// その後 multires_tree.hlsl の TreeExpand が枠の順に頁を配り、StepExpanded が埋めて刻む(gpu_multires.cpp の RecordStep)。
// CPU リファレンスは sim::StepNest(engine/src/sim/multires_nest.cpp)。段をまたぐ輸送は T-0019。
// Main・StepExpanded は今までの丸め(cutoffRounding。T-0122・T-0125 で消す)。既定の待ちの丸め(ADR-0018。T-0121)は下の WakeDue・StepWait・StepExpandedWaitPass。
#include "sim/multires_wait_step.hlsli"

[numthreads(64, 1, 1)] void Main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t slot = dispatchThreadId.x / MR_BLOCK_CELLS;
    const uint32_t index = dispatchThreadId.x % MR_BLOCK_CELLS;
    if (slot >= g_blockCount)
        return;

    const MrBlock block = g_blocks[slot];
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    if (MrIsUniform(block)) {
        if (index == 0 && MrUniformWouldChange(MakeTable(), g_cells[slot], seed, tick, block))
            g_blocks[slot].page = MR_PAGE_WANTED;

        return;
    }

    if (!MrIsSteppedCell(block, index))
        return;

    const uint32_t address = PageCellAddress(block.page, index);
    g_cells[address] = MrStepCell(MakeTable(), g_cells[address], seed, tick, block, index);
}

// TreeExpand が頁を配ったブロック(GPU の入力の一覧 MR_GRAPH_INPUT_EXPAND_*。レコード 0 は空)を一様の値で埋めて刻む。
// 1 グループ = 1 ブロック(グループ g はレコード g + 1。数を超えたグループは何もしない)。全部を刻む時だけ使う
static const uint32_t EXPAND_STEP_THREADS = 64;

[numthreads(EXPAND_STEP_THREADS, 1, 1)] void StepExpanded(uint3 group : SV_GroupID, uint32_t thread : SV_GroupIndex) {
    const uint32_t record = group.x + 1;
    if (record >= ExpandRecordCount())
        return;

    const uint32_t slot = ExpandRecord(record);
    const MrBlock block = g_blocks[slot];
    const RxCell value = g_cells[slot];
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / EXPAND_STEP_THREADS; ++k) {
        const uint32_t index = thread + (EXPAND_STEP_THREADS * k);
        RxCell cell = MrUniformCell(block, value, index);
        if (MrIsSteppedCell(block, index))
            cell = MrStepCell(MakeTable(), cell, seed, tick, block, index);

        g_cells[PageCellAddress(block.page, index)] = cell;
    }
}

    // --- 待ちの丸め(ADR-0018。T-0121)---------------------------------------------------------------
    // 全部を刻む時は WakeDue(起こす段。つつかれたブロックの印を直す)→ StepWait → TreeExpand → StepExpandedWaitPass の順
    // (gpu_multires.cpp の RecordStep)。活性の刻みでは WakeDue が種の一覧に起こす刻みの来たブロックも足す(RecordStepActive)。

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

// 全部の枠を待ちの丸めで刻む(1 グループ = 1 枠。刻むのは使っている枠 = 写しと空き以外。CPU の StepNest)
[numthreads(WAIT_STEP_THREADS, 1, 1)] void StepWait(uint3 group : SV_GroupID, uint32_t thread : SV_GroupIndex) {
    const uint32_t slot = group.x;
    if (slot >= g_blockCount)
        return;

    const uint32_t kind = g_blocks[slot].kind;
    if (kind == MR_BLOCK_UNUSED || kind == MR_BLOCK_MIRROR)
        return;

    StepBlockWait(slot, thread);
}

    // TreeExpand が頁を配ったブロックを埋めて待ちの丸めで刻む(1 グループ = 1 ブロック。グループ g はレコード g + 1)
    [numthreads(WAIT_STEP_THREADS, 1, 1)] void StepExpandedWaitPass(uint3 group : SV_GroupID,
                                                                    uint32_t thread : SV_GroupIndex) {
    const uint32_t record = group.x + 1;
    if (record >= ExpandRecordCount())
        return;

    StepExpandedWait(ExpandRecord(record), thread);
}
