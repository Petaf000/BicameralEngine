// multires_step.hlsl — 多重解像度の入れ子の刻む(反応の)パス(T-0017)。1 スレッド = 1 セル(枠 × 512 セル)。
// 刻むのは本物の葉のセルと影のセル(MrIsSteppedCell)。セルどうしは独立(隣へは書かない)なので、その場で書き換える。
// CPU リファレンスは sim::StepNest(engine/src/sim/multires_nest.cpp)。段をまたぐ輸送は T-0019。
#include "sim/multires_bindings.hlsli"

[numthreads(64, 1, 1)] void Main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t slot = dispatchThreadId.x / MR_BLOCK_CELLS;
    const uint32_t index = dispatchThreadId.x % MR_BLOCK_CELLS;
    if (slot >= g_blockCount)
        return;

    const MrBlock block = g_blocks[slot];
    if (!MrIsSteppedCell(block, index))
        return;

    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    const uint32_t address = CellAddress(slot, index);
    g_cells[address] = MrStepCell(MakeTable(), g_cells[address], seed, tick, block, index);
}
