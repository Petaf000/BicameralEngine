// object_heat_nest.hlsl — 物の熱の段(object_heat.hlsl)と多重解像度の世界のセルをつなぐ 2 つの Compute(T-1077・ADR-0451・ADR-0360)。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)に、外のバッファ u4・u5 を足して使う(覗き窓・実験室の箱と同じ口。T-0096)。
// CPU リファレンスは tests/object_heat_nest_scene.h の LoadCells・StoreCells(試験 gpu_object_heat_nest がビット一致を確かめる)。
//
// データの流れ(1 刻み = 世界の刻み → 写す → 物の熱の段 → 書き戻す):
//   OhNestGather:  世界のセル(LoadCell。一様なら一様の値)→ 物の熱の囲むセル(u4 の前半)と写した時の値(u4 の後半)。
//                  前の刻みの値と違えば、そのセルの「変わった段」(u5 の前半 = 物の熱の段の u9)を今の段にする(物のブロックを起こす)
//   OhNestScatter: 物の熱が変えたセル(写した時の値と違うセル)を世界の頁へ書き、差を帳簿の理由「物との受け渡し」に載せ、ブロックをつつく
// 物の熱の段は囲むセルを平らな並びで持つ(写しは囲むセルの数だけ。費用は物の数ではなくセルの数)。
// 頁を持たない(一様の)ブロックへは書けないので数える(u5 の最後の語)。一様のブロックを頁に広げるのは T-1077 の残り。
#include "sim/multires_bindings.hlsli"

// u4 = RxCell [N](物の熱の段の u5 と同じバッファ)+ RxCell [N](写した時の値)
RWStructuredBuffer<RxCell> g_objectCells : register(u4);
// u5 = 変わった段 [N](物の熱の段の u9 と同じバッファ)+ 世界の場所(枠・セルの番号)[N × 2] + 書けなかった数 [1]
RWStructuredBuffer<uint32_t> g_objectCellWords : register(u5);

static const uint32_t NEST_GROUP_THREADS = 64;

uint32_t CellCount() {
    return g_external0;
}

uint32_t RefSlot(uint32_t cell) {
    return g_objectCellWords[CellCount() + (2 * cell)];
}

uint32_t RefIndex(uint32_t cell) {
    return g_objectCellWords[CellCount() + (2 * cell) + 1];
}

// --- 写す: g_external0 = 囲むセルの数、g_external1 = 今の段(物の熱の段の通し番号。CPU の world.pass)---
[numthreads(NEST_GROUP_THREADS, 1, 1)] void OhNestGather(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cell = dispatchThreadId.x;
    if (cell >= CellCount())
        return;

    const RxCell value = LoadCell(RefSlot(cell), RefIndex(cell));
    if (!RxSameCell(value, g_objectCells[cell]))
        g_objectCellWords[cell] = g_external1;

    g_objectCells[cell] = value;
    g_objectCells[CellCount() + cell] = value;
}

// 量の差 delta を帳簿の理由「物との受け渡し」の列 column に(増えた = 入・減った = 出)
void AddExchangeDelta(int32_t level, uint32_t column, int64_t delta) {
    if (delta == 0)
        return;

    const uint32_t direction = delta > 0 ? WL_DIRECTION_IN : WL_DIRECTION_OUT;
    const uint64_t magnitude = delta > 0 ? (uint64_t)delta : (uint64_t)(-delta);
    AddWorldLedgerUnits(WlSlot(WL_REASON_OBJECT_EXCHANGE, direction), level, column, magnitude);
}

uint64_t AmountIn(RxCell cell, uint32_t species) {
    const uint32_t slot = RxFindSlot(cell, species);

    return slot == RX_NO_SLOT ? (uint64_t)0 : RX_AMOUNT_AT(cell, slot);
}

// セルの変わった分を帳簿へ(エネルギーと、どちらかのセルにある物質ごと。CPU の LedgerCellChange と同じ差)
void LedgerCellChange(int32_t level, RxCell before, RxCell after) {
    AddExchangeDelta(level, WL_COLUMN_ENERGY, after.energy - before.energy);

    FX_UNROLL
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i >= after.speciesCount)
            continue;

        const uint32_t species = after.species[i];
        const int64_t delta = (int64_t)after.amounts[i] - (int64_t)AmountIn(before, species);
        AddExchangeDelta(level, WL_COLUMN_SPECIES_BASE + species, delta);
    }

    // 前にあって後に無い物質(全部が出た)
    FX_UNROLL
    for (uint32_t j = 0; j < RX_MAX_CELL_SPECIES; ++j) {
        if (j >= before.speciesCount || RxFindSlot(after, before.species[j]) != RX_NO_SLOT)
            continue;

        AddExchangeDelta(level, WL_COLUMN_SPECIES_BASE + before.species[j], -(int64_t)before.amounts[j]);
    }
}

// --- 書き戻す: g_external0 = 囲むセルの数 ---
[numthreads(NEST_GROUP_THREADS, 1, 1)] void OhNestScatter(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cell = dispatchThreadId.x;
    if (cell >= CellCount())
        return;

    const RxCell after = g_objectCells[cell];
    const RxCell before = g_objectCells[CellCount() + cell];
    if (RxSameCell(after, before))
        return;

    const uint32_t slot = RefSlot(cell);
    const MrBlock block = g_blocks[slot];
    if (MrIsUniform(block)) {
        InterlockedAdd(g_objectCellWords[3 * CellCount()], 1u);
        return;
    }

    LedgerCellChange(block.level, before, after);
    g_cells[PageCellAddress(block.page, RefIndex(cell))] = after;
    PokeBlock(slot);
}
