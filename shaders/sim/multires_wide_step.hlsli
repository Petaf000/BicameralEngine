// multires_wide_step.hlsli — 溢れを持つ頁のブロックを待ちの丸めで 1 刻み(成分の二段の GPU 版。T-0176・02 §3・17 R-MULTI-4)。
// multires_wait_step.hlsli が MR_WIDE_CELLS の時だけ使う(全部を刻む Compute の変種 multires_step_wide_*。活性のグラフには入れない:
// グラフのノードに反応の核を足さない約束。T-0124)。CPU リファレンスは multires_nest.cpp の StepPagedBlock(溢れを使う世界)。
// 溢れの並びは shaders/common/multires_wide.hlsli。
//
// 1 グループ = 1 ブロック(64 スレッド、1 スレッド 8 セル)。セルどうしは独立なので、成分が 8 種に収まるセルはインラインの核(RxCell)で
// 刻んでその場で書く(上限に当たらない間は上限なしの形とビット単位で同じ。T-0175)。溢れを持つセル・インラインで 9 種目の生成物を
// 待たせたセルだけ、上限の無い形(RxGpuWideCell)で刻み直す:
//   ① 刻んだ後の溢れの数をセルごとに数える(書かない)→ ② セルの番号の順のプレフィックス和(スレッド 0)で並びを決め、
//   頁の溢れの枠に入るなら ③ もう一度刻んで、インラインを頁へ・溢れをもう片方の面へ書き、面を入れ替える。
// 入らなければそのセルは書かずに待たせる(MR_COUNTER_LIMIT_PRODUCTS。大きい溢れを配ってやり直すのは T-0211)。
//
// 順番に依存しない理由: 書くのは自分のブロックのセル・溢れ・面だけ。並びはセルの番号の順のプレフィックス和で、刻む順に依らない。
#ifndef BICAMERAL_MULTIRES_WIDE_STEP_HLSLI
#define BICAMERAL_MULTIRES_WIDE_STEP_HLSLI

#include "common/multires_wide.hlsli"

// --- 上限の無い形のセル(GPU は RX_GPU_WIDE_SPECIES 個まで。reaction.hlsli の「セルの形ごとの道具」の版)---

struct RxGpuWideCell {
    int64_t energy;
    uint32_t speciesCount;
    uint32_t species[RX_GPU_WIDE_SPECIES];
    uint64_t amounts[RX_GPU_WIDE_SPECIES];
};

struct RxGpuWideUsage {
    uint64_t amounts[RX_GPU_WIDE_SPECIES];
    uint64_t heat;
};

RxGpuWideCell RxMakeEmptyGpuWideCell(int64_t energy) {
    RxGpuWideCell cell;
    cell.energy = energy;
    cell.speciesCount = 0;
    for (uint32_t i = 0; i < RX_GPU_WIDE_SPECIES; ++i) {
        cell.species[i] = 0;
        cell.amounts[i] = 0;
    }

    return cell;
}

bool RxHasRoomForSpecies(RxGpuWideCell cell) {
    return cell.speciesCount < RX_GPU_WIDE_SPECIES;
}

RxGpuWideCell RxCellWithRoom(RxGpuWideCell cell) {
    return cell;
}

RxGpuWideCell RxEmptyCellLike(RxGpuWideCell cell, int64_t energy) {
    return RxMakeEmptyGpuWideCell(energy);
}

RxGpuWideUsage RxEmptyUsage(RxGpuWideCell cell) {
    RxGpuWideUsage usage;
    for (uint32_t i = 0; i < RX_GPU_WIDE_SPECIES; ++i)
        usage.amounts[i] = 0;

    usage.heat = 0;

    return usage;
}

bool SameGpuWideCell(RxGpuWideCell a, RxGpuWideCell b) {
    if (a.energy != b.energy || a.speciesCount != b.speciesCount)
        return false;

    for (uint32_t i = 0; i < a.speciesCount; ++i) {
        if (a.species[i] != b.species[i] || a.amounts[i] != b.amounts[i])
            return false;
    }

    return true;
}

// --- 頁の溢れ(u6 の後ろ。g_overflowBase)---

uint32_t WideCurrentSide(uint32_t page) {
    return g_treeWords[MrWidePageWord(g_overflowBase, page)];
}

// 面 side のセル index の溢れの [始まり, 終わり)
uint2 WideRange(uint32_t page, uint32_t side, uint32_t index) {
    const uint32_t base = MrWideSideWord(g_overflowBase, page, side);

    return uint2(g_treeWords[base + index], g_treeWords[base + index + 1]);
}

// インラインのセルに溢れ(面 side)を足して上限の無い形にする(CPU の LoadWideNestCell)
RxGpuWideCell LoadGpuWideCell(RxCell inlineCell, uint32_t page, uint32_t side, uint32_t index) {
    RxGpuWideCell cell = RxMakeEmptyGpuWideCell(inlineCell.energy);
    cell.speciesCount = inlineCell.speciesCount;
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        cell.species[i] = inlineCell.species[i];
        cell.amounts[i] = inlineCell.amounts[i];
    }

    if (inlineCell.speciesCount < RX_MAX_CELL_SPECIES)
        return cell;

    const uint2 range = WideRange(page, side, index);
    const uint32_t base = MrWideSideWord(g_overflowBase, page, side);
    FX_ASSERT(range.y - range.x <= RX_GPU_WIDE_SPECIES - RX_MAX_CELL_SPECIES);
    for (uint32_t e = range.x; e < range.y; ++e) {
        const uint32_t word = base + MrWideEntryWord(e);
        cell.species[cell.speciesCount] = g_treeWords[word];
        cell.amounts[cell.speciesCount] = FX_U64(g_treeWords[word + 2], g_treeWords[word + 1]);
        cell.speciesCount += 1;
    }

    return cell;
}

// 溢れの数(インラインに入らない成分)
uint32_t WideTailCount(RxGpuWideCell cell) {
    return cell.speciesCount > RX_MAX_CELL_SPECIES ? cell.speciesCount - RX_MAX_CELL_SPECIES : 0u;
}

// インラインを頁へ(使わない枠は 0。CPU の StoreWidePageCell)、残りを面 side の first からへ書く
void StoreGpuWideCell(RxGpuWideCell cell, uint32_t address, uint32_t page, uint32_t side, uint32_t first) {
    RxCell narrow = RxMakeEmptyCell(cell.energy);
    narrow.speciesCount = min(cell.speciesCount, RX_MAX_CELL_SPECIES);
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < narrow.speciesCount) {
            narrow.species[i] = cell.species[i];
            narrow.amounts[i] = cell.amounts[i];
        }
    }

    g_cells[address] = narrow;
    const uint32_t base = MrWideSideWord(g_overflowBase, page, side);
    for (uint32_t i = RX_MAX_CELL_SPECIES; i < cell.speciesCount; ++i) {
        const uint32_t word = base + MrWideEntryWord(first + i - RX_MAX_CELL_SPECIES);
        g_treeWords[word] = cell.species[i];
        g_treeWords[word + 1] = (uint32_t)cell.amounts[i];
        g_treeWords[word + 2] = (uint32_t)(cell.amounts[i] >> 32);
    }
}

// 頁を配ったばかりのブロックの今の面を空にする(前に使った頁の溢れを読まないため。一様の値は溢れを持たない)
void ClearWidePage(uint32_t page, uint32_t thread) {
    const uint32_t base = MrWideSideWord(g_overflowBase, page, 0);
    for (uint32_t i = thread; i <= MR_BLOCK_CELLS; i += WAIT_STEP_THREADS)
        g_treeWords[base + i] = 0;

    if (thread == 0)
        g_treeWords[MrWidePageWord(g_overflowBase, page)] = 0;

    AllMemoryBarrierWithGroupSync();
}

// --- 刻む ---

groupshared uint32_t gs_wideOffsets[MR_BLOCK_CELLS];  // ① セルの溢れの数 → ② 新しい面の始まり
groupshared uint32_t gs_wideAny;                      // 上限の無い形で刻むセルがある
groupshared uint32_t gs_wideTotal;                    // 新しい面の溢れの数

// 上限の無い形で読む 1 セル(今の面)
RxGpuWideCell LoadSteppedWideCell(MrBlock block, uint32_t index, uint32_t side) {
    return LoadGpuWideCell(g_cells[PageCellAddress(block.page, index)], block.page, side, index);
}

// 頁を持つブロックの刻むセルを待ちの丸めで 1 刻み(StepPagedWait の溢れを使う版)
WaitBlockResult StepPagedWaitWide(uint32_t slot, uint32_t thread) {
    BeginWaitReduce(thread);
    if (thread == 0)
        gs_wideAny = 0;

    const MrBlock block = g_blocks[slot];
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    const uint32_t side = WideCurrentSide(block.page);
    uint64_t wakeTick = RX_WAIT_NEVER;
    MrLimitTally tally = MrMakeLimitTally();
    uint32_t wideMask = 0;  // ビット k = セル thread + 64k を上限の無い形で刻む
    GroupMemoryBarrierWithGroupSync();

    // --- ① 8 種に収まるセルはその場で刻んで書く。残りは溢れの数だけ数える ---
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        const uint32_t index = thread + (WAIT_STEP_THREADS * k);
        gs_wideOffsets[index] = 0;
        if (!MrIsSteppedCell(block, index))
            continue;

        const uint32_t address = PageCellAddress(block.page, index);
        const RxCell before = g_cells[address];
        uint2 range = uint2(0, 0);  // 溢れは 8 種のセルにだけある
        if (before.speciesCount == RX_MAX_CELL_SPECIES)
            range = WideRange(block.page, side, index);

        if (range.x == range.y) {
            const RxWaitStep step = MrStepCellWait(MakeTable(), before, seed, tick, block, index);
            if ((step.limits & RX_LIMIT_PRODUCTS) == 0) {
                g_cells[address] = step.cell;
                wakeTick = MinTick(wakeTick, step.wakeTick);
                tally = MrAddLimits(tally, step.limits);
                if (MrCellChanged(before, step.cell))
                    InterlockedOr(gs_waitChanged, 1u);

                continue;
            }
        }

        wideMask |= 1u << k;
        gs_wideAny = 1;
        const RxGpuWideCell wide = LoadSteppedWideCell(block, index, side);
        gs_wideOffsets[index] = WideTailCount(MrStepCellWait(MakeTable(), wide, seed, tick, block, index).cell);
    }

    GroupMemoryBarrierWithGroupSync();
    if (gs_wideAny == 0) {
        CountLimits(tally);
        return EndWaitReduce(thread, wakeTick);
    }

    // --- ② セルの番号の順のプレフィックス和(新しい面の並び)---
    if (thread == 0) {
        uint32_t total = 0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const uint32_t count = gs_wideOffsets[index];
            gs_wideOffsets[index] = total;
            total += count;
        }

        gs_wideTotal = total;
    }

    GroupMemoryBarrierWithGroupSync();
    const bool fits = gs_wideTotal <= MR_WIDE_PAGE_ENTRIES;
    const uint32_t next = 1 - side;
    const uint32_t nextBase = MrWideSideWord(g_overflowBase, block.page, next);
    if (fits) {
        for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
            const uint32_t index = thread + (WAIT_STEP_THREADS * k);
            g_treeWords[nextBase + index] = gs_wideOffsets[index];
        }

        if (thread == 0)
            g_treeWords[nextBase + MR_BLOCK_CELLS] = gs_wideTotal;
    }

    // --- ③ 入るなら刻み直して書く(読むのは今の面、書くのはもう片方の面)。入らなければ待たせる ---
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        if ((wideMask & (1u << k)) == 0)
            continue;

        const uint32_t index = thread + (WAIT_STEP_THREADS * k);
        if (!fits) {
            wakeTick = MinTick(wakeTick, MrChangeMark(tick) + 1);
            tally.productsHeld += 1;
            continue;
        }

        const RxGpuWideCell before = LoadSteppedWideCell(block, index, side);
        const RxWaitStepOf<RxGpuWideCell> step = MrStepCellWait(MakeTable(), before, seed, tick, block, index);
        wakeTick = MinTick(wakeTick, step.wakeTick);
        tally = MrAddLimits(tally, step.limits);
        if (!SameGpuWideCell(before, step.cell))
            InterlockedOr(gs_waitChanged, 1u);

        StoreGpuWideCell(step.cell, PageCellAddress(block.page, index), block.page, next, gs_wideOffsets[index]);
    }

    // --- 面を入れ替える(全部のセルを書いた後)---
    AllMemoryBarrierWithGroupSync();
    if (fits && thread == 0)
        g_treeWords[MrWidePageWord(g_overflowBase, block.page)] = next;

    CountLimits(tally);

    return EndWaitReduce(thread, wakeTick);
}

#endif  // BICAMERAL_MULTIRES_WIDE_STEP_HLSLI
