// multires_step.hlsl — 多重解像度の入れ子の刻む(反応の)パス(T-0017)。1 スレッド = 1 セル(枠 × 512 セル)。
// 刻むのは本物の葉のセルと影のセル(MrIsSteppedCell)。セルどうしは独立(隣へは書かない)なので、その場で書き換える。
// 一様なブロック(T-0102)はセル 0 のスレッドが値 1 つで反応が進むかを調べ、進むなら頁に広げる印(MR_PAGE_WANTED)を付ける。
// その後 multires_tree.hlsl の TreeExpand が枠の順に頁を配り、StepExpanded が埋めて刻む(gpu_multires.cpp の RecordStep)。
// CPU リファレンスは sim::StepNest(engine/src/sim/multires_nest.cpp)。段をまたぐ輸送は T-0019。
#include "sim/multires_bindings.hlsli"

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
