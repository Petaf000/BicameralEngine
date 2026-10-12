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
#include "sim/multires_patches.hlsli"

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
        const RxWaitStep step = MrStepCellWait(MakeStepTable(), before, seed, tick, block, index);
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

// 一様なブロックを待ちの丸めで評価する(CPU の MrUniformWaitOf を 64 スレッドで。変わるかは OR、変わらなければ最小)。
// 溢れを使う変種は 9 種目の生成物を待たせたセルも「変わる」として頁に広げる(UNIFORM_EXPANDS_HELD。T-0276)
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

        const RxWaitStep step = MrStepCellWait(MakeStepTable(), value, seed, tick, block, index);
        wakeTick = MinTick(wakeTick, step.wakeTick);
        if (MrUniformStepExpands(step, value, UNIFORM_EXPANDS_HELD))
            InterlockedOr(gs_waitChanged, 1u);
    }

    return EndWaitReduce(thread, wakeTick);
}

#ifdef MR_WIDE_CELLS
// 一様の値(インライン + 一様の値の溢れ〔WideUniformArea のセル 0 の範囲。T-0312〕)を速い形で読む。速い形に入らなければ false
// (観察の枠は一様の値の溢れの領域を持たないので、インラインだけ)
bool LoadUniformMid(uint32_t slot, out RxGpuMidCell value) {
    const RxCell inlineValue = g_cells[slot];
    if (slot >= g_worldBlocks) {
        value = RxEmptyCellLike(MidShape(), inlineValue.energy);
        value.speciesCount = inlineValue.speciesCount;
        for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
            value.species[i] = inlineValue.species[i];
            value.amounts[i] = inlineValue.amounts[i];
        }

        return true;
    }

    const uint32_t area = WideUniformArea(slot);
    const uint32_t side = WideCurrentSide(area);
    value = RxEmptyCellLike(MidShape(), inlineValue.energy);
    if (!WideFitsShape(RX_GPU_MID_SPECIES, inlineValue, area, side, 0))
        return false;

    value = LoadGpuWideCell(MidShape(), inlineValue, area, side, 0);

    return true;
}

// 一様なブロックを速い形で評価する(溢れを使う変種。T-0384。CPU の MrUniformWaitOf(…, LoadWideUniform, …, true))。
// 一様の値の溢れまで読む。溢れの無い値も速い形で評価する: 8 種以下の間はインラインの形とビット単位で同じ(T-0175)で、9 種目を作るセルは
// インラインなら待たせた印(UNIFORM_EXPANDS_HELD で広げる)・速い形なら変わる(広げる)なので、どちらも頁に広げる。速い形の上限(32 種)で
// 待たせたセルも広げる(上限の無い形では変わる = 広げる)。反応の核の展開はインラインの評価を置き換えるので増えない。
// 速い形に入らない値(33 種以上)は評価せずに頁に広げる(仮。CPU は上限の無い形で評価して、変わらなければ一様のまま = 食い違う。T-0389)
WaitBlockResult EvaluateUniformWaitWide(uint32_t slot, uint32_t thread) {
    BeginWaitReduce(thread);
    const MrBlock block = g_blocks[slot];
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    uint64_t wakeTick = RX_WAIT_NEVER;
    RxGpuMidCell value;
    if (!LoadUniformMid(slot, value)) {
        InterlockedOr(gs_waitChanged, 1u);
        return EndWaitReduce(thread, wakeTick);
    }

    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        const uint32_t index = thread + (WAIT_STEP_THREADS * k);
        if (!MrIsSteppedCell(block, index))
            continue;

        const RxWaitStepOf<RxGpuMidCell> step = MrStepCellWait(MakeStepTable(), value, seed, tick, block, index);
        wakeTick = MinTick(wakeTick, step.wakeTick);
        if (!SameGpuWideCell(step.cell, value) || (step.limits & RX_LIMIT_PRODUCTS) != 0)
            InterlockedOr(gs_waitChanged, 1u);
    }

    return EndWaitReduce(thread, wakeTick);
}

// 頁に広げたブロック(枠 slot)の頁の面 0 へ、一様の値の溢れを覆われていないセル全部に写す(CPU の ExpandWantedPages の
// StoreWidePageCell。T-0384)。塊は TreeExpand の溢れの変種が配ってある(置き場が尽きて足りなければ写さずに MR_COUNTER_OVERFLOW に
// 数える = CPU と食い違う。細かくする時と同じ)。並びはセルの番号の順(覆われていないセルが溢れの数ずつ)。
// ClearWidePage の後に全部のスレッドが呼ぶ(終わりに同期)
void CopyUniformTails(uint32_t slot, MrBlock block, uint32_t thread) {
    const uint32_t count = UniformTailCount(slot);
    const uint32_t entries = count * UncoveredCellCount(block);
    if (entries == 0)
        return;

    if (MrWideChunksFor(entries) > WideChunkCount(block.page, 0)) {
        if (thread == 0)
            InterlockedAdd(g_counters[MR_COUNTER_OVERFLOW], 1u);

        return;
    }

    // --- 始まりの表(スレッド 0 のプレフィックス和)---
    if (thread == 0) {
        uint32_t total = 0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            gs_wideOffsets[index] = total;
            total += MrIsCoveredCell(block, index) ? 0u : count;
        }

        g_treeWords[WideWord(block.page, 0, MR_BLOCK_CELLS)] = total;
    }

    GroupMemoryBarrierWithGroupSync();

    // --- 成分を写す(一様の値の溢れはどのセルも同じ)---
    const uint32_t area = WideUniformArea(slot);
    const uint32_t side = WideCurrentSide(area);
    const uint32_t first = WideRange(area, side, 0).x;
    for (uint32_t index = thread; index < MR_BLOCK_CELLS; index += WAIT_STEP_THREADS) {
        const uint32_t start = gs_wideOffsets[index];
        g_treeWords[WideWord(block.page, 0, index)] = start;
        if (MrIsCoveredCell(block, index))
            continue;

        for (uint32_t e = 0; e < count; ++e) {
            const uint32_t from = MrWideEntryWord(first + e);
            const uint32_t to = MrWideEntryWord(start + e);
            for (uint32_t w = 0; w < 3; ++w)
                g_treeWords[WideWord(block.page, 0, to + w)] = g_treeWords[WideWord(area, side, from + w)];
        }
    }

    AllMemoryBarrierWithGroupSync();
}
#endif

// 一様なブロックを評価する(溢れを使う変種は EvaluateUniformWaitWide。T-0384)
WaitBlockResult EvaluateUniformWaitShaped(uint32_t slot, uint32_t thread) {
#ifdef MR_WIDE_CELLS
    return EvaluateUniformWaitWide(slot, thread);
#else
    return EvaluateUniformWait(slot, thread);
#endif
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

    const WaitBlockResult result = EvaluateUniformWaitShaped(slot, thread);
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
    ClearWidePage(block.page, thread);      // 前に使った頁の溢れを読まない
    CopyUniformTails(slot, block, thread);  // 一様の値の溢れごと(T-0384)
#endif

    StepPagedWaitShaped(slot, thread);  // 同じスレッドが同じセルを受け持つので、埋めた後に同期は要らない
    if (thread == 0)
        FinishWaitBlock(slot, 1, RX_WAIT_NEVER);
}

#endif  // BICAMERAL_MULTIRES_WAIT_STEP_HLSLI
