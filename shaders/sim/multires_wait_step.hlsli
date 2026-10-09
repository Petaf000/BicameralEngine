// multires_wait_step.hlsli — 待ちの丸め(ADR-0018)で 1 ブロックを刻む GPU の部品(T-0121)。1 グループ = 1 ブロック(WAIT_STEP_THREADS スレッド、
// 1 スレッド 8 セル)。全部を刻む Compute(multires_step.hlsl)と活性のグラフ(multires_activity_graph.hlsl。T-0124)が使う。
// CPU リファレンスは engine/src/sim/multires_nest.cpp の StepBlocks(一様なブロックの評価・StepPagedBlock)と RecordWaitResults。
// 見出しの書き方: 変わった・頁に広げた・つつかれた(busyTick = この刻みの印)ブロックは busyTick = 印・wakeTick = 印 + 1、
// 評価して変わらなければ wakeTick = 刻むセルの次に評価の要る刻みの最小。つつかれたブロックは刻む前に起こす段(WakeDue)が
// busyTick = 印・wakeTick = 印 + 1 にしてある(CPU の ResolvePokes の後に RecordWaitResults が書く値と同じ)。
//
// 順番に依存しない理由: 書くのは自分のブロックのセルと見出しだけ。グループの集計は「変わったか」の OR と、スレッドごとの最小を
// スレッド 0 が順に見る最小(どちらも順によらない)。
#ifndef BICAMERAL_MULTIRES_WAIT_STEP_HLSLI
#define BICAMERAL_MULTIRES_WAIT_STEP_HLSLI

#include "sim/multires_bindings.hlsli"

static const uint32_t WAIT_STEP_THREADS = 64;

groupshared uint32_t gs_waitChanged;
// スレッドごとの最小を下位・上位 32bit に分けて置く
groupshared uint32_t gs_waitWakeLow[WAIT_STEP_THREADS];
groupshared uint32_t gs_waitWakeHigh[WAIT_STEP_THREADS];

// グループで集計した結果(スレッド 0 だけが正しい値を持つ)
struct WaitBlockResult {
    uint64_t wakeTick;  // 変わらなければ、次に評価の要る刻みの印(刻むセルの最小。無ければ RX_WAIT_NEVER)
    uint32_t changed;   // 刻むセルが 1 つでも変わった(0 か 1)
};

// この刻みの印(MrChangeMark。64bit)
uint64_t CurrentChangeMark() {
    return MrChangeMark(FX_U64(g_tickHigh, g_tickLow));
}

// --- グループの集計 ---

// 64bit の最小(HW の wakeTick の不具合の原因は RxStepCellWait の飽和する足し算の形だった。T-0124・reaction.hlsli の RxWakeTickOf)
uint64_t MinTick(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

void BeginWaitReduce(uint32_t thread) {
    if (thread == 0)
        gs_waitChanged = 0;

    GroupMemoryBarrierWithGroupSync();
}

WaitBlockResult EndWaitReduce(uint32_t thread, uint64_t wakeTick) {
    gs_waitWakeLow[thread] = (uint32_t)wakeTick;
    gs_waitWakeHigh[thread] = (uint32_t)(wakeTick >> 32);
    GroupMemoryBarrierWithGroupSync();

    WaitBlockResult result;
    result.changed = gs_waitChanged != 0 ? 1u : 0u;
    result.wakeTick = RX_WAIT_NEVER;
    if (thread != 0)
        return result;

    for (uint32_t i = 0; i < WAIT_STEP_THREADS; ++i)
        result.wakeTick = MinTick(result.wakeTick, FX_U64(gs_waitWakeHigh[i], gs_waitWakeLow[i]));

    return result;
}

// スレッドで足した上限の印を数える器に足す(T-0163。0 の時は atomic を使わない。足す順に依らない)
void CountLimits(MrLimitTally tally) {
    if (tally.productsHeld != 0)
        InterlockedAdd(g_counters[MR_COUNTER_LIMIT_PRODUCTS], tally.productsHeld);

    if (tally.candidatesLimited != 0)
        InterlockedAdd(g_counters[MR_COUNTER_LIMIT_CANDIDATES], tally.candidatesLimited);
}

// --- 刻む ---

// 頁を持つブロックの刻むセルを待ちの丸めで 1 刻み(CPU の StepPagedBlock の反応。上限に当たった印も数える。T-0163)
WaitBlockResult StepPagedWait(uint32_t slot, uint32_t thread) {
    BeginWaitReduce(thread);
    const MrBlock block = g_blocks[slot];
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    uint64_t wakeTick = RX_WAIT_NEVER;
    MrLimitTally tally = MrMakeLimitTally();
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        const uint32_t index = thread + (WAIT_STEP_THREADS * k);
        if (!MrIsSteppedCell(block, index))
            continue;

        const uint32_t address = PageCellAddress(block.page, index);
        const RxCell before = g_cells[address];
        const RxWaitStep step = MrStepCellWait(MakeTable(), before, seed, tick, block, index);
        g_cells[address] = step.cell;
        wakeTick = MinTick(wakeTick, step.wakeTick);
        tally = MrAddLimits(tally, step.limits);
        if (MrCellChanged(before, step.cell))
            InterlockedOr(gs_waitChanged, 1u);
    }

    CountLimits(tally);

    return EndWaitReduce(thread, wakeTick);
}

#ifdef MR_WIDE_CELLS
#include "sim/multires_wide_step.hlsli"
#endif

// 頁を持つブロックを刻む(溢れを使う変種〔MR_WIDE_CELLS。T-0176〕は StepPagedWaitWide)
WaitBlockResult StepPagedWaitShaped(uint32_t slot, uint32_t thread) {
#ifdef MR_WIDE_CELLS
    return StepPagedWaitWide(slot, thread);
#else
    return StepPagedWait(slot, thread);
#endif
}

// 一様なブロックを待ちの丸めで評価する(CPU の MrUniformWaitOf を 64 スレッドで。変わるかは OR、変わらなければ最小)
WaitBlockResult EvaluateUniformWait(uint32_t slot, uint32_t thread) {
    BeginWaitReduce(thread);
    const MrBlock block = g_blocks[slot];
    const RxCell value = g_cells[slot];
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    uint64_t wakeTick = RX_WAIT_NEVER;
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        const uint32_t index = thread + (WAIT_STEP_THREADS * k);
        if (!MrIsSteppedCell(block, index))
            continue;

        const RxWaitStep step = MrStepCellWait(MakeTable(), value, seed, tick, block, index);
        wakeTick = MinTick(wakeTick, step.wakeTick);
        if (!MrSameCell(step.cell, value))
            InterlockedOr(gs_waitChanged, 1u);
    }

    return EndWaitReduce(thread, wakeTick);
}

// 刻んだ結果を見出しに書く(スレッド 0。CPU の RecordWaitResults の 1 ブロック分)
void FinishWaitBlock(uint32_t slot, uint32_t changed, uint64_t wakeTick) {
    const uint64_t mark = CurrentChangeMark();
    if (changed != 0 || g_blocks[slot].busyTick == mark) {
        g_blocks[slot].busyTick = mark;
        g_blocks[slot].wakeTick = mark + 1;
        return;
    }

    g_blocks[slot].wakeTick = wakeTick;
}

// 刻む印の付いたブロックを 1 刻み(グループで一様な分岐)。どのブロックも起こす刻みが来た時(つつかれた時も)だけ評価する。
// 一様なブロックは変わるなら頁に広げる印(MR_PAGE_WANTED。頁は TreeExpand が枠の順に配り、埋めて刻むのは StepExpandedWait)
void StepBlockWait(uint32_t slot, uint32_t thread) {
    const MrBlock block = g_blocks[slot];

    // --- 眠っているブロック(起こす刻みがまだ来ず、この刻みにつつかれてもいない)は評価しない(T-0123): 最後に評価してから
    //     セルも tc も変わっていないので、どのセルを評価しても変わらず、起こす刻みも同じ値になる(ADR-0018)。全部を刻む刻みと
    //     観察の枠も同じ。CPU の StepNest・StepActive は全部を評価するので、毎刻みのビット一致がこの省略を確かめる ---
    const uint64_t mark = CurrentChangeMark();
    if (mark < block.wakeTick && block.busyTick != mark)
        return;

    if (!MrIsUniform(block)) {
        const WaitBlockResult result = StepPagedWaitShaped(slot, thread);
        if (thread == 0)
            FinishWaitBlock(slot, result.changed, result.wakeTick);

        return;
    }

    const WaitBlockResult result = EvaluateUniformWait(slot, thread);
    if (thread != 0)
        return;

    if (result.changed != 0)
        g_blocks[slot].page = MR_PAGE_WANTED;
    else
        FinishWaitBlock(slot, 0, result.wakeTick);
}

// 頁を配ったばかりの一様なブロックを一様の値で埋めて刻む(頁に広げたのは変わったのと同じ: 次の刻みにまた評価)
void StepExpandedWait(uint32_t slot, uint32_t thread) {
    const MrBlock block = g_blocks[slot];
    const RxCell value = g_cells[slot];
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        const uint32_t index = thread + (WAIT_STEP_THREADS * k);
        g_cells[PageCellAddress(block.page, index)] = MrUniformCell(block, value, index);
    }

#ifdef MR_WIDE_CELLS
    ClearWidePage(block.page, thread);  // 前に使った頁の溢れを読まない
#endif

    StepPagedWaitShaped(slot, thread);  // 同じスレッドが同じセルを受け持つので、埋めた後に同期は要らない
    if (thread == 0)
        FinishWaitBlock(slot, 1, RX_WAIT_NEVER);
}

#endif  // BICAMERAL_MULTIRES_WAIT_STEP_HLSLI
