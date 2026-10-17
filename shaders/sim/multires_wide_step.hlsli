// multires_wide_step.hlsli — 溢れを持つ頁のブロックを待ちの丸めで 1 刻み(成分の二段の GPU 版。T-0176・02 §3・17 R-MULTI-4)。
// multires_wait_step.hlsli が MR_WIDE_CELLS の時だけ使う(全部を刻む Compute の変種 multires_step_wide_*。活性のグラフには入れない:
// グラフのノードに反応の核を足さない約束。T-0124)。CPU リファレンスは multires_nest.cpp の StepPagedBlock(溢れを使う世界)。
// 溢れの並びは shaders/common/multires_wide.hlsli。
//
// 1 グループ = 1 ブロック(64 スレッド、1 スレッド 8 セル)。セルどうしは独立なので、成分が 8 種に収まるセルはインラインの核(RxCell)で
// 刻んでその場で書く(上限に当たらない間は上限なしの形とビット単位で同じ。T-0175)。溢れを持つセル・インラインで 9 種目の生成物を
// 待たせたセルだけ、上限の無い形で刻み直す(全部を刻む段・WideDeferred・WideRetry は速い形 RxGpuMidCell〔32 種〕だけ。入らないセルが
// あるブロックは作業場の段 WideBigPass が速い形・作業場の形 RxGpuSpaceCell〔表の物質の数まで〕で刻む。T-0326・T-0362):
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
#include "sim/multires_wide_access.hlsli"

// --- 上限の無い形のセル: 速い形(RX_GPU_MID_SPECIES = 32 種)と作業場の形(T-0362)。reaction.hlsli の「セルの形ごとの道具」の版 ---
// 速い形の配列は動的な添字なのでレジスタではなくスクラッチに置かれうる。持つのは 64 スレッドの段(全部を刻む・頁に広げて刻む・WideRetry・
// WideDeferred・WideBigPass)だけ。512 スレッドの伝導の段に持たせると上限 56 以上で HW の compute キューが止まった(T-0290)ので、T-0272 で
// 伝導の段の熱は溢れから直接足し(WideTailThermal)、反応は WideDeferredPass に回した。上限の大きさの配列を 64 スレッドの段に持たせても
// 128 種で止まった(T-0327)ので、速い形に入らないセルは作業場の形 RxGpuSpaceCell(成分を u6 の作業場に置き、核は reaction.hlsli の
// 「成分を読む・書く・写す」道具の特殊化で読み書きする)で作業場の段 WideBigPass だけが刻む(1 スレッドのスクラッチが上限に依らない)。
// 速い形に入らないセル(32 種を超えて読む・生成物で 32 種を超える)が 1 つでもあるブロックは、溢れるセルを全部後の段へ回す
// (全部を刻む段 → WideDeferredPass、WideDeferred・WideRetry → WideBigPass。並びはブロックの全部の溢れの数で決まるので一部だけは書けない)。
// 上限に当たらない間は形の大きさに依らずビット単位で同じ(形の道具が違うのは「成分をもう 1 つ増やせるか」だけ。T-0175 のインラインと同じ)。
struct RxGpuMidCell {
    int64_t energy;
    uint32_t speciesCount;
    uint32_t species[RX_GPU_MID_SPECIES];
    uint64_t amounts[RX_GPU_MID_SPECIES];
};

struct RxGpuMidUsage {
    uint64_t amounts[RX_GPU_MID_SPECIES];
    uint64_t heat;
    uint64_t gas;
};

// --- 速い形の道具 ---

RxGpuMidCell RxMakeEmptyGpuMidCell(int64_t energy) {
    RxGpuMidCell empty;
    empty.energy = energy;
    empty.speciesCount = 0;
    for (uint32_t i = 0; i < RX_GPU_MID_SPECIES; ++i) {
        empty.species[i] = 0;
        empty.amounts[i] = 0;
    }

    return empty;
}

RxGpuMidCell RxEmptyCellLike(RxGpuMidCell cell, int64_t energy) {
    return RxMakeEmptyGpuMidCell(energy);
}

uint32_t RxGpuCapacity(RxGpuMidCell cell) {
    return RX_GPU_MID_SPECIES;
}

bool RxHasRoomForSpecies(RxGpuMidCell cell) {
    return cell.speciesCount < RX_GPU_MID_SPECIES;
}

RxGpuMidCell RxCellWithRoom(RxGpuMidCell cell) {
    return cell;
}

RxGpuMidUsage RxEmptyUsage(RxGpuMidCell cell) {
    RxGpuMidUsage usage;
    for (uint32_t i = 0; i < RX_GPU_MID_SPECIES; ++i)
        usage.amounts[i] = 0;

    usage.heat = 0;
    usage.gas = 0;

    return usage;
}

// 形を選ぶための空のセル(中身は使わない。型だけ)
RxGpuMidCell MidShape() {
    return RxMakeEmptyGpuMidCell(0);
}

#ifdef MR_WIDE_BIG_PASS
#ifndef RX_CELL_ACCESSORS
#error 作業場の段は核の成分の道具(RX_CELL_ACCESSORS。reaction.hlsli)を使う
#endif

// --- 作業場の形(T-0362)---
// 作業場の段(WideBigPass)だけがコンパイルする。g_external0 = 作業場の始まり(u6 の語)、g_external1 = 1 セルの成分の上限(表の物質の数。
// RX_GPU_WIDE_SPECIES まで)。並びは shaders/common/multires_wide.hlsli の「作業場」。スレッドのレーンは [刻む前 A][使う量][書く B × 8][位置の表][① の結果 × 8]
// (B と ① の結果は受け持つ 8 セルごと。③ は刻み直さずに写す。T-0380)。
// セルは「成分 0 の語」を指す小さな値で、写しても同じ場所を指す。核の中で元のセルを残す所(反応物を引く前)は RxWritableCell が
// A から B へ写す。RxCompactCell・RxTryAddSpecies は同じ場所で詰める・ずらす(書く位置が読む位置を追い越さない)

static uint32_t s_spaceGroup = 0;  // 作業場の段のグループの番号(WideBigPass が初めに書く。スレッドごとの値)

struct RxGpuSpaceCell {
    int64_t energy;
    uint32_t speciesCount;
    uint32_t word;  // 成分 0 の u6 の語(成分 i の語 k は SpaceWord(word, 3i + k): 物質 ID・µmol の下位・上位)
    uint32_t lane;  // スレッドのレーンの先頭(A = lane、使う量 = SpaceWord(lane, 3c))
    uint32_t
        write;  // このセルの書くセル B(SpaceWord(lane, 5c + 3ck)。k = セルの番号 / 64。① の結果を ③ まで残す。T-0380)
};

struct RxGpuSpaceUsage {
    uint32_t word;  // 成分 i の使う量は SpaceWord(word, 2i)(下位・上位)
    uint64_t heat;
    uint64_t gas;
};

uint32_t SpaceCapacity() {
    return g_external1;
}

// スレッド thread のレーンの先頭。グループの 64 スレッドのレーンは語ごとに交互に並ぶ(レーンの k 語目 = 先頭 + 64k。T-0380):
// 同じ成分を読む隣のスレッドの語が並ぶので、ワープの読み書きがまとまる(前はレーンを続けて置き、隣のスレッドが 8c 語離れていた)
uint32_t SpaceLaneWord(uint32_t thread) {
    const uint32_t laneWords = (MR_WIDE_SPACE_LANE_WORDS_PER_SPECIES * SpaceCapacity()) +
                               (MR_WIDE_SPACE_RESULT_WORDS * MR_WIDE_SPACE_CELLS_PER_THREAD);

    return g_external0 + (s_spaceGroup * WAIT_STEP_THREADS * laneWords) + thread;
}

// レーンの中の語 word から offset 語先(交互に並ぶので 64 語ずつ進む)
uint32_t SpaceWord(uint32_t word, uint32_t offset) {
    return word + (MR_WIDE_SPACE_THREADS * offset);
}

template <>
uint32_t RxSpeciesAt<RxGpuSpaceCell>(RxGpuSpaceCell cell, uint32_t i) {
    return g_treeWords[SpaceWord(cell.word, MR_WIDE_ENTRY_WORDS * i)];
}

// 物質 ID speciesId の位置の表の語(レーンの書くセルの後ろ c 語。T-0380)。RxWithEntry が書き、RxFindSlot が引く
uint32_t SpaceSlotWord(uint32_t lane, uint32_t speciesId) {
    return SpaceWord(lane, ((MR_WIDE_SPACE_LANE_WORDS_PER_SPECIES - 1) * SpaceCapacity()) + speciesId);
}

// 物質 ID の位置(T-0380。核の RxFindSlot の作業場の形の版): 位置の表が指す成分が同じ物質なら、それが前から探した位置と同じ
// (1 セルに同じ物質の成分は 1 つだけ)。表が古い(前のセル・別の書くセルの位置・まだ書いていない)と違う物質を指すので、前から探す。
// 成分を前から探すと 1 セルの成分の数に比例して読む(作業場の段の候補を集める所が成分の数の 2 乗になっていた)
template <>
uint32_t RxFindSlot<RxGpuSpaceCell>(RxGpuSpaceCell cell, uint32_t speciesId) {
    if (speciesId < SpaceCapacity()) {
        const uint32_t slot = g_treeWords[SpaceSlotWord(cell.lane, speciesId)];
        if (slot < cell.speciesCount && RxSpeciesAt(cell, slot) == speciesId)
            return slot;
    }

    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        if (RxSpeciesAt(cell, i) == speciesId)
            return i;
    }

    return RX_NO_SLOT;
}

template <>
uint64_t RxAmountAt<RxGpuSpaceCell>(RxGpuSpaceCell cell, uint32_t i) {
    const uint32_t word = MR_WIDE_ENTRY_WORDS * i;

    return FX_U64(g_treeWords[SpaceWord(cell.word, word + 2)], g_treeWords[SpaceWord(cell.word, word + 1)]);
}

template <>
RxGpuSpaceCell RxWithAmount<RxGpuSpaceCell>(RxGpuSpaceCell cell, uint32_t i, uint64_t amount) {
    const uint32_t word = MR_WIDE_ENTRY_WORDS * i;
    g_treeWords[SpaceWord(cell.word, word + 1)] = (uint32_t)amount;
    g_treeWords[SpaceWord(cell.word, word + 2)] = (uint32_t)(amount >> 32);

    return cell;
}

template <>
RxGpuSpaceCell RxWithEntry<RxGpuSpaceCell>(RxGpuSpaceCell cell, uint32_t i, uint32_t speciesId, uint64_t amount) {
    const uint32_t word = MR_WIDE_ENTRY_WORDS * i;
    g_treeWords[SpaceWord(cell.word, word)] = speciesId;
    if (speciesId < SpaceCapacity())
        g_treeWords[SpaceSlotWord(cell.lane, speciesId)] = i;

    g_treeWords[SpaceWord(cell.word, word + 1)] = (uint32_t)amount;
    g_treeWords[SpaceWord(cell.word, word + 2)] = (uint32_t)(amount >> 32);

    return cell;
}

// 刻む前のセル(A)を書くセル(B)へ写す。B を指していればそのまま
template <>
RxGpuSpaceCell RxWritableCell<RxGpuSpaceCell>(RxGpuSpaceCell cell) {
    RxGpuSpaceCell copy = cell;
    copy.word = cell.write;
    if (copy.word == cell.word)
        return copy;

    for (uint32_t w = 0; w < MR_WIDE_ENTRY_WORDS * cell.speciesCount; ++w)
        g_treeWords[SpaceWord(copy.word, w)] = g_treeWords[SpaceWord(cell.word, w)];

    return copy;
}

template <>
uint64_t RxUsageAt<RxGpuSpaceUsage>(RxGpuSpaceUsage usage, uint32_t i) {
    return FX_U64(g_treeWords[SpaceWord(usage.word, (2 * i) + 1)], g_treeWords[SpaceWord(usage.word, 2 * i)]);
}

template <>
RxGpuSpaceUsage RxWithUsage<RxGpuSpaceUsage>(RxGpuSpaceUsage usage, uint32_t i, uint64_t amount) {
    g_treeWords[SpaceWord(usage.word, 2 * i)] = (uint32_t)amount;
    g_treeWords[SpaceWord(usage.word, (2 * i) + 1)] = (uint32_t)(amount >> 32);

    return usage;
}

// 同じ形の空のセル: 同じ場所を指す(RxCompactCell が同じ場所で詰める。LoadGpuWideCell は A に読む)
RxGpuSpaceCell RxEmptyCellLike(RxGpuSpaceCell cell, int64_t energy) {
    RxGpuSpaceCell empty = cell;
    empty.energy = energy;
    empty.speciesCount = 0;

    return empty;
}

uint32_t RxGpuCapacity(RxGpuSpaceCell cell) {
    return SpaceCapacity();
}

bool RxHasRoomForSpecies(RxGpuSpaceCell cell) {
    return cell.speciesCount < SpaceCapacity();
}

RxGpuSpaceCell RxCellWithRoom(RxGpuSpaceCell cell) {
    return cell;
}

// 0 の使う量(核が添字に使うのは成分の位置 < speciesCount だけなので、そこまで 0 にする)
RxGpuSpaceUsage RxEmptyUsage(RxGpuSpaceCell cell) {
    RxGpuSpaceUsage usage;
    usage.word = SpaceWord(cell.lane, MR_WIDE_ENTRY_WORDS * SpaceCapacity());
    usage.heat = 0;
    usage.gas = 0;
    for (uint32_t w = 0; w < 2 * cell.speciesCount; ++w)
        g_treeWords[SpaceWord(usage.word, w)] = 0;

    return usage;
}

// 頁のセル index の作業場の形(受け持つスレッド index % 64 の A を指す空のセル。書くセルは index / 64 番目の B)
RxGpuSpaceCell SpaceShape(uint32_t index) {
    const uint32_t k = index / WAIT_STEP_THREADS;
    RxGpuSpaceCell shape;
    shape.energy = 0;
    shape.speciesCount = 0;
    shape.lane = SpaceLaneWord(index % WAIT_STEP_THREADS);
    shape.word = shape.lane;
    shape.write = SpaceWord(shape.lane, (MR_WIDE_ENTRY_WORDS + 2 + (MR_WIDE_ENTRY_WORDS * k)) * SpaceCapacity());

    return shape;
}

// 頁のセル index の ① の結果の語(レーンの書くセルの後ろ)
uint32_t SpaceResultWord(uint32_t index) {
    const uint32_t k = index / WAIT_STEP_THREADS;
    const uint32_t offset = (MR_WIDE_SPACE_LANE_WORDS_PER_SPECIES * SpaceCapacity()) + (MR_WIDE_SPACE_RESULT_WORDS * k);

    return SpaceWord(SpaceLaneWord(index % WAIT_STEP_THREADS), offset);
}
#endif

// --- 形に依らない道具 ---

template <typename Cell>
bool SameGpuWideCell(Cell a, Cell b) {
    if (a.energy != b.energy || a.speciesCount != b.speciesCount)
        return false;

    for (uint32_t i = 0; i < a.speciesCount; ++i) {
        if (RX_SPECIES_AT(a, i) != RX_SPECIES_AT(b, i) || RX_AMOUNT_AT(a, i) != RX_AMOUNT_AT(b, i))
            return false;
    }

    return true;
}

// 今の成分(インライン + 面 side の溢れ)が capacity 種の形に入るか
bool WideFitsShape(uint32_t capacity, RxCell inlineCell, uint32_t page, uint32_t side, uint32_t index) {
    if (inlineCell.speciesCount < RX_MAX_CELL_SPECIES)
        return true;

    const uint2 range = WideRange(page, side, index);

    return RX_MAX_CELL_SPECIES + (range.y - range.x) <= capacity;
}

// インラインのセルに溢れ(面 side)を足して形 shape にする(CPU の LoadWideNestCell。入ることは呼ぶ側が WideFitsShape で確かめる)
template <typename Cell>
Cell LoadGpuWideCell(Cell shape, RxCell inlineCell, uint32_t page, uint32_t side, uint32_t index) {
    Cell cell = RxEmptyCellLike(shape, inlineCell.energy);
    cell.speciesCount = inlineCell.speciesCount;
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        RX_SET_ENTRY(cell, i, inlineCell.species[i], inlineCell.amounts[i]);
    }

    if (inlineCell.speciesCount < RX_MAX_CELL_SPECIES)
        return cell;

    const uint2 range = WideRange(page, side, index);
    FX_ASSERT(range.y - range.x <= RxGpuCapacity(shape) - RX_MAX_CELL_SPECIES);
    for (uint32_t e = range.x; e < range.y; ++e) {
        const uint32_t word = MrWideEntryWord(e);
        const uint64_t amount = FX_U64(g_treeWords[WideWord(page, side, word + 2)],
                                       g_treeWords[WideWord(page, side, word + 1)]);
        RX_SET_ENTRY(cell, cell.speciesCount, g_treeWords[WideWord(page, side, word)], amount);
        cell.speciesCount += 1;
    }

    return cell;
}

// 溢れの数(インラインに入らない成分)
template <typename Cell>
uint32_t WideTailCount(Cell cell) {
    return cell.speciesCount > RX_MAX_CELL_SPECIES ? cell.speciesCount - RX_MAX_CELL_SPECIES : 0u;
}

// インラインを頁へ(使わない枠は 0。CPU の StoreWidePageCell)、残りを面 side の first からへ書く
template <typename Cell>
void StoreGpuWideCell(Cell cell, uint32_t address, uint32_t page, uint32_t side, uint32_t first) {
    RxCell narrow = RxMakeEmptyCell(cell.energy);
    narrow.speciesCount = min(cell.speciesCount, RX_MAX_CELL_SPECIES);
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < narrow.speciesCount) {
            narrow.species[i] = RX_SPECIES_AT(cell, i);
            narrow.amounts[i] = RX_AMOUNT_AT(cell, i);
        }
    }

    g_cells[address] = narrow;
    for (uint32_t i = RX_MAX_CELL_SPECIES; i < cell.speciesCount; ++i) {
        const uint32_t word = MrWideEntryWord(first + i - RX_MAX_CELL_SPECIES);
        const uint64_t amount = RX_AMOUNT_AT(cell, i);
        g_treeWords[WideWord(page, side, word)] = RX_SPECIES_AT(cell, i);
        g_treeWords[WideWord(page, side, word + 1)] = (uint32_t)amount;
        g_treeWords[WideWord(page, side, word + 2)] = (uint32_t)(amount >> 32);
    }
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

// 上限の無い形で 1 セルを刻んだ結果(① は溢れの数だけ使い、③ は書いて変わったか)
struct WideCellStep {
    uint32_t tail;    // 刻んだ後の溢れの数
    uint32_t limits;  // 上限の印(RX_LIMIT_*)
    uint64_t wakeTick;
    bool changed;  // ③: 書いたセルが刻む前と違う
};

// 頁のセル index を形 shape で刻む(読むのは g_cells と今の面 side の溢れ。shape に入ることは呼ぶ側が確かめる)。
// write なら ③: インラインを頁へ・溢れをもう片方の面の first からへ書く(並びは ② で決まっている)
template <typename Cell>
WideCellStep StepWideCellAs(Cell shape, MrBlock block, uint32_t index, uint32_t side, bool write, uint32_t first) {
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    const uint32_t address = PageCellAddress(block.page, index);
    const Cell before = LoadGpuWideCell(shape, g_cells[address], block.page, side, index);
    const RxWaitStepOf<Cell> step = MrStepCellWait(MakeTable(), before, seed, tick, block, index);

    WideCellStep result;
    result.tail = WideTailCount(step.cell);
    result.limits = step.limits;
    result.wakeTick = step.wakeTick;
    result.changed = false;
    if (write) {
        StoreGpuWideCell(step.cell, address, block.page, 1 - side, first);
        result.changed = !SameGpuWideCell(before, step.cell);
    }

    return result;
}

#ifdef MR_WIDE_BIG_PASS
// 作業場の形の ①(T-0380): 刻んで、結果のセルを書くセル B に、起こす刻み・上限の印・変わったかを ① の結果の語に残す
// (③ の StoreSpaceCell が刻み直さずに写す。① と ③ は同じ入力〔刻む前の面・同じ種と刻み〕で同じ式なので、刻み直した時と
// ビット単位で同じ。変わったかの比べ方も前の ③ と同じ〔A の刻む前と結果〕)
WideCellStep StepSpaceCell(MrBlock block, uint32_t index, uint32_t side) {
    const uint64_t seed = FX_U64(g_seedHigh, g_seedLow);
    const uint64_t tick = FX_U64(g_tickHigh, g_tickLow);
    const RxGpuSpaceCell before = LoadGpuWideCell(SpaceShape(index), g_cells[PageCellAddress(block.page, index)],
                                                  block.page, side, index);
    const RxWaitStepOf<RxGpuSpaceCell> step = MrStepCellWait(MakeTable(), before, seed, tick, block, index);

    WideCellStep result;
    result.tail = WideTailCount(step.cell);
    result.limits = step.limits;
    result.wakeTick = step.wakeTick;
    result.changed = !SameGpuWideCell(before, step.cell);

    // --- 結果を残す(核が書かなかったセルは A のままなので B へ写す)---
    const RxGpuSpaceCell kept = RxWritableCell(step.cell);
    const uint32_t word = SpaceResultWord(index);
    g_treeWords[SpaceWord(word, 0)] = (uint32_t)kept.energy;
    g_treeWords[SpaceWord(word, 1)] = (uint32_t)((uint64_t)kept.energy >> 32);
    g_treeWords[SpaceWord(word, 2)] = kept.speciesCount;
    g_treeWords[SpaceWord(word, 3)] = result.limits;
    g_treeWords[SpaceWord(word, 4)] = (uint32_t)result.wakeTick;
    g_treeWords[SpaceWord(word, 5)] = (uint32_t)(result.wakeTick >> 32);
    g_treeWords[SpaceWord(word, 6)] = result.changed ? 1u : 0u;

    return result;
}

// 作業場の形の ③(T-0380): ① の結果を頁へ・溢れをもう片方の面の first からへ写す
WideCellStep StoreSpaceCell(MrBlock block, uint32_t index, uint32_t side, uint32_t first) {
    const uint32_t word = SpaceResultWord(index);
    RxGpuSpaceCell cell = SpaceShape(index);
    cell.word = cell.write;
    cell.energy = (int64_t)FX_U64(g_treeWords[SpaceWord(word, 1)], g_treeWords[SpaceWord(word, 0)]);
    cell.speciesCount = g_treeWords[SpaceWord(word, 2)];
    StoreGpuWideCell(cell, PageCellAddress(block.page, index), block.page, 1 - side, first);

    WideCellStep result;
    result.tail = WideTailCount(cell);
    result.limits = g_treeWords[SpaceWord(word, 3)];
    result.wakeTick = FX_U64(g_treeWords[SpaceWord(word, 5)], g_treeWords[SpaceWord(word, 4)]);
    result.changed = g_treeWords[SpaceWord(word, 6)] != 0;

    return result;
}
#endif

WideCellStep EmptyWideCellStep() {
    WideCellStep step;
    step.tail = 0;
    step.limits = 0;
    step.wakeTick = RX_WAIT_NEVER;
    step.changed = false;

    return step;
}

// ③ で書いたセルの起こす刻み・上限の印を足す(上限の印はブロックが書いた時だけ足す)。変わったら true
bool AddWrittenWideCell(WideCellStep step, inout uint64_t wakeTick, inout MrLimitTally tally) {
    wakeTick = MinTick(wakeTick, step.wakeTick);
    tally = MrAddLimits(tally, step.limits);

    return step.changed;
}

// --- 刻む ---

groupshared uint32_t gs_wideOffsets[MR_BLOCK_CELLS];     // ① セルの溢れの数 → ② 新しい面の始まり
groupshared uint32_t gs_wideAny;                         // 上限の無い形で刻むセルがある
groupshared uint32_t gs_wideTotal;                       // 新しい面の溢れの数
groupshared uint32_t gs_wideMarks[MR_BLOCK_CELLS / 32];  // ビット = 上限の無い形で刻み直すセル(伝導の段。T-0211)
groupshared uint32_t gs_wideBig;                         // 速い形に入らないセルがある(全部を刻む段。T-0326)
groupshared uint32_t
    gs_wideBigMarks[MR_BLOCK_CELLS / 32];  // ビット = 大きい形で刻むセル(WideDeferred・WideRetry。T-0326)

// ① の初め: 自分の受け持つセルの溢れの数と印を 0 に(threads スレッド。同期は呼ぶ側)
void BeginWideCells(uint32_t thread, uint32_t threads) {
    if (thread == 0) {
        gs_wideAny = 0;
        gs_wideBig = 0;
    }

    for (uint32_t index = thread; index < MR_BLOCK_CELLS; index += threads)
        gs_wideOffsets[index] = 0;

    for (uint32_t word = thread; word < MR_BLOCK_CELLS / 32; word += threads) {
        gs_wideMarks[word] = 0;
        gs_wideBigMarks[word] = 0;
    }
}

bool IsWideMarked(uint32_t index) {
    return (gs_wideMarks[index / 32] & (1u << (index % 32))) != 0;
}

bool IsWideBig(uint32_t index) {
    return (gs_wideBigMarks[index / 32] & (1u << (index % 32))) != 0;
}

// 刻み直すセルの印(gs_wideMarks)・刻みの初めの busyTick・要求の語を見出しに書く(RequestWideRetry・RequestWideDeferred。
// WideAllocate・WideRetryBlock・WideDeferredBlock が読む)
void WriteWideRequest(uint32_t page, uint32_t request, uint64_t busyTick, uint32_t thread, uint32_t threads) {
    for (uint32_t word = thread; word < MR_WIDE_MARK_WORDS; word += threads)
        g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_MARKS + word)] = gs_wideMarks[word];

    if (thread != 0)
        return;

    g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_REQUEST)] = request;
    g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_BUSY)] = (uint32_t)busyTick;
    g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_BUSY + 1)] = (uint32_t)(busyTick >> 32);
}

void RequestWideRetry(uint32_t page, uint32_t side, uint32_t chunks, uint64_t busyTick, uint32_t thread,
                      uint32_t threads) {
    WriteWideRequest(page, MrWideMakeRequest(1 - side, chunks, true), busyTick, thread, threads);
}

// 伝導の段(512 スレッド)が印のセルの反応を WideDeferredPass に回す(T-0272。印のセルは変化を足したインラインだけ書いてある)
void RequestWideDeferred(uint32_t page, uint64_t busyTick, uint32_t thread, uint32_t threads) {
    WriteWideRequest(page, MR_WIDE_REQUEST_PRESENT | MR_WIDE_REQUEST_DEFERRED, busyTick, thread, threads);
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

// 頁を持つブロックの刻むセルを待ちの丸めで 1 刻み(StepPagedWait の溢れを使う版)。溢れるセルは速い形で刻み、速い形に入らないセルが
// あれば溢れるセルを全部(① の印)WideDeferredPass に回す(T-0326。並びはブロックの全部の溢れの数で決まるので、一部だけは書けない)
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

    // --- ① 8 種に収まるセルはその場で刻んで書く。残りは速い形で溢れの数だけ数える(入らなければ印だけ)---
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
        if (!WideFitsShape(RX_GPU_MID_SPECIES, before, block.page, side, index)) {
            gs_wideBig = 1;
            continue;
        }

        const WideCellStep counted = StepWideCellAs(MidShape(), block, index, side, false, 0);
        gs_wideOffsets[index] = counted.tail;
        if ((counted.limits & RX_LIMIT_PRODUCTS) != 0)
            gs_wideBig = 1;
    }

    GroupMemoryBarrierWithGroupSync();
    if (gs_wideAny == 0) {
        CountLimits(tally);
        return EndWaitReduce(thread, wakeTick);
    }

    // --- 速い形に入らないセルがある: 印のセルは書かずに WideDeferredPass へ(見出しは印のセルを除いて書く。伝導の段と同じ)---
    if (gs_wideBig != 0) {
        RequestWideDeferred(block.page, block.busyTick, thread, WAIT_STEP_THREADS);
        CountLimits(tally);
        return EndWaitReduce(thread, wakeTick);
    }

    // --- ② セルの番号の順のプレフィックス和(新しい面の並び)---
    const bool fits = PlanWideSide(block.page, side, thread, WAIT_STEP_THREADS, block.busyTick);

    // --- ③ 入るなら刻み直して書く(読むのは今の面、書くのはもう片方の面)。入らなければ WideRetryBlock が刻み直す ---
    for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
        if (!fits || (wideMask & (1u << k)) == 0)
            continue;

        const uint32_t index = thread + (WAIT_STEP_THREADS * k);
        const WideCellStep written = StepWideCellAs(MidShape(), block, index, side, true, gs_wideOffsets[index]);
        if (AddWrittenWideCell(written, wakeTick, tally))
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
        g_treeWords[stack + push] = g_treeWords[WideTableEntryWord(table, i)];
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
            g_treeWords[WideTableEntryWord(table, held + j)] = g_treeWords[stack + freeCount - 1 - (taken + j)];

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

// 印のセル 1 つの ①(write = false: 溢れの数を gs_wideOffsets へ)か ③(write = true: 書く)。速い形に入れば速い形で、入らなければ
// (32 種を超えて読む・生成物で 32 種を超える)作業場の段では作業場の形で刻み、ほかの段では gs_wideBig を立てるだけ(ブロックを作業場の段へ
// 回す。T-0362)。① で決めた形は gs_wideBigMarks に残して ③ も同じ形で刻む。③ で変わったら true
bool StepMarkedWideCell(MrBlock block, uint32_t index, uint32_t side, bool write, inout uint64_t wakeTick,
                        inout MrLimitTally tally) {
    const uint32_t first = write ? gs_wideOffsets[index] : 0u;
    WideCellStep step = EmptyWideCellStep();
    bool done = false;
    if (!IsWideBig(index)) {
        const RxCell inlineCell = g_cells[PageCellAddress(block.page, index)];
        if (write || WideFitsShape(RX_GPU_MID_SPECIES, inlineCell, block.page, side, index)) {
            step = StepWideCellAs(MidShape(), block, index, side, write, first);
            done = write || (step.limits & RX_LIMIT_PRODUCTS) == 0;
        }

        if (!done)
            InterlockedOr(gs_wideBigMarks[index / 32], 1u << (index % 32));
    }

#ifdef MR_WIDE_BIG_PASS
    if (!done && write)
        step = StoreSpaceCell(block, index, side, first);
    else if (!done)
        step = StepSpaceCell(block, index, side);
#else
    if (!done) {
        gs_wideBig = 1;
        return false;
    }
#endif

    if (!write) {
        gs_wideOffsets[index] = step.tail;
        return false;
    }

    return AddWrittenWideCell(step, wakeTick, tally);
}

// RestepWideMarked の結果
static const uint32_t WIDE_RESTEP_DONE = 0;   // 書いた
static const uint32_t WIDE_RESTEP_SHORT = 1;  // 書く面の塊が足りないので書かずに要求を残した
static const uint32_t
    WIDE_RESTEP_BIG = 2;  // 速い形に入らないセルがあるので書かずに作業場の段へ回す(呼ぶ側が要求に印。T-0362)

// 印のセルだけ ①②③ を刻む(印のセルは刻みの段で書いていない = 刻む前の値〔伝導の段は変化を足した値〕のまま)。
// 書く面の塊が足りなければ書かずに要求を残す(PlanWideSide)。
// ① と ③ は同じ繰り返しの 2 回目([loop])にして、反応の核を形ごとに 1 か所にだけ展開する
uint32_t RestepWideMarked(MrBlock block, uint32_t side, uint32_t thread, inout uint64_t wakeTick,
                          inout MrLimitTally tally) {
    bool fits = true;
    bool big = false;
    [loop] for (uint32_t phase = 0; phase < 2; ++phase) {
        const bool write = phase == 1;
        if (write) {
            GroupMemoryBarrierWithGroupSync();
            big = gs_wideBig != 0;  // 作業場の段では立たない
            if (big)
                break;

            fits = PlanWideSide(block.page, side, thread, WAIT_STEP_THREADS, block.busyTick);
        }

        for (uint32_t k = 0; k < MR_BLOCK_CELLS / WAIT_STEP_THREADS; ++k) {
            const uint32_t index = thread + (WAIT_STEP_THREADS * k);
            if (!fits || !IsWideMarked(index))
                continue;

            if (StepMarkedWideCell(block, index, side, write, wakeTick, tally))
                InterlockedOr(gs_waitChanged, 1u);
        }
    }

    if (big)
        return WIDE_RESTEP_BIG;

    SwapWideSide(block.page, side, fits, thread);

    return fits ? WIDE_RESTEP_DONE : WIDE_RESTEP_SHORT;
}

// 見出しの要求(印・刻みの初めの busyTick)を読み戻す: block.busyTick を刻みの初めの値にし、印を gs_wideMarks へ
// (グループの全部のスレッドが呼ぶ。終わりに同期)。今の面を返す
uint32_t BeginWideMarked(inout MrBlock block, uint32_t thread) {
    block.busyTick = FX_U64(g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_BUSY + 1)],
                            g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_BUSY)]);
    const uint32_t side = WideCurrentSide(block.page);
    BeginWaitReduce(thread);
    BeginWideCells(thread, WAIT_STEP_THREADS);
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t word = thread; word < MR_WIDE_MARK_WORDS; word += WAIT_STEP_THREADS)
        gs_wideMarks[word] = g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_MARKS + word)];

    GroupMemoryBarrierWithGroupSync();

    return side;
}

// 印のセルを刻んだ後の見出し(スレッド 0)。見出しは刻みの段が印のセルを除いて書いてあるので、変わったら busyTick = 印・
// wakeTick = 印 + 1、変わらなければ wakeTick を印のセルの最小と合わせる
void FinishWideMarked(uint32_t slot, WaitBlockResult result) {
    const uint64_t mark = CurrentChangeMark();
    if (result.changed != 0) {
        g_blocks[slot].busyTick = mark;
        g_blocks[slot].wakeTick = mark + 1;
        return;
    }

    g_blocks[slot].wakeTick = MinTick(g_blocks[slot].wakeTick, result.wakeTick);
}

// 塊が足りなかったブロック(枠 slot。刻む枠)の刻み直し(1 グループ = 1 枠、WAIT_STEP_THREADS スレッド)
void WideRetryBlock(uint32_t slot, uint32_t thread) {
    MrBlock block = g_blocks[slot];
    if (MrIsUniform(block))
        return;

    const uint32_t request = g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_REQUEST)];
    if ((request & MR_WIDE_REQUEST_RETRY) == 0)
        return;

    const uint32_t side = BeginWideMarked(block, thread);

    // --- 刻み直す(配れなければ待たせる)---
    uint64_t wakeTick = RX_WAIT_NEVER;
    MrLimitTally tally = MrMakeLimitTally();
    if ((request & MR_WIDE_REQUEST_GRANTED) != 0) {
        const uint32_t restep = RestepWideMarked(block, side, thread, wakeTick, tally);
        if (restep == WIDE_RESTEP_BIG) {
            if (thread == 0)
                g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_REQUEST)] = request | MR_WIDE_REQUEST_BIG;

            return;
        }

        FX_ASSERT(restep == WIDE_RESTEP_DONE);
    } else {
        HoldWideCells(thread, wakeTick, tally);
    }

    CountLimits(tally);
    const WaitBlockResult result = EndWaitReduce(thread, wakeTick);
    if (thread != 0)
        return;

    // --- 見出し(要求を消す)---
    g_treeWords[WideHeaderAt(block.page, MR_WIDE_HEADER_REQUEST)] = 0;
    FinishWideMarked(slot, result);
}

// --- 伝導の段が回したセルを刻む(T-0272)---
// 伝導の足す段(512 スレッド = 1 スレッド 1 セル)は上限の無い形のセルを持たず、溢れを持つセル・9 種目を待たせたセルの反応をここへ回す
// (RequestWideDeferred)。1 グループ 64 スレッド = 1 枠で、印のセルだけ StepPagedWaitWide と同じ ①②③ を刻む。書く面の塊が足りなければ
// 要求を書き換えて(RETRY)後の WideAllocate・WideRetryBlock に任せる(前の伝導の段の中の ①②③ と同じ流れ)。
// 刻む関数・順は前と同じ(セルの番号の順のプレフィックス和)なので、結果はビット単位で同じ
void WideDeferredBlock(uint32_t slot, uint32_t thread) {
    MrBlock block = g_blocks[slot];
    if (MrIsUniform(block))
        return;

    const uint32_t requestWord = WideHeaderAt(block.page, MR_WIDE_HEADER_REQUEST);
    if ((g_treeWords[requestWord] & MR_WIDE_REQUEST_DEFERRED) == 0)
        return;

    const uint32_t side = BeginWideMarked(block, thread);

    // --- 要求を消す(② の PlanWideSide が要れば書き直す。全部のスレッドが読み終えた後〔BeginWideMarked の同期〕)---
    if (thread == 0)
        g_treeWords[requestWord] = 0;

    // --- ①②③(塊が足りなければ書かずに要求だけ残す)---
    uint64_t wakeTick = RX_WAIT_NEVER;
    MrLimitTally tally = MrMakeLimitTally();
    const uint32_t restep = RestepWideMarked(block, side, thread, wakeTick, tally);
    if (restep == WIDE_RESTEP_BIG && thread == 0)
        g_treeWords[requestWord] = MR_WIDE_REQUEST_PRESENT | MR_WIDE_REQUEST_DEFERRED | MR_WIDE_REQUEST_BIG;

    if (restep != WIDE_RESTEP_DONE)
        return;

    CountLimits(tally);
    const WaitBlockResult result = EndWaitReduce(thread, wakeTick);
    if (thread == 0)
        FinishWideMarked(slot, result);
}

#ifdef MR_WIDE_BIG_PASS
// --- 速い形に入らないセルのあるブロックを作業場の形で刻む(T-0362)---
// WideDeferred・WideRetry が速い形に入らないセルを見つけたブロック(要求の MR_WIDE_REQUEST_BIG)だけ、回した段と同じ ①②③ を
// 作業場の形で刻み直す(刻む関数・順は同じなので結果はビット単位で同じ)。回した段の後にすぐ流す(WideDeferred → WideBig → 配る →
// 刻み直す → WideBig)。WideDeferred から来たブロックの書く面の塊が足りなければ要求を RETRY に書き換え、後の WideRetry・WideBig が刻む
void WideBigBlock(uint32_t slot, uint32_t thread) {
    MrBlock block = g_blocks[slot];
    if (MrIsUniform(block))
        return;

    const uint32_t requestWord = WideHeaderAt(block.page, MR_WIDE_HEADER_REQUEST);
    const uint32_t request = g_treeWords[requestWord];
    if ((request & MR_WIDE_REQUEST_BIG) == 0)
        return;

    const bool retry = (request & MR_WIDE_REQUEST_DEFERRED) == 0;  // WideRetry から(塊は配ってある)
    const uint32_t side = BeginWideMarked(block, thread);
    if (thread == 0 && !retry)
        g_treeWords[requestWord] = 0;

    uint64_t wakeTick = RX_WAIT_NEVER;
    MrLimitTally tally = MrMakeLimitTally();
    const uint32_t restep = RestepWideMarked(block, side, thread, wakeTick, tally);
    FX_ASSERT(restep != WIDE_RESTEP_BIG);
    FX_ASSERT(!retry || restep == WIDE_RESTEP_DONE);
    if (restep != WIDE_RESTEP_DONE)
        return;

    CountLimits(tally);
    const WaitBlockResult result = EndWaitReduce(thread, wakeTick);
    if (thread != 0)
        return;

    if (retry)
        g_treeWords[requestWord] = 0;

    FinishWideMarked(slot, result);
}
#endif

#endif  // BICAMERAL_MULTIRES_WIDE_STEP_HLSLI
