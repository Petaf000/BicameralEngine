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
// 書く面の塊が足りなければそのセルは書かずに、頁の見出しに要求(要る塊の数・刻み直すセルの印・刻みの初めの busyTick)を書く。
// 刻みの段の後に WideAllocate が枠の順(頁の番号の順)に塊を配り、WideRetryBlock が同じ刻みのうちに印のセルだけ ①②③ をやり直す(T-0236)。
// 塊の置き場が尽きた時だけ待たせる(MR_COUNTER_LIMIT_PRODUCTS)。
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

// --- 頁の溢れ(u6 の後ろ。g_overflowBase。並びは shaders/common/multires_wide.hlsli)---

// 頁の数(観察の枠の頁 + 世界の頁。OverflowBaseWord = 世界の枠 × 2 + 索引 + 世界の頁)
uint32_t WidePageCount() {
    return (g_overflowBase - (2 * g_worldBlocks) - g_indexEntries) + (g_blockCount - g_worldBlocks);
}

uint32_t WidePoolWord() {
    return MrWidePoolWord(g_overflowBase, WidePageCount());
}

uint32_t WideHeaderAt(uint32_t page, uint32_t field) {
    return MrWideHeaderWord(g_overflowBase, page) + field;
}

uint32_t WideCurrentSide(uint32_t page) {
    return g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_SIDE)];
}

// 面 side が持つ塊の数
uint32_t WideChunkCount(uint32_t page, uint32_t side) {
    return g_treeWords[MrWideTableWord(g_overflowBase, page, side)];
}

// 面 side の論理の語 word の u6 の語(塊の表を引く。面が持つ塊の中であること)
uint32_t WideWord(uint32_t page, uint32_t side, uint32_t word) {
    const uint32_t table = MrWideTableWord(g_overflowBase, page, side);
    const uint32_t chunk = g_treeWords[table + 1 + (word / MR_WIDE_CHUNK_WORDS)];

    return MrWideChunkWord(WidePoolWord(), chunk) + (word % MR_WIDE_CHUNK_WORDS);
}

// 面 side のセル index の溢れの [始まり, 終わり)(塊を持たない面は溢れが無い)
uint2 WideRange(uint32_t page, uint32_t side, uint32_t index) {
    if (WideChunkCount(page, side) == 0)
        return uint2(0, 0);

    return uint2(g_treeWords[WideWord(page, side, index)], g_treeWords[WideWord(page, side, index + 1)]);
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
    FX_ASSERT(range.y - range.x <= RX_GPU_WIDE_SPECIES - RX_MAX_CELL_SPECIES);
    for (uint32_t e = range.x; e < range.y; ++e) {
        const uint32_t word = MrWideEntryWord(e);
        cell.species[cell.speciesCount] = g_treeWords[WideWord(page, side, word)];
        cell.amounts[cell.speciesCount] = FX_U64(g_treeWords[WideWord(page, side, word + 2)],
                                                 g_treeWords[WideWord(page, side, word + 1)]);
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
    for (uint32_t i = RX_MAX_CELL_SPECIES; i < cell.speciesCount; ++i) {
        const uint32_t word = MrWideEntryWord(first + i - RX_MAX_CELL_SPECIES);
        g_treeWords[WideWord(page, side, word)] = cell.species[i];
        g_treeWords[WideWord(page, side, word + 1)] = (uint32_t)cell.amounts[i];
        g_treeWords[WideWord(page, side, word + 2)] = (uint32_t)(cell.amounts[i] >> 32);
    }
}

// 頁の今の面を面 0 にして空にする(threads スレッドで分ける。同期は呼ぶ側。一様の値は溢れを持たない)。
// 前に使った頁の塊はそのまま持つ(次に書く時に要らなければ WideAllocate が返す)ので、塊を持つなら始まりの表を 0 にする
void ClearWideSideWords(uint32_t page, uint32_t thread, uint32_t threads) {
    if (WideChunkCount(page, 0) != 0) {
        for (uint32_t i = thread; i <= MR_BLOCK_CELLS; i += threads)
            g_treeWords[WideWord(page, 0, i)] = 0;
    }

    if (thread == 0)
        g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_SIDE)] = 0;
}

// 頁を配ったばかりのブロックの今の面を空にする(前に使った頁の溢れを読まないため)
void ClearWidePage(uint32_t page, uint32_t thread) {
    ClearWideSideWords(page, thread, WAIT_STEP_THREADS);
    AllMemoryBarrierWithGroupSync();
}

// 頁のセル index に溢れがあるか(今の面 side。溢れは 8 種のセルにだけある)
bool WideHasTail(RxCell inlineCell, uint32_t page, uint32_t side, uint32_t index) {
    if (inlineCell.speciesCount != RX_MAX_CELL_SPECIES)
        return false;

    const uint2 range = WideRange(page, side, index);

    return range.x != range.y;
}

// 上限の無い形で刻み直すセルを、もう片方の面の並びが決まった後に刻んで書く(StepPagedWaitWide・伝導の段・WideRetryBlock の ③)。
// 入らない(fits でない)なら書かない: 見出しに要求を書いてあるので、WideRetryBlock が塊を配った後に刻み直す(T-0236。
// 起こす刻み・上限の印もその時に足す)。変わったら true
bool RestepWideCell(MrBlock block, uint32_t index, uint32_t side, bool fits, uint32_t first, inout uint64_t wakeTick,
                    inout MrLimitTally tally) {
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    if (!fits)
        return false;

    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const RxGpuWideCell before = LoadGpuWideCell(g_cells[PageCellAddress(block.page, index)], block.page, side, index);
    const RxWaitStepOf<RxGpuWideCell> step = MrStepCellWait(MakeTable(), before, seed, tick, block, index);
    wakeTick = MinTick(wakeTick, step.wakeTick);
    tally = MrAddLimits(tally, step.limits);
    StoreGpuWideCell(step.cell, PageCellAddress(block.page, index), block.page, 1 - side, first);

    return !SameGpuWideCell(before, step.cell);
}

// --- 刻む ---

groupshared uint32_t gs_wideOffsets[MR_BLOCK_CELLS];     // ① セルの溢れの数 → ② 新しい面の始まり
groupshared uint32_t gs_wideAny;                         // 上限の無い形で刻むセルがある
groupshared uint32_t gs_wideTotal;                       // 新しい面の溢れの数
groupshared uint32_t gs_wideMarks[MR_BLOCK_CELLS / 32];  // ビット = 上限の無い形で刻み直すセル(伝導の段。T-0211)

// ① の初め: 自分の受け持つセルの溢れの数と印を 0 に(threads スレッド。同期は呼ぶ側)
void BeginWideCells(uint32_t thread, uint32_t threads) {
    if (thread == 0)
        gs_wideAny = 0;

    for (uint32_t index = thread; index < MR_BLOCK_CELLS; index += threads)
        gs_wideOffsets[index] = 0;

    for (uint32_t word = thread; word < MR_BLOCK_CELLS / 32; word += threads)
        gs_wideMarks[word] = 0;
}

// ① で上限の無い形に回すセル(inlineCell は伝導の変化を足した後): 刻んだ後の溢れの数を数えて印を付ける(書くのは ③ の RestepWideCell)
void MarkWideCell(MrBlock block, uint32_t index, uint32_t side, RxCell inlineCell) {
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    const RxGpuWideCell wide = LoadGpuWideCell(inlineCell, block.page, side, index);
    gs_wideOffsets[index] = WideTailCount(MrStepCellWait(MakeTable(), wide, seed, tick, block, index).cell);
    InterlockedOr(gs_wideMarks[index / 32], 1u << (index % 32));
    gs_wideAny = 1;
}

bool IsWideMarked(uint32_t index) {
    return (gs_wideMarks[index / 32] & (1u << (index % 32))) != 0;
}

// 書く面の塊が足りない: 要る塊の数・刻み直すセルの印(gs_wideMarks)・刻みの初めの busyTick を見出しに書く(WideAllocate・WideRetryBlock が読む)
void RequestWideRetry(uint32_t page, uint32_t side, uint32_t chunks, uint64_t busyTick, uint32_t thread,
                      uint32_t threads) {
    for (uint32_t word = thread; word < MR_WIDE_MARK_WORDS; word += threads)
        g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_MARKS + word)] = gs_wideMarks[word];

    if (thread != 0)
        return;

    g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_REQUEST)] = MrWideMakeRequest(1 - side, chunks, true);
    g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_BUSY)] = (uint32_t)busyTick;
    g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_BUSY + 1)] = (uint32_t)(busyTick >> 32);
}

// ② セルの番号の順のプレフィックス和(スレッド 0)で新しい面(1 − side)の並びを決め(gs_wideOffsets は溢れの数 → 新しい面の始まり)、
// 書く面の塊に入るなら始まりを新しい面へ書く(余る塊は WideAllocate に返させる)。入らなければ要求を書く(RequestWideRetry)。
// グループの全部のスレッド(threads)が ① の同期の後に呼ぶ。入るなら true。busyTick = 刻みの初めの見出しの値
bool PlanWideSide(uint32_t page, uint32_t side, uint32_t thread, uint32_t threads, uint64_t busyTick) {
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
    const uint32_t need = MrWideChunksFor(gs_wideTotal);
    const uint32_t held = WideChunkCount(page, 1 - side);
    if (need > held) {
        RequestWideRetry(page, side, need, busyTick, thread, threads);
        return false;
    }

    if (need != 0) {
        for (uint32_t index = thread; index < MR_BLOCK_CELLS; index += threads)
            g_treeWords[WideWord(page, 1 - side, index)] = gs_wideOffsets[index];

        if (thread == 0)
            g_treeWords[WideWord(page, 1 - side, MR_BLOCK_CELLS)] = gs_wideTotal;
    }

    if (thread == 0 && need < held)
        g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_REQUEST)] = MrWideMakeRequest(1 - side, need, false);

    return true;
}

// ③ の後: 全部のセルを書いてから面を入れ替える(グループの全部のスレッドが呼ぶ)
void SwapWideSide(uint32_t page, uint32_t side, bool fits, uint32_t thread) {
    AllMemoryBarrierWithGroupSync();
    if (fits && thread == 0)
        g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_SIDE)] = 1 - side;
}

// 上限の無い形で読む 1 セル(今の面)
RxGpuWideCell LoadSteppedWideCell(MrBlock block, uint32_t index, uint32_t side) {
    return LoadGpuWideCell(g_cells[PageCellAddress(block.page, index)], block.page, side, index);
}

// 頁を持つブロックの刻むセルを待ちの丸めで 1 刻み(StepPagedWait の溢れを使う版)
WaitBlockResult StepPagedWaitWide(uint32_t slot, uint32_t thread) {
    BeginWaitReduce(thread);
    BeginWideCells(thread, WAIT_STEP_THREADS);

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
        InterlockedOr(gs_wideMarks[index / 32], 1u << (index % 32));
        const RxGpuWideCell wide = LoadSteppedWideCell(block, index, side);
        gs_wideOffsets[index] = WideTailCount(MrStepCellWait(MakeTable(), wide, seed, tick, block, index).cell);
    }

    GroupMemoryBarrierWithGroupSync();
    if (gs_wideAny == 0) {
        CountLimits(tally);
        return EndWaitReduce(thread, wakeTick);
    }

    // --- ② セルの番号の順のプレフィックス和(新しい面の並び)---
    const bool fits = PlanWideSide(block.page, side, thread, WAIT_STEP_THREADS, block.busyTick);

    // --- ③ 入るなら刻み直して書く(読むのは今の面、書くのはもう片方の面)。入らなければ待たせる ---
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        if ((wideMask & (1u << k)) == 0)
            continue;

        const uint32_t index = thread + (WAIT_STEP_THREADS * k);
        if (RestepWideCell(block, index, side, fits, gs_wideOffsets[index], wakeTick, tally))
            InterlockedOr(gs_waitChanged, 1u);
    }

    // --- 面を入れ替える(全部のセルを書いた後)---
    SwapWideSide(block.page, side, fits, thread);

    CountLimits(tally);

    return EndWaitReduce(thread, wakeTick);
}

// --- 塊が足りなかった頁: 塊を配る・同じ刻みのうちに刻み直す(T-0236)---
// 刻みの段(StepPagedWaitWide・伝導の段の足す段)の後に WideAllocate(1 グループ)→ WideRetryBlock(1 グループ = 1 枠)の順に投げる
// (gpu_multires.cpp の RecordWideRetry)。CPU の上限の無い世界は 1 刻みで全部を書くので、塊を配る順(頁の番号の順)と
// 刻み直しは結果に出ない(置き場が尽きた時だけ待たせる。どの頁が待つかは頁の番号の順で決まる)。

static const uint32_t WIDE_ALLOCATE_THREADS = 256;

groupshared uint32_t gs_allocReleased[WIDE_ALLOCATE_THREADS];  // スレッドの頁が返す塊の数 → 積む位置(プレフィックス和)
groupshared uint32_t
    gs_allocWanted[WIDE_ALLOCATE_THREADS];  // スレッドの頁が欲しい塊の数 → 前の頁が欲しい数(プレフィックス和)
groupshared uint32_t gs_allocGranted[WIDE_ALLOCATE_THREADS];  // スレッドの頁に配った塊の数
groupshared uint32_t gs_allocReleasedTotal;

// 頁の要求で返す塊の数(x)・欲しい塊の数(y)
uint2 WideRequestChange(uint32_t page) {
    const uint32_t request = g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_REQUEST)];
    if (request == 0)
        return uint2(0, 0);

    const uint32_t held = WideChunkCount(page, MrWideRequestSide(request));
    const uint32_t want = MrWideRequestChunks(request);

    return held > want ? uint2(held - want, 0) : uint2(0, want - held);
}

// スレッドごとの数を前のスレッドの和にする(スレッド 0。同期は呼ぶ側)
void PrefixAllocCounts() {
    uint32_t released = 0;
    uint32_t wanted = 0;
    for (uint32_t i = 0; i < WIDE_ALLOCATE_THREADS; ++i) {
        const uint2 counts = uint2(gs_allocReleased[i], gs_allocWanted[i]);
        gs_allocReleased[i] = released;
        gs_allocWanted[i] = wanted;
        released += counts.x;
        wanted += counts.y;
    }

    gs_allocReleasedTotal = released;
}

// 返す: 余る塊を空きのスタックに積む(頁の番号の順)。返すだけの要求はここで消す
void ReleaseWideChunks(uint32_t page, uint32_t stack, inout uint32_t push) {
    const uint32_t requestWord = WideHeaderAt(page, MR_WIDE_HEADER_REQUEST);
    const uint32_t request = g_treeWords[requestWord];
    const uint32_t table = MrWideTableWord(g_overflowBase, page, MrWideRequestSide(request));
    const uint32_t held = g_treeWords[table];
    const uint32_t want = MrWideRequestChunks(request);
    if (request == 0 || held <= want)
        return;

    for (uint32_t i = want; i < held; ++i) {
        g_treeWords[stack + push] = g_treeWords[table + 1 + i];
        push += 1;
    }

    g_treeWords[table] = want;
    g_treeWords[requestWord] = 0;
}

// 配る: 前の頁が欲しい数(taken)と合わせて空きに入れば、スタックの上から取る(入らなければ配らない = WideRetryBlock が待たせる)
uint32_t GrantWideChunks(uint32_t page, uint32_t stack, uint32_t freeCount, inout uint32_t taken) {
    const uint32_t requestWord = WideHeaderAt(page, MR_WIDE_HEADER_REQUEST);
    const uint32_t request = g_treeWords[requestWord];
    const uint32_t table = MrWideTableWord(g_overflowBase, page, MrWideRequestSide(request));
    const uint32_t held = g_treeWords[table];
    const uint32_t want = MrWideRequestChunks(request);
    if (request == 0 || want <= held)
        return 0;

    const uint32_t count = want - held;
    const bool granted = taken + count <= freeCount;
    if (granted) {
        for (uint32_t j = 0; j < count; ++j)
            g_treeWords[table + 1 + held + j] = g_treeWords[stack + freeCount - 1 - (taken + j)];

        g_treeWords[table] = want;
        g_treeWords[requestWord] = request | MR_WIDE_REQUEST_GRANTED;
    }

    taken += count;

    return granted ? count : 0u;
}

// 塊を配る段(1 グループ。頁を番号の順に区切ってスレッドが受け持つ): 先に全部の頁の余る塊を返し、次に頁の番号の順に配る
void WideAllocate(uint32_t thread) {
    const uint32_t pages = WidePageCount();
    const uint32_t perThread = (pages + WIDE_ALLOCATE_THREADS - 1) / WIDE_ALLOCATE_THREADS;
    const uint32_t first = min(thread * perThread, pages);
    const uint32_t last = min(first + perThread, pages);

    // --- 数える → 前のスレッドの和 ---
    uint2 counts = uint2(0, 0);
    for (uint32_t page = first; page < last; ++page)
        counts += WideRequestChange(page);

    gs_allocReleased[thread] = counts.x;
    gs_allocWanted[thread] = counts.y;
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0)
        PrefixAllocCounts();

    GroupMemoryBarrierWithGroupSync();

    // --- 返す(積む)---
    const uint32_t pool = WidePoolWord();
    const uint32_t stack = MrWideStackWord(pool, g_treeWords[pool + MR_WIDE_POOL_CAPACITY]);
    const uint32_t freeBefore = g_treeWords[pool + MR_WIDE_POOL_FREE];
    uint32_t push = freeBefore + gs_allocReleased[thread];
    for (uint32_t page = first; page < last; ++page)
        ReleaseWideChunks(page, stack, push);

    AllMemoryBarrierWithGroupSync();

    // --- 配る(上から取る)---
    const uint32_t freeCount = freeBefore + gs_allocReleasedTotal;
    uint32_t taken = gs_allocWanted[thread];
    uint32_t granted = 0;
    for (uint32_t page = first; page < last; ++page)
        granted += GrantWideChunks(page, stack, freeCount, taken);

    gs_allocGranted[thread] = granted;
    GroupMemoryBarrierWithGroupSync();
    if (thread != 0)
        return;

    uint32_t grantedTotal = 0;
    for (uint32_t i = 0; i < WIDE_ALLOCATE_THREADS; ++i)
        grantedTotal += gs_allocGranted[i];

    g_treeWords[pool + MR_WIDE_POOL_FREE] = freeCount - grantedTotal;
}

// 置き場が尽きた: 印のセルを待たせる(MR_COUNTER_LIMIT_PRODUCTS。T-0211 までの当座のふるまいと同じ)
void HoldWideCells(uint32_t thread, inout uint64_t wakeTick, inout MrLimitTally tally) {
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        if (IsWideMarked(thread + (WAIT_STEP_THREADS * k))) {
            wakeTick = MinTick(wakeTick, CurrentChangeMark() + 1);
            tally.productsHeld += 1;
        }
    }
}

// 塊を配れた: 印のセルだけ ①②③ をやり直す(印のセルは刻みの段で書いていない = 刻む前の値〔伝導の段は変化を足した値〕のまま)
void RestepWideMarked(MrBlock block, uint32_t side, uint32_t thread, inout uint64_t wakeTick,
                      inout MrLimitTally tally) {
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        const uint32_t index = thread + (WAIT_STEP_THREADS * k);
        if (IsWideMarked(index))
            MarkWideCell(block, index, side, g_cells[PageCellAddress(block.page, index)]);
    }

    GroupMemoryBarrierWithGroupSync();
    const bool fits = PlanWideSide(block.page, side, thread, WAIT_STEP_THREADS, block.busyTick);
    FX_ASSERT(fits);
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        const uint32_t index = thread + (WAIT_STEP_THREADS * k);
        if (IsWideMarked(index) && RestepWideCell(block, index, side, fits, gs_wideOffsets[index], wakeTick, tally))
            InterlockedOr(gs_waitChanged, 1u);
    }

    SwapWideSide(block.page, side, fits, thread);
}

// 塊が足りなかったブロック(枠 slot。刻む枠)の刻み直し(1 グループ = 1 枠、WAIT_STEP_THREADS スレッド)。見出しは刻みの段が
// 印のセルを除いて書いてあるので、変わったら busyTick = 印・wakeTick = 印 + 1、変わらなければ wakeTick を印のセルの最小と合わせる
void WideRetryBlock(uint32_t slot, uint32_t thread) {
    MrBlock block = g_blocks[slot];
    if (MrIsUniform(block))
        return;

    const uint32_t request = g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_REQUEST)];
    if ((request & MR_WIDE_REQUEST_RETRY) == 0)
        return;

    // --- 刻みの初めの busyTick と印を戻す ---
    block.busyTick = FX_U64(g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_BUSY + 1)],
                            g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_BUSY)]);
    const uint32_t side = WideCurrentSide(block.page);
    BeginWaitReduce(thread);
    BeginWideCells(thread, WAIT_STEP_THREADS);
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t word = thread; word < MR_WIDE_MARK_WORDS; word += WAIT_STEP_THREADS)
        gs_wideMarks[word] = g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_MARKS + word)];

    GroupMemoryBarrierWithGroupSync();

    // --- 刻み直す(配れなければ待たせる)---
    uint64_t wakeTick = RX_WAIT_NEVER;
    MrLimitTally tally = MrMakeLimitTally();
    if ((request & MR_WIDE_REQUEST_GRANTED) != 0)
        RestepWideMarked(block, side, thread, wakeTick, tally);
    else
        HoldWideCells(thread, wakeTick, tally);

    CountLimits(tally);
    const WaitBlockResult result = EndWaitReduce(thread, wakeTick);
    if (thread != 0)
        return;

    // --- 見出し(要求を消す)---
    g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_REQUEST)] = 0;
    const uint64_t mark = CurrentChangeMark();
    if (result.changed != 0) {
        g_blocks[slot].busyTick = mark;
        g_blocks[slot].wakeTick = mark + 1;
        return;
    }

    g_blocks[slot].wakeTick = MinTick(g_blocks[slot].wakeTick, result.wakeTick);
}

#endif  // BICAMERAL_MULTIRES_WIDE_STEP_HLSLI
