// multires.hlsli — 多重解像度: ブロックを入れ子に細かくする・粗く戻す・観察の影を親に引き戻す(17 §1・§3・§5。T-0017。ADR-0015)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/multires_nest.cpp)と
// GPU(shaders/sim/multires_graph.hlsl・multires_step.hlsl)が同じ関数を呼ぶので、結果はビット単位で同じになる。
//
// 木の形(ADR-0015): ブロックはどのレベルも 8³ セル。子ブロック(レベル k+1)は親ブロックの八分の一(4³ セル)を覆い、
// 親のセル 1 つ = 子のセル 2³。覆われた親のセルは空(中身は子にある)。
// 量の単位: レベル k の物質量 = 8^-k µmol、エネルギー = 8^-k mJ。同じ濃度・温度ならどのレベルでも同じ数なので、反応の核(reaction.hlsli)は
// レベルを知らずにそのまま使える。
//
// データの流れ:
//   細かくする: 親のセル → 子のセル 2³ に同じ数を写す(余りは出ない)。端数も同じ数を写す。
//   粗くする:   子のセル 2³ の (整数部 · 2^64 + 端数) を足して 3bit 右へ → 親の整数部と端数(MrCoarsenCell)。
//   影:         反応の後、影の子 2³ の合計を「親 × 8」に引き戻す(MrPullBackShadow)。影は親を覆わず、世界に書き戻さない(D-403)。
//
// 頁(T-0102): 見出しの枠とセルの頁(512 セル)は別のプール。一様なブロック(覆われていないセルが全部同じ値・覆われたセルは空・端数なし)は
// 頁を持たず値 1 つ(セルのバッファの先頭の、枠ごとの 1 セル)。セルのバッファの並びは [一様の値 × 枠][頁 × 512 セル]。
// 観察の枠(影・写し)はいつも頁を持つ(頁の番号 = 枠 − 世界の枠の数。世界の頁はその後ろ)。
//
// 端数(ADR-0015): 2^-64 単位。反応は整数部だけを使い、端数は細かく/粗くする時だけ動く。21 段の往復まではビット一致。
// それより深くから粗くして落ちた分は、世界の帳簿に (レベル, 物質) ごとに足す(T-0018。木の管理は multires_tree.hlsli)。
#ifndef BICAMERAL_MULTIRES_HLSLI
#define BICAMERAL_MULTIRES_HLSLI

#include "reaction.hlsli"

#ifdef __cplusplus
#define MR_NAMESPACE_BEGIN               \
    namespace bicameral::multires {      \
        using namespace ::bicameral::fx; \
        using namespace ::bicameral::reaction;
#define MR_NAMESPACE_END }
#else
#define MR_NAMESPACE_BEGIN
#define MR_NAMESPACE_END
#endif

MR_NAMESPACE_BEGIN

// --- 大きさと印 --------------------------------------------------------------------------------
FX_CONST uint32_t MR_BLOCK_EDGE = 8;
FX_CONST uint32_t MR_BLOCK_CELLS = 512;
FX_CONST uint32_t MR_OCTANT_EDGE = 4;  // 子ブロックが覆う親のセルの一辺
FX_CONST uint32_t MR_OCTANT_CELLS = 64;
FX_CONST uint32_t MR_CHILDREN_PER_CELL = 8;  // 親のセル 1 つ = 子のセル 2³
FX_CONST uint32_t MR_LEVEL_SHIFT = 3;        // 1 段で単位が 1/8 = 3bit
FX_CONST uint32_t MR_NO_BLOCK = 0xFFFFFFFFu;
FX_CONST uint32_t MR_NO_FRACTION = 0xFFFFFFFFu;
FX_CONST uint32_t MR_NO_SPECIES = 0xFFFFFFFFu;
FX_CONST uint32_t MR_CELL_ID_PURPOSE = 0x4D520001u;  // セルの ID(乱数の鍵)を作るハッシュの用途
// MrBlock::busyTick: 木の変更でつつかれた(刻む時に刻みの印にする。T-0101)
FX_CONST uint64_t MR_BUSY_POKED = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);
FX_CONST uint32_t MR_NO_PAGE = 0xFFFFFFFFu;      // MrBlock::page: 頁なし = 一様(値はセルのバッファの先頭。T-0102)
FX_CONST uint32_t MR_PAGE_WANTED = 0xFFFFFFFEu;  // 一様で、この刻みに反応が進むので頁に広げる(刻みの中だけ。T-0102)

// ブロックの種類
FX_CONST uint32_t MR_BLOCK_UNUSED = 0;
FX_CONST uint32_t MR_BLOCK_REAL = 1;    // 世界の一部(状態・介入による細分。親を覆う)
FX_CONST uint32_t MR_BLOCK_SHADOW = 2;  // 観察の影(親を覆わない。世界に返さない)
FX_CONST uint32_t MR_BLOCK_MIRROR = 3;  // 世界の写し(木の外の世界から毎回写す。刻まない・読むだけ。影の根の親。T-0096)

// 数える欄(u3 の uint32 の並び)。空きのスタックの数・墓石の数・要求の数は状態の一部(CPU と GPU で一致する)
FX_CONST uint32_t MR_COUNTER_FREE_FRACTIONS = 0;  // 端数の枠の空きのスタックの数(上 = 次に取る所。T-0018)
FX_CONST uint32_t MR_COUNTER_LOST = 1;            // 粗くした時に端数の下から 0 でないビットが落ちた (セル, 成分) の数
FX_CONST uint32_t MR_COUNTER_OVERFLOW = 2;        // 成分がインラインの数を超えて捨てた数(R-MULTI-4)
FX_CONST uint32_t MR_COUNTER_SHADOW_CLAMPED = 3;  // 影の引き戻しでエネルギーの余裕が負だった数
FX_CONST uint32_t MR_COUNTER_FREE_BLOCKS = 4;     // 世界の枠の空きのスタックの数
FX_CONST uint32_t MR_COUNTER_TOMBSTONES = 5;      // 索引の墓石の数(作り直すと 0)
FX_CONST uint32_t MR_COUNTER_REBUILD_INDEX = 6;   // この要求の処理の終わりに索引を作り直すなら 1
FX_CONST uint32_t MR_COUNTER_REQUESTS = 7;        // 要求の一覧の数(処理すると 0)
FX_CONST uint32_t MR_COUNTER_GRANTED = 8;         // 適用した要求の数(累計)
FX_CONST uint32_t MR_COUNTER_ALREADY = 9;         // 既にそうなっていた細かくする要求の数(累計)
FX_CONST uint32_t MR_COUNTER_CONFLICT = 10;       // 同じ刻みの取り合いで後回しにした要求の数(累計)
FX_CONST uint32_t MR_COUNTER_NO_SPACE = 11;       // 枠が足りず後回しにした要求の数(累計)
FX_CONST uint32_t MR_COUNTER_INVALID = 12;  // 無効な要求の数(累計。根が無い・ブロックが無い・子がある・根を粗くする)
FX_CONST uint32_t MR_COUNTER_LEDGER_OUTSIDE = 13;  // 帳簿のレベルの外で落ちた端数の数(帳簿に入らない)
FX_CONST uint32_t MR_COUNTER_INDEX_FULL = 14;      // 索引に入れられなかった数
FX_CONST uint32_t MR_COUNTER_SCHEDULED = 15;       // 活性で刻んだ世界のブロックの数(累計。T-0100)
FX_CONST uint32_t MR_COUNTER_WAKE_TOO_DEEP = 16;   // 面の隣を細かい側へたどる再帰の上限で起こせなかった数(累計。T-0100)
FX_CONST uint32_t MR_COUNTER_QUIET_REQUESTS = 17;  // 静かな葉を粗くする要求を作った数(累計。T-0101)
FX_CONST uint32_t MR_COUNTER_QUIET_DEFERRED = 18;  // 要求の一覧が一杯で次の刻みへ回した静かな葉の数(累計。T-0101)
FX_CONST uint32_t MR_COUNTER_FREE_PAGES = 19;      // 世界の頁の空きのスタックの数(T-0102)
FX_CONST uint32_t MR_COUNTER_EXPANDED = 20;        // 刻むために一様から頁に広げた数(累計。T-0102)
FX_CONST uint32_t
    MR_COUNTER_PAGE_SHORTAGE = 21;         // 頁が足りず、その刻みは刻まずに次へ回した一様なブロックの数(累計。T-0102)
FX_CONST uint32_t MR_COUNTER_FOLDED = 22;  // 静かで一様になった頁を畳んで返した数(累計。T-0103)
// 端数の枠が足りず、熱の伝導で細かい側から粗い側へ整数の単位の倍数だけ送ったブロックの数(累計。T-0019)
FX_CONST uint32_t MR_COUNTER_FRACTION_SHORTAGE = 23;
FX_CONST uint32_t MR_COUNTER_COUNT = 24;

// --- 構造体 ------------------------------------------------------------------------------------

// ブロックの見出し。原点はそのレベルのセルの単位の世界の座標
struct MrBlock {
    int64_t originX;
    int64_t originY;
    int64_t originZ;

    // --- 木 ---
    int32_t level;
    uint32_t kind;          // MR_BLOCK_*
    uint32_t parent;        // 親のブロックの枠(無ければ MR_NO_BLOCK)
    uint32_t parentOctant;  // 親のどの八分の一を覆うか
    uint32_t fraction;      // 端数のブロックの枠(無ければ MR_NO_FRACTION)
    uint32_t children[8];   // 八分の一ごとの本物の子ブロック(影は入れない)
    // --- 活性(世界の要約 HashRealLeaves には入れない)---
    uint32_t activeTick;  // 最後に活性で刻んだ刻みの印(MrActivityMark。T-0100)
    // 最後に変わった刻みの印(MrChangeMark。64bit。ADR-0018 の tc。MR_BUSY_POKED = 木の変更でつつかれ、まだ刻みの印にしていない。T-0101・T-0115)
    uint64_t busyTick;
    // 次に反応の評価が要る刻みの印(待ちの丸め。セルの RxWaitStep::wakeTick の最小。0 = まだ求めていない = すぐ。T-0115)
    uint64_t wakeTick;

    // --- セル(T-0102)---
    uint32_t page;     // セルの頁(MR_NO_PAGE = 一様、MR_PAGE_WANTED = 一様で頁に広げる途中)
    uint32_t padding;  // 8 バイトの倍数にそろえる(CPU と GPU で同じ並び)
};

// セルの端数(2^-64 単位。物質 ID の昇順、0 は持たない)。エネルギーの端数は符号なし(値 = 整数部 + 端数 · 2^-64、整数部は切り捨て)
struct MrFraction {
    uint64_t energy;
    uint32_t speciesCount;
    uint32_t padding;
    uint32_t species[RX_MAX_CELL_SPECIES];
    uint64_t amounts[RX_MAX_CELL_SPECIES];
};

// 親のセル 1 つの子 2³(粗くする入力)
struct MrChildren {
    RxCell cells[8];
    MrFraction fractions[8];
};

// 粗くした時に落ちた下位 3bit を覚える成分の数(セルの整数部と端数に入る成分の数まで。超えたら溢れに数える)
FX_CONST uint32_t MR_MAX_LOST_SPECIES = 2 * RX_MAX_CELL_SPECIES;

struct MrCoarsened {
    RxCell cell;
    MrFraction fraction;
    uint32_t lostCount;      // 端数の下から落ちたビットがあった成分(とエネルギー)の数
    uint32_t overflowCount;  // インラインに入りきらず捨てた成分の数

    // --- 落ちた分(子のレベルの単位 × 2^-64。世界の帳簿へ。T-0018)---
    uint32_t energyLostBits;
    uint32_t lostSpeciesCount;
    uint32_t lostSpecies[MR_MAX_LOST_SPECIES];
    uint32_t lostBits[MR_MAX_LOST_SPECIES];
};

// 影の子 2³(引き戻しの入力と出力)
struct MrShadowFamily {
    RxCell cells[8];
    uint32_t clamped;  // エネルギーの余裕が負で、合計を親に合わせられなかったら 1
};

// 192bit の和(2 の補数。値 = top · 2^128 + hi · 2^64 + lo)
struct MrWide {
    uint64_t top;
    uint64_t hi;
    uint64_t lo;
};

// --- ブロック ----------------------------------------------------------------------------------

// 使っていない枠の見出し(CPU と GPU が同じ値を書く)
FX_FN MrBlock MrMakeUnusedBlock() {
    MrBlock block;
    block.originX = 0;
    block.originY = 0;
    block.originZ = 0;
    block.level = 0;
    block.kind = MR_BLOCK_UNUSED;
    block.parent = MR_NO_BLOCK;
    block.parentOctant = 0;
    block.fraction = MR_NO_FRACTION;
    // HLSL には範囲 for が無い
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (uint32_t i = 0; i < 8; ++i)
        block.children[i] = MR_NO_BLOCK;

    block.activeTick = 0;
    block.busyTick = 0;
    block.wakeTick = 0;
    block.page = MR_NO_PAGE;
    block.padding = 0;

    return block;
}

// 観察の枠の頁(いつも頁を持つ。頁の番号は固定。T-0102)
FX_FN uint32_t MrObserverPage(uint32_t slot, uint32_t worldBlocks) {
    return slot - worldBlocks;
}

// 世界の写しの見出し(セルは呼ぶ側が写す。T-0096)。page は MrObserverPage
FX_FN MrBlock MrMakeMirrorBlock(int32_t level, int64_t originX, int64_t originY, int64_t originZ, uint32_t page) {
    MrBlock block = MrMakeUnusedBlock();
    block.originX = originX;
    block.originY = originY;
    block.originZ = originZ;
    block.level = level;
    block.kind = MR_BLOCK_MIRROR;
    block.page = page;

    return block;
}

// --- 座標 --------------------------------------------------------------------------------------

FX_FN uint32_t MrCellIndex(uint32_t x, uint32_t y, uint32_t z) {
    return x + (MR_BLOCK_EDGE * (y + (MR_BLOCK_EDGE * z)));
}

FX_FN uint32_t MrCellX(uint32_t index) {
    return index & 7u;
}

FX_FN uint32_t MrCellY(uint32_t index) {
    return (index >> 3) & 7u;
}

FX_FN uint32_t MrCellZ(uint32_t index) {
    return index >> 6;
}

// セルがブロックのどの八分の一にあるか(x が 1bit 目、y が 2bit 目、z が 3bit 目)
FX_FN uint32_t MrOctantOfCell(uint32_t index) {
    return (MrCellX(index) >> 2) | ((MrCellY(index) >> 2) << 1) | ((MrCellZ(index) >> 2) << 2);
}

// 八分の一の中の local 番目(0〜63)の親のセルの番号
FX_FN uint32_t MrOctantCell(uint32_t octant, uint32_t local) {
    const uint32_t x = ((octant & 1u) * MR_OCTANT_EDGE) + (local & 3u);
    const uint32_t y = (((octant >> 1) & 1u) * MR_OCTANT_EDGE) + ((local >> 2) & 3u);
    const uint32_t z = (((octant >> 2) & 1u) * MR_OCTANT_EDGE) + (local >> 4);

    return MrCellIndex(x, y, z);
}

// 子ブロックのセル childIndex が重なる親のセル(親ブロックの中の番号)
FX_FN uint32_t MrParentCellOfChild(uint32_t octant, uint32_t childIndex) {
    const uint32_t x = ((octant & 1u) * MR_OCTANT_EDGE) + (MrCellX(childIndex) >> 1);
    const uint32_t y = (((octant >> 1) & 1u) * MR_OCTANT_EDGE) + (MrCellY(childIndex) >> 1);
    const uint32_t z = (((octant >> 2) & 1u) * MR_OCTANT_EDGE) + (MrCellZ(childIndex) >> 1);

    return MrCellIndex(x, y, z);
}

// 八分の一の中の local 番目の親のセルの、j 番目(0〜7)の子のセル(子ブロックの中の番号)
FX_FN uint32_t MrChildCell(uint32_t local, uint32_t j) {
    const uint32_t x = ((local & 3u) * 2) + (j & 1u);
    const uint32_t y = (((local >> 2) & 3u) * 2) + ((j >> 1) & 1u);
    const uint32_t z = ((local >> 4) * 2) + (j >> 2);

    return MrCellIndex(x, y, z);
}

// 子ブロックの原点(1 軸ぶん)。親の原点 + 八分の一のずれを、1 段細かい単位にする
FX_FN int64_t MrChildOrigin(int64_t parentOrigin, uint32_t octantBit) {
    return 2 * (parentOrigin + (int64_t)(octantBit * MR_OCTANT_EDGE));
}

// 点(pointLevel のセルの単位の座標)が、レベル level・原点 origin のブロックのどの八分の一にあるか(1 軸ぶんの 0 か 1)
FX_FN uint32_t MrOctantBitOfPoint(int64_t origin, int32_t level, int64_t coordinate, int32_t pointLevel) {
    FX_ASSERT(pointLevel >= level);
    const int64_t local = (coordinate >> (uint32_t)(pointLevel - level)) - origin;
    FX_ASSERT(local >= 0 && local < (int64_t)MR_BLOCK_EDGE);

    return (uint32_t)(local >> 2);
}

// セルの ID(反応の乱数の鍵)。(レベル, 世界の座標) から作るので、どのレベルでも重ならず、観察の有無で変わらない
FX_FN uint64_t MrCellId(int32_t level, int64_t x, int64_t y, int64_t z) {
    uint64_t hash = FxMix64((uint64_t)MR_CELL_ID_PURPOSE + FX_GOLDEN_GAMMA);
    hash = FxHashCombine(hash, (uint64_t)(int64_t)level);
    hash = FxHashCombine(hash, (uint64_t)x);
    hash = FxHashCombine(hash, (uint64_t)y);

    return FxHashCombine(hash, (uint64_t)z);
}

// --- 端数 --------------------------------------------------------------------------------------

FX_FN MrFraction MrMakeEmptyFraction() {
    MrFraction fraction;
    fraction.energy = 0;
    fraction.speciesCount = 0;
    fraction.padding = 0;
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        fraction.species[i] = 0;
        fraction.amounts[i] = 0;
    }

    return fraction;
}

FX_FN bool MrFractionIsZero(MrFraction fraction) {
    return fraction.energy == 0 && fraction.speciesCount == 0;
}

FX_FN uint64_t MrFractionOf(MrFraction fraction, uint32_t speciesId) {
    for (uint32_t i = 0; i < fraction.speciesCount; ++i) {
        if (fraction.species[i] == speciesId)
            return fraction.amounts[i];
    }

    return 0;
}

FX_FN uint64_t MrAmountOf(RxCell cell, uint32_t speciesId) {
    const uint32_t slot = RxFindSlot(cell, speciesId);
    if (slot == RX_NO_SLOT)
        return 0;

    return cell.amounts[slot];
}

// セルと端数の一覧で、after より大きい最小の物質 ID(無ければ MR_NO_SPECIES)。どちらの一覧も ID の昇順
FX_FN uint32_t MrNextSpecies(RxCell cell, MrFraction fraction, uint32_t after) {
    uint32_t next = MR_NO_SPECIES;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        if (cell.species[i] > after && cell.species[i] < next)
            next = cell.species[i];
    }

    for (uint32_t i = 0; i < fraction.speciesCount; ++i) {
        if (fraction.species[i] > after && fraction.species[i] < next)
            next = fraction.species[i];
    }

    return next;
}

// --- 192bit の和 -------------------------------------------------------------------------------

FX_FN MrWide MrMakeWide() {
    MrWide wide;
    wide.top = 0;
    wide.hi = 0;
    wide.lo = 0;

    return wide;
}

// (整数部 · 2^64 + 端数) を足す。整数部が負なら上の語を符号拡張する
FX_FN MrWide MrWideAdd(MrWide sum, uint64_t whole, bool negative, uint64_t fraction) {
    sum.lo += fraction;
    const uint64_t carryLow = sum.lo < fraction ? 1u : 0u;

    const uint64_t hi = sum.hi + whole;
    const uint64_t carryWhole = hi < whole ? 1u : 0u;
    sum.hi = hi + carryLow;
    const uint64_t carryHigh = sum.hi < carryLow ? 1u : 0u;

    sum.top += carryWhole + carryHigh;
    if (negative)
        sum.top -= 1u;

    return sum;
}

// 和を 1 段粗い単位にする(3bit 右へ)。整数部は上の 2 語、端数は下の 2 語から。落ちる下位 3bit を返す
struct MrWideShifted {
    uint64_t whole;
    uint64_t fraction;
    uint32_t lostBits;
};

FX_FN MrWideShifted MrWideShiftLevel(MrWide sum) {
    MrWideShifted result;
    result.whole = (sum.top << (64 - MR_LEVEL_SHIFT)) | (sum.hi >> MR_LEVEL_SHIFT);
    result.fraction = (sum.hi << (64 - MR_LEVEL_SHIFT)) | (sum.lo >> MR_LEVEL_SHIFT);
    result.lostBits = (uint32_t)(sum.lo & 7u);

    return result;
}

// --- 粗くする ----------------------------------------------------------------------------------

// 粗くした物質 1 つを親のセル(整数部)と端数に足す(ID の昇順に呼ぶ)。入りきらなければ溢れに数える
FX_FN MrCoarsened MrAppendCoarsened(MrCoarsened result, uint32_t species, MrWideShifted shifted) {
    if (shifted.whole != 0 && result.cell.speciesCount < RX_MAX_CELL_SPECIES) {
        result.cell.species[result.cell.speciesCount] = species;
        result.cell.amounts[result.cell.speciesCount] = shifted.whole;
        result.cell.speciesCount += 1;
    } else if (shifted.whole != 0) {
        result.overflowCount += 1;
    }

    if (shifted.fraction != 0 && result.fraction.speciesCount < RX_MAX_CELL_SPECIES) {
        result.fraction.species[result.fraction.speciesCount] = species;
        result.fraction.amounts[result.fraction.speciesCount] = shifted.fraction;
        result.fraction.speciesCount += 1;
    } else if (shifted.fraction != 0) {
        result.overflowCount += 1;
    }

    return result;
}

// 落ちた下位 3bit を成分ごとに覚える(覚えきれなければ溢れに数える)
FX_FN MrCoarsened MrRecordLost(MrCoarsened result, uint32_t species, uint32_t lostBits) {
    if (lostBits == 0)
        return result;

    if (result.lostSpeciesCount >= MR_MAX_LOST_SPECIES) {
        result.overflowCount += 1;
        return result;
    }

    result.lostSpecies[result.lostSpeciesCount] = species;
    result.lostBits[result.lostSpeciesCount] = lostBits;
    result.lostSpeciesCount += 1;

    return result;
}

// 子 2³ を親のセル 1 つにまとめる。物質量とエネルギーは「整数部 + 端数」の合計の 1/8(単位が 8 倍になるので)。
// 成分は子の和集合(ID の昇順)。並び順に依存しない(物質ごとに全部の子を同じ順で足す)
FX_FN MrCoarsened MrCoarsenCell(MrChildren children) {
    MrCoarsened result;
    result.lostCount = 0;
    result.overflowCount = 0;
    result.lostSpeciesCount = 0;
    // NOLINTNEXTLINE(modernize-loop-convert) HLSL には範囲 for が無い
    for (uint32_t i = 0; i < MR_MAX_LOST_SPECIES; ++i) {
        result.lostSpecies[i] = 0;
        result.lostBits[i] = 0;
    }

    // --- エネルギー(符号つき)---
    MrWide energy = MrMakeWide();
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        const int64_t whole = children.cells[j].energy;
        energy = MrWideAdd(energy, (uint64_t)whole, whole < 0, children.fractions[j].energy);
    }

    const MrWideShifted energyShifted = MrWideShiftLevel(energy);
    FX_ASSERT(((int64_t)energy.top >> MR_LEVEL_SHIFT) == ((int64_t)energyShifted.whole < 0 ? -1 : 0));
    result.cell = RxMakeEmptyCell((int64_t)energyShifted.whole);
    result.fraction = MrMakeEmptyFraction();
    result.fraction.energy = energyShifted.fraction;
    result.lostCount += energyShifted.lostBits != 0 ? 1u : 0u;
    result.energyLostBits = energyShifted.lostBits;

    // --- 成分(子の和集合を ID の昇順に)---
    uint32_t species = 0;
    for (uint32_t step = 0; step < 2 * RX_MAX_CELL_SPECIES * MR_CHILDREN_PER_CELL; ++step) {
        uint32_t next = MR_NO_SPECIES;
        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
            const uint32_t candidate = MrNextSpecies(children.cells[j], children.fractions[j], species);
            next = candidate < next ? candidate : next;
        }

        if (next == MR_NO_SPECIES)
            break;

        species = next;
        MrWide amount = MrMakeWide();
        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
            amount = MrWideAdd(amount, MrAmountOf(children.cells[j], species), false,
                               MrFractionOf(children.fractions[j], species));
        }

        const MrWideShifted shifted = MrWideShiftLevel(amount);
        FX_ASSERT((amount.top >> MR_LEVEL_SHIFT) == 0);
        result.lostCount += shifted.lostBits != 0 ? 1u : 0u;
        result = MrRecordLost(result, species, shifted.lostBits);

        result = MrAppendCoarsened(result, species, shifted);
    }

    return result;
}

// --- 影を親に引き戻す --------------------------------------------------------------------------

// total を重み weights の比で 8 つに配る(切り捨て。余りは重みが 0 でない子に番号順に 1 ずつ)。重みが全部 0 なら等分
struct MrShares {
    uint64_t values[8];
};

FX_FN MrShares MrDistribute(uint64_t total, MrShares weights) {
    uint64_t weightSum = 0;
    // NOLINTNEXTLINE(modernize-loop-convert) HLSL には範囲 for が無い
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        FX_ASSERT(weightSum + weights.values[j] >= weightSum);
        weightSum += weights.values[j];
    }

    MrShares shares;
    uint64_t given = 0;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        if (weightSum == 0)
            shares.values[j] = (total / MR_CHILDREN_PER_CELL) + (j < total % MR_CHILDREN_PER_CELL ? 1u : 0u);
        else
            shares.values[j] = FxDivU128By64(FxMulU64Full(total, weights.values[j]), weightSum).quotient;

        given += shares.values[j];
    }

    // 切り捨てで足りない分(重みが 0 でない子の数より少ない)を番号順に 1 ずつ
    uint64_t rest = total - given;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        if (rest == 0)
            break;

        if (weights.values[j] == 0)
            continue;

        shares.values[j] += 1;
        rest -= 1;
    }

    FX_ASSERT(rest == 0);

    return shares;
}

// mJ に切り上げる(µJ → mJ。エネルギーの下限 = 化学のエネルギーを下回らないように)
FX_FN int64_t MrCeilMillijoules(int64_t microjoules) {
    const int64_t quotient = FxDivS64(microjoules, (int64_t)RX_MICROJOULES_PER_MILLIJOULE);
    const bool hasRest = quotient * (int64_t)RX_MICROJOULES_PER_MILLIJOULE != microjoules;

    return quotient + (microjoules > 0 && hasRest ? 1 : 0);
}

// 影の子 2³ の合計を「親 × 8」(子の単位)に合わせる(17 §3 B)。
//   物質: 親にある物質だけを、子の今の量の比で配る(親に無い物質は消える)。
//   エネルギー: 子ごとに「化学のエネルギー以上」(熱が負にならない)を確保し、残りを子の今の熱の比で配る。
// 端数は扱わない(影は世界に返さないので、整数部だけを親に従わせる)
template <typename Table>
FX_FN MrShadowFamily MrPullBackShadow(Table table, RxCell parent, MrShadowFamily family) {
    MrShadowFamily result;
    result.clamped = 0;
    // NOLINTNEXTLINE(modernize-loop-convert) HLSL には範囲 for が無い
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
        result.cells[j] = RxMakeEmptyCell(0);

    // --- 物質 ---
    for (uint32_t p = 0; p < parent.speciesCount; ++p) {
        const uint32_t species = parent.species[p];
        FX_ASSERT((parent.amounts[p] >> (64 - MR_LEVEL_SHIFT)) == 0);
        MrShares weights;
        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
            weights.values[j] = MrAmountOf(family.cells[j], species);

        const MrShares shares = MrDistribute(parent.amounts[p] << MR_LEVEL_SHIFT, weights);
        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
            result.cells[j] = RxAddSpecies(result.cells[j], species, shares.values[j]);
    }

    // --- エネルギー: 下限(化学のエネルギーの mJ への切り上げ。いったん子のエネルギーに置く)と、残りの配り方の重み(今の熱)---
    int64_t floorSum = 0;
    MrShares heatWeights;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        result.cells[j].energy = MrCeilMillijoules(RxChemicalEnergy(table, result.cells[j]));
        floorSum += result.cells[j].energy;

        const int64_t heat = RxMulS64Checked(family.cells[j].energy, (int64_t)RX_MICROJOULES_PER_MILLIJOULE) -
                             RxChemicalEnergy(table, family.cells[j]);
        heatWeights.values[j] = heat > 0 ? (uint64_t)heat : 0u;
    }

    const int64_t target = RxMulS64Checked(parent.energy, (int64_t)MR_CHILDREN_PER_CELL);
    int64_t spare = target - floorSum;
    if (spare < 0) {
        result.clamped = 1;
        spare = 0;
    }

    const MrShares spareShares = MrDistribute((uint64_t)spare, heatWeights);
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
        result.cells[j].energy += (int64_t)spareShares.values[j];

    return result;
}

// --- 刻み --------------------------------------------------------------------------------------

// 刻み tick の印(MrBlock::busyTick・wakeTick。64bit なので一周しない。0 = 刻み 0 より前 = 初めの状態。T-0115)
FX_FN uint64_t MrChangeMark(uint64_t tick) {
    return tick + 1;
}

// 待ちの丸め(ADR-0018。T-0115)の葉のセル(または影のセル)の 1 刻み。刻みと tc は印で渡す: tc = busyTick(ブロックが最後に
// 変わった刻みの印。刻みの初めに読んだ値。つつかれたままの MR_BUSY_POKED は呼ぶ側が先にこの刻みの印にしておく)。返す wakeTick も印。
// この刻みにつつかれた(busyTick = この刻みの印)ブロックは「刻みの直前に変わった」として評価する(この刻みにも確率 f で進む)。
// 次の刻みからは tc = この刻みの印で引き直すので、呼ぶ側はつつかれたブロックを次の刻みにまた評価する(記憶が無いので偏らない)
template <typename Table>
FX_FN RxWaitStep MrStepCellWait(Table table, RxCell cell, uint64_t worldSeed, uint64_t tick, MrBlock block,
                                uint32_t index) {
    const uint64_t cellId = MrCellId(block.level, block.originX + (int64_t)MrCellX(index),
                                     block.originY + (int64_t)MrCellY(index), block.originZ + (int64_t)MrCellZ(index));
    const uint64_t mark = MrChangeMark(tick);
    const uint64_t changedMark = block.busyTick < mark ? block.busyTick : mark - 1;

    return RxStepCellWait(table, cell, worldSeed, mark, changedMark, cellId);
}

// このセルを刻むか: 使っているブロックで、本物の子に覆われていない(影のブロックは中間のセルも刻む。世界の写しは刻まない)
FX_FN bool MrIsSteppedCell(MrBlock block, uint32_t index) {
    if (block.kind == MR_BLOCK_UNUSED || block.kind == MR_BLOCK_MIRROR)
        return false;

    if (block.kind == MR_BLOCK_SHADOW)
        return true;

    return block.children[MrOctantOfCell(index)] == MR_NO_BLOCK;
}

// --- 一様なブロック(T-0102)----------------------------------------------------------------------

// 2 つのセルがビット単位で同じか(padding は見ない)
FX_FN bool MrSameCell(RxCell a, RxCell b) {
    return RxSameCell(a, b);
}

FX_FN bool MrIsUniform(MrBlock block) {
    return block.page >= MR_PAGE_WANTED;
}

// 本物の子に覆われたセルか(覆われたセルは空)
FX_FN bool MrIsCoveredCell(MrBlock block, uint32_t index) {
    return block.kind == MR_BLOCK_REAL && block.children[MrOctantOfCell(index)] != MR_NO_BLOCK;
}

// 一様なブロックのセル index(覆われていれば空、でなければ値 value)。頁に広げる時もこれで埋める
FX_FN RxCell MrUniformCell(MrBlock block, RxCell value, uint32_t index) {
    if (MrIsCoveredCell(block, index))
        return RxMakeEmptyCell(0);

    return value;
}

// 刻むセルが 1 つでもあるか(MrIsSteppedCell を八分の一ごとに)
FX_FN bool MrHasSteppedCell(MrBlock block) {
    bool any = false;
    for (uint32_t octant = 0; octant < 8; ++octant)
        any = any ||
              MrIsSteppedCell(block, MrCellIndex((octant & 1u) * MR_OCTANT_EDGE, ((octant >> 1) & 1u) * MR_OCTANT_EDGE,
                                                 ((octant >> 2) & 1u) * MR_OCTANT_EDGE));

    return any;
}

// 一様なブロック(値 value)を待ちの丸めで評価した結果(T-0115)
struct MrUniformWait {
    uint32_t changed;   // 刻むセルが 1 つでも変わる(頁に広げて刻む)
    uint64_t wakeTick;  // 変わらなければ、次に評価の要る刻みの印(刻むセルの最小。無ければ RX_WAIT_NEVER)
};

// 一様なブロックを刻み tick に待ちの丸めで評価する。待ちはセルの ID ごとに違うので、値が同じでも刻むセルを 1 つずつ見る
// (評価するのは起こす刻み ≤ tick の時だけ。普通は変わる最初のセルで抜ける)
template <typename Table>
FX_FN MrUniformWait MrUniformWaitOf(Table table, RxCell value, uint64_t worldSeed, uint64_t tick, MrBlock block) {
    MrUniformWait result;
    result.changed = 0;
    result.wakeTick = RX_WAIT_NEVER;
    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
        if (!MrIsSteppedCell(block, index))
            continue;

        const RxWaitStep step = MrStepCellWait(table, value, worldSeed, tick, block, index);
        if (!MrSameCell(step.cell, value)) {
            result.changed = 1;
            return result;
        }

        result.wakeTick = step.wakeTick < result.wakeTick ? step.wakeTick : result.wakeTick;
    }

    return result;
}

// --- 頁を畳む(T-0103)-----------------------------------------------------------------------------
// 頁を持つブロックが一様(覆われていないセルが全部同じ・覆われたセルは空・端数なし)なら、値 1 つに戻して頁を返す。
// 調べる時は multires_activity.hlsli の MrWantsFoldCheck(ちょうど静かになった刻みに 1 回)。

// 畳む時の一様の値を読むセル: 覆われていない最初の八分の一の最初のセル(全部覆われていれば MR_BLOCK_CELLS = 値は空のセル)
FX_FN uint32_t MrFoldValueCell(MrBlock block) {
    for (uint32_t octant = 0; octant < 8; ++octant) {
        if (block.children[octant] == MR_NO_BLOCK)
            return MrCellIndex((octant & 1u) * MR_OCTANT_EDGE, ((octant >> 1) & 1u) * MR_OCTANT_EDGE,
                               ((octant >> 2) & 1u) * MR_OCTANT_EDGE);
    }

    return MR_BLOCK_CELLS;
}

// 頁のセル index(中身 cell)が、一様の値 value で畳んだ時と同じか(覆われていれば空、でなければ value とビット単位で同じ)
FX_FN bool MrFoldsCell(MrBlock block, RxCell value, uint32_t index, RxCell cell) {
    return MrSameCell(cell, MrUniformCell(block, value, index));
}

// --- ほぼ同じ頁を畳む(T-0104。D-428)--------------------------------------------------------------
// 覆われていないセルの差が許容差(ゲーム内の計器で測れない差)の中なら、平均の値 1 つに戻す。合計はビット一致: 平均の切り捨てで
// 余った単位(物質ごと・エネルギー。セルの数未満)は世界の帳簿へ(multires_activity.cpp の AddFoldRemainder)。許容差の値は未定
// (docs/plan/QUESTIONS.md Q4)。完全に同じ(MrIsExactFold)なら T-0103 のビット単位の判定(MrFoldsCell)のまま。
// 集計(MrFoldStats)は最小・最大・和と、物質 ID の昇順に並べた成分だけなので、セルを足す順によらない。

FX_CONST uint32_t MR_FOLD_EXACT_SHIFT = 64;

struct MrFoldTolerance {
    uint32_t temperatureMk;  // セルの温度の幅(最大 − 最小)の上限
    uint32_t
        amountShift;  // 物質ごとの量の幅の上限 = 成分の合計が最大のセルの合計 >> amountShift(MR_FOLD_EXACT_SHIFT 以上は 0)
};

FX_FN MrFoldTolerance MrExactFoldTolerance() {
    MrFoldTolerance tolerance;
    tolerance.temperatureMk = 0;
    tolerance.amountShift = MR_FOLD_EXACT_SHIFT;

    return tolerance;
}

FX_FN bool MrIsExactFold(MrFoldTolerance tolerance) {
    return tolerance.temperatureMk == 0 && tolerance.amountShift >= MR_FOLD_EXACT_SHIFT;
}

// GPU のルート定数 1 語に詰めた許容差(T-0112。ルート署名の語が残り少ないため): 下位 25bit = 温度の幅 mK(約 33 K まで)、
// 上位 7bit = MR_FOLD_EXACT_SHIFT − amountShift(0 なら量はビット単位)。0 は完全に同じ(MrExactFoldTolerance)
FX_CONST uint32_t MR_FOLD_TEMPERATURE_BITS = 25;
FX_CONST uint32_t MR_FOLD_TEMPERATURE_MASK = (1u << MR_FOLD_TEMPERATURE_BITS) - 1u;

FX_FN uint32_t MrPackFoldTolerance(MrFoldTolerance tolerance) {
    FX_ASSERT(tolerance.temperatureMk <= MR_FOLD_TEMPERATURE_MASK);
    const uint32_t shift = tolerance.amountShift < MR_FOLD_EXACT_SHIFT ? tolerance.amountShift : MR_FOLD_EXACT_SHIFT;

    return ((MR_FOLD_EXACT_SHIFT - shift) << MR_FOLD_TEMPERATURE_BITS) | tolerance.temperatureMk;
}

FX_FN MrFoldTolerance MrUnpackFoldTolerance(uint32_t packed) {
    MrFoldTolerance tolerance;
    tolerance.temperatureMk = packed & MR_FOLD_TEMPERATURE_MASK;
    tolerance.amountShift = MR_FOLD_EXACT_SHIFT - (packed >> MR_FOLD_TEMPERATURE_BITS);

    return tolerance;
}

struct MrFoldStats {
    // --- セル ---
    uint32_t cellCount;
    uint32_t tooManySpecies;  // 合わせた成分が RX_MAX_CELL_SPECIES を超えた(畳まない)
    int32_t lowTemperature;   // mK
    int32_t highTemperature;
    uint64_t highTotal;  // セルの成分の物質量の合計の最大
    MrWide energy;       // エネルギーの和(上の 2 語。下の語は使わない)

    // --- 成分(物質 ID の昇順)---
    uint32_t speciesCount;
    uint32_t species[RX_MAX_CELL_SPECIES];
    uint32_t present[RX_MAX_CELL_SPECIES];  // その物質を持つセルの数
    uint64_t lowAmount[RX_MAX_CELL_SPECIES];
    uint64_t highAmount[RX_MAX_CELL_SPECIES];
    uint64_t sumHigh[RX_MAX_CELL_SPECIES];
    uint64_t sumLow[RX_MAX_CELL_SPECIES];
};

FX_FN MrFoldStats MrMakeFoldStats() {
    MrFoldStats stats;
    stats.cellCount = 0;
    stats.tooManySpecies = 0;
    stats.lowTemperature = 2147483647;
    stats.highTemperature = -2147483647 - 1;
    stats.highTotal = 0;
    stats.energy = MrMakeWide();
    stats.speciesCount = 0;
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        stats.species[i] = 0;
        stats.present[i] = 0;
        stats.lowAmount[i] = 0;
        stats.highAmount[i] = 0;
        stats.sumHigh[i] = 0;
        stats.sumLow[i] = 0;
    }

    return stats;
}

// 成分の位置(物質 ID の昇順に並べた時に入る位置)
FX_FN uint32_t MrFoldSpeciesPosition(MrFoldStats stats, uint32_t speciesId) {
    uint32_t position = 0;
    while (position < stats.speciesCount && stats.species[position] < speciesId)
        position += 1;

    return position;
}

// 成分が無ければ ID の順の位置に空けて差し込む(一杯なら tooManySpecies)
FX_FN MrFoldStats MrFoldInsertSpecies(MrFoldStats stats, uint32_t speciesId) {
    const uint32_t position = MrFoldSpeciesPosition(stats, speciesId);
    if (position < stats.speciesCount && stats.species[position] == speciesId)
        return stats;

    if (stats.speciesCount >= RX_MAX_CELL_SPECIES) {
        stats.tooManySpecies = 1;
        return stats;
    }

    for (uint32_t i = stats.speciesCount; i > position; --i) {
        stats.species[i] = stats.species[i - 1];
        stats.present[i] = stats.present[i - 1];
        stats.lowAmount[i] = stats.lowAmount[i - 1];
        stats.highAmount[i] = stats.highAmount[i - 1];
        stats.sumHigh[i] = stats.sumHigh[i - 1];
        stats.sumLow[i] = stats.sumLow[i - 1];
    }

    stats.species[position] = speciesId;
    stats.present[position] = 0;
    stats.lowAmount[position] = 0;
    stats.highAmount[position] = 0;
    stats.sumHigh[position] = 0;
    stats.sumLow[position] = 0;
    stats.speciesCount += 1;

    return stats;
}

// 覆われていないセル 1 つ(温度は RxComputeThermal の値)を足す
FX_FN MrFoldStats MrAddFoldCell(MrFoldStats stats, RxCell cell, int32_t temperature) {
    stats.cellCount += 1;
    stats.lowTemperature = temperature < stats.lowTemperature ? temperature : stats.lowTemperature;
    stats.highTemperature = temperature > stats.highTemperature ? temperature : stats.highTemperature;
    stats.energy = MrWideAdd(stats.energy, (uint64_t)cell.energy, cell.energy < 0, 0);

    uint64_t total = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const uint64_t amount = cell.amounts[i];
        total = total + amount < total ? FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu) : total + amount;
        stats = MrFoldInsertSpecies(stats, cell.species[i]);
        const uint32_t position = MrFoldSpeciesPosition(stats, cell.species[i]);
        if (position >= stats.speciesCount || stats.species[position] != cell.species[i])
            continue;

        const bool first = stats.present[position] == 0;
        stats.present[position] += 1;
        stats.lowAmount[position] = first || amount < stats.lowAmount[position] ? amount : stats.lowAmount[position];
        stats.highAmount[position] = amount > stats.highAmount[position] ? amount : stats.highAmount[position];
        stats.sumLow[position] += amount;
        stats.sumHigh[position] += stats.sumLow[position] < amount ? 1u : 0u;
    }

    stats.highTotal = total > stats.highTotal ? total : stats.highTotal;

    return stats;
}

// 集めたセルの差が許容差の中か(成分が無いセルの量は 0)
FX_FN bool MrFoldStatsWithin(MrFoldStats stats, MrFoldTolerance tolerance) {
    if (stats.cellCount == 0 || stats.tooManySpecies != 0)
        return false;

    if ((uint32_t)(stats.highTemperature - stats.lowTemperature) > tolerance.temperatureMk)
        return false;

    const uint64_t amountLimit = tolerance.amountShift >= MR_FOLD_EXACT_SHIFT
                                     ? 0
                                     : stats.highTotal >> tolerance.amountShift;
    bool within = true;
    for (uint32_t i = 0; i < stats.speciesCount; ++i) {
        const uint64_t low = stats.present[i] < stats.cellCount ? 0 : stats.lowAmount[i];
        within = within && stats.highAmount[i] - low <= amountLimit;
    }

    return within;
}

// 平均の値と、切り捨てで余った単位(セルの数未満。帳簿へ。並びは stats.species と同じ)
struct MrFoldValue {
    RxCell cell;
    uint64_t energyRemainder;
    uint64_t amountRemainders[RX_MAX_CELL_SPECIES];
};

// エネルギーの和(符号つき 128bit)を床の割り算する。余りは 0 以上
FX_FN FxDivResult MrFloorDivideEnergy(MrWide energy, uint64_t divisor) {
    if ((int64_t)energy.top >= 0) {
        FxU128 numerator = {energy.top, energy.hi};
        return FxDivU128By64(numerator, divisor);
    }

    // --- 負: 大きさ M = −和 を切り上げで割り、商を負にする(余り = 商 × n − M = n − 1 − (M + n − 1) の余り)---
    const uint64_t low = ~energy.hi + 1u;
    const uint64_t high = ~energy.top + (energy.hi == 0 ? 1u : 0u);
    const uint64_t raisedLow = low + (divisor - 1u);
    FxU128 raised = {high + (raisedLow < low ? 1u : 0u), raisedLow};
    const FxDivResult magnitude = FxDivU128By64(raised, divisor);

    FxDivResult result;
    result.quotient = (uint64_t)(-(int64_t)magnitude.quotient);
    result.remainder = divisor - 1u - magnitude.remainder;

    return result;
}

FX_FN MrFoldValue MrFoldStatsValue(MrFoldStats stats) {
    MrFoldValue value;
    const uint64_t cells = stats.cellCount;
    const FxDivResult energy = MrFloorDivideEnergy(stats.energy, cells);
    value.cell = RxMakeEmptyCell((int64_t)energy.quotient);
    value.energyRemainder = energy.remainder;
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        value.amountRemainders[i] = 0;
        if (i >= stats.speciesCount)
            continue;

        FxU128 sum = {stats.sumHigh[i], stats.sumLow[i]};
        const FxDivResult amount = FxDivU128By64(sum, cells);
        value.cell = RxAddSpecies(value.cell, stats.species[i], amount.quotient);
        value.amountRemainders[i] = amount.remainder;
    }

    return value;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_HLSLI
