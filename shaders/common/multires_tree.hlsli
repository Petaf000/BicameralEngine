// multires_tree.hlsli — 多重解像度の木の管理: 索引・空きのスタック・要求・世界の帳簿の約束と小さな関数(17 §5「木の管理」。T-0018。ADR-0016)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/multires_nest.cpp の ProcessRequests)と
// GPU(shaders/sim/multires_tree.hlsl の段・multires_graph.hlsl の鎖)が同じ関数を呼ぶ。
//
// 要求の処理の段(1 刻みに 1 回): 解決 → 確定 → 割り当て → 適用 → 解放 → (印があれば)索引の作り直し。
// 枠の番号は要求の一覧の順の累積和で決まる(ADR-0016)。索引の表の中の並びは決定的でなくてよい(見出しから作り直せる)。
#ifndef BICAMERAL_MULTIRES_TREE_HLSLI
#define BICAMERAL_MULTIRES_TREE_HLSLI

#include "multires.hlsli"

MR_NAMESPACE_BEGIN

// --- 大きさと印 --------------------------------------------------------------------------------
FX_CONST uint32_t MR_MAX_REQUESTS = 512;     // 1 刻みの要求の一覧の大きさ(割り当ての段は 1 グループ)
FX_CONST uint32_t MR_MAX_CHAIN_LEVELS = 24;  // 1 つの細かくする要求が 1 刻みで作る段の最大(Work Graph の再帰の深さ)
FX_CONST uint32_t MR_MAX_RELEASES = (2 * MR_MAX_CHAIN_LEVELS) + 2;  // 1 つの要求が返す端数の枠の最大
FX_CONST uint32_t MR_NO_CLAIM = 0xFFFFFFFFu;
FX_CONST uint32_t MR_INDEX_EMPTY = 0xFFFFFFFFu;
FX_CONST uint32_t MR_INDEX_TOMBSTONE = 0xFFFFFFFEu;
FX_CONST uint32_t MR_MAX_LEVEL_SPAN = 62;  // 要求のレベルと根のレベルの差の最大(64bit の座標をずらすため)

// 世界の帳簿: [レベル − MR_LEDGER_LEVEL_MIN][列]。列 0 = エネルギー、1 + 物質 ID。値は子のレベルの単位 × 2^-64 の数
FX_CONST int32_t MR_LEDGER_LEVEL_MIN = -16;
FX_CONST uint32_t MR_LEDGER_LEVELS = 80;

// 要求の種類
FX_CONST uint32_t MR_REQUEST_REFINE = 1;   // 点を含むレベル L の本物のブロックを作る(途中の段も)
FX_CONST uint32_t MR_REQUEST_COARSEN = 2;  // 点を含むレベル L の本物のブロックを親へ戻す

// 要求の状態
FX_CONST uint32_t MR_STATUS_PENDING = 0;
FX_CONST uint32_t MR_STATUS_GRANTED = 1;
FX_CONST uint32_t MR_STATUS_ALREADY = 2;
FX_CONST uint32_t MR_STATUS_CONFLICT = 3;
FX_CONST uint32_t MR_STATUS_NO_SPACE = 4;
FX_CONST uint32_t MR_STATUS_INVALID = 5;

// --- 構造体 ------------------------------------------------------------------------------------

// 要求(40 バイト)。点はレベル level のセルの単位の世界の座標
struct MrRequest {
    int64_t x;
    int64_t y;
    int64_t z;
    int32_t level;
    uint32_t op;  // MR_REQUEST_*
    uint32_t padding[2];
};

// 要求の処理の途中の値(1 刻みの中だけ。状態のハッシュに入らない)
struct MrRequestState {
    uint32_t status;     // MR_STATUS_*
    uint32_t target;     // 細かくする = 鎖を作り始める親 D、粗くする = 戻すブロック B
    uint32_t claimSlot;  // 取り合いの枠(無ければ MR_NO_CLAIM)
    uint32_t levels;     // 作る段の数

    // --- 割り当て(空きのスタックの上から blockBase − 1 − i 番目を i 番目に使う。頁も同じ。T-0102)---
    uint32_t blockNeed;
    uint32_t fractionNeed;
    uint32_t pageNeed;
    uint32_t blockBase;
    uint32_t fractionBase;
    uint32_t pageBase;

    // --- 返す枠 ---
    uint32_t releaseBlock;  // 返すブロックの枠(無ければ MR_NO_BLOCK)
    uint32_t releasePage;   // 返す頁(無ければ MR_NO_PAGE)
    uint32_t releaseCount;  // 返す端数の枠の数
    uint32_t releases[MR_MAX_RELEASES];
};

// --- 関数 --------------------------------------------------------------------------------------

FX_FN MrRequest MrMakeRequest(uint32_t op, int32_t level, int64_t x, int64_t y, int64_t z) {
    MrRequest request;
    request.x = x;
    request.y = y;
    request.z = z;
    request.level = level;
    request.op = op;
    request.padding[0] = 0;
    request.padding[1] = 0;

    return request;
}

FX_FN MrRequestState MrMakeRequestState() {
    MrRequestState state;
    state.status = MR_STATUS_PENDING;
    state.target = MR_NO_BLOCK;
    state.claimSlot = MR_NO_CLAIM;
    state.levels = 0;
    state.blockNeed = 0;
    state.fractionNeed = 0;
    state.pageNeed = 0;
    state.blockBase = 0;
    state.fractionBase = 0;
    state.pageBase = 0;
    state.releaseBlock = MR_NO_BLOCK;
    state.releasePage = MR_NO_PAGE;
    state.releaseCount = 0;
    // NOLINTNEXTLINE(modernize-loop-convert) HLSL には範囲 for が無い
    for (uint32_t i = 0; i < MR_MAX_RELEASES; ++i)
        state.releases[i] = 0;

    return state;
}

// 座標を含むブロックの原点(8 の倍数に切り捨て。負の座標も床へ)
FX_FN int64_t MrBlockOriginOf(int64_t coordinate) {
    return coordinate & ~(int64_t)(MR_BLOCK_EDGE - 1);
}

// 索引の最初の番地(capacity は 2 の冪)
FX_FN uint32_t MrIndexHome(int32_t level, int64_t originX, int64_t originY, int64_t originZ, uint32_t capacity) {
    uint64_t hash = FxMix64((uint64_t)(int64_t)level + FX_GOLDEN_GAMMA);
    hash = FxHashCombine(hash, (uint64_t)originX);
    hash = FxHashCombine(hash, (uint64_t)originY);
    hash = FxHashCombine(hash, (uint64_t)originZ);

    return (uint32_t)hash & (capacity - 1);
}

// 索引の項目の見出しがキーと合うか(本物のブロックだけを入れる)
FX_FN bool MrBlockHasKey(MrBlock block, int32_t level, int64_t originX, int64_t originY, int64_t originZ) {
    return block.kind == MR_BLOCK_REAL && block.level == level && block.originX == originX &&
           block.originY == originY && block.originZ == originZ;
}

// 点(pointLevel の座標 1 軸)を、それより粗い level の座標にする(床へ)
FX_FN int64_t MrCoarserCoordinate(int64_t coordinate, int32_t pointLevel, int32_t level) {
    FX_ASSERT(pointLevel >= level && pointLevel - level <= (int32_t)MR_MAX_LEVEL_SPAN);

    return coordinate >> (uint32_t)(pointLevel - level);
}

// 点を含む八分の一(3 軸)
FX_FN uint32_t MrOctantOfPoint(MrBlock block, int64_t x, int64_t y, int64_t z, int32_t pointLevel) {
    return MrOctantBitOfPoint(block.originX, block.level, x, pointLevel) |
           (MrOctantBitOfPoint(block.originY, block.level, y, pointLevel) << 1) |
           (MrOctantBitOfPoint(block.originZ, block.level, z, pointLevel) << 2);
}

// 細かくした子の見出し(点を含む八分の一)。page は子の頁(一様な親の本物の子は MR_NO_PAGE。T-0102)
FX_FN MrBlock MrMakeChildBlock(MrBlock parent, uint32_t parentSlot, uint32_t octant, uint32_t kind,
                               uint32_t fractionSlot, uint32_t page) {
    MrBlock child = MrMakeUnusedBlock();
    child.originX = MrChildOrigin(parent.originX, octant & 1u);
    child.originY = MrChildOrigin(parent.originY, (octant >> 1) & 1u);
    child.originZ = MrChildOrigin(parent.originZ, (octant >> 2) & 1u);
    child.level = parent.level + 1;
    child.kind = kind;
    child.parent = parentSlot;
    child.parentOctant = octant;
    child.fraction = fractionSlot;
    child.page = page;

    return child;
}

// --- 一様なブロックと要求(T-0102)---

// 一様な子(値 childValue、端数なし)を一様な親(値 parentValue)へ粗くしても、親が一様のままか。
// 子 2³ が同じ値なら粗くした値は同じ値に戻るはずだが、表現(成分の並び)まで同じことを確かめる(違えば親を頁に広げる)
FX_FN bool MrCoarsenKeepsUniform(RxCell childValue, RxCell parentValue) {
    MrChildren children;
    for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
        children.cells[j] = childValue;
        children.fractions[j] = MrMakeEmptyFraction();
    }

    const MrCoarsened result = MrCoarsenCell(children);

    return result.lostCount == 0 && result.overflowCount == 0 && MrFractionIsZero(result.fraction) &&
           MrSameCell(result.cell, parentValue);
}

// 粗くする要求で親に要る頁の数(親が一様で、子が一様で同じ値に戻る時だけ 0)
FX_FN uint32_t MrCoarsenPageNeed(MrBlock child, RxCell childValue, MrBlock parent, RxCell parentValue) {
    if (!MrIsUniform(parent))
        return 0;

    if (MrIsUniform(child) && child.fraction == MR_NO_FRACTION && MrCoarsenKeepsUniform(childValue, parentValue))
        return 0;

    return 1;
}

FX_FN bool MrHasRealChild(MrBlock block) {
    bool any = false;
    // NOLINTNEXTLINE(modernize-loop-convert) HLSL には範囲 for が無い
    for (uint32_t i = 0; i < 8; ++i)
        any = any || block.children[i] != MR_NO_BLOCK;

    return any;
}

// 帳簿の番地(レベルが帳簿の外なら MR_NO_BLOCK)
FX_FN uint32_t MrLedgerAddress(int32_t level, uint32_t column, uint32_t columns) {
    if (level < MR_LEDGER_LEVEL_MIN || level >= MR_LEDGER_LEVEL_MIN + (int32_t)MR_LEDGER_LEVELS || column >= columns)
        return MR_NO_BLOCK;

    return ((uint32_t)(level - MR_LEDGER_LEVEL_MIN) * columns) + column;
}

// 要求の結果を数える欄の番号
FX_FN uint32_t MrStatusCounter(uint32_t status) {
    if (status == MR_STATUS_GRANTED)
        return MR_COUNTER_GRANTED;

    if (status == MR_STATUS_ALREADY)
        return MR_COUNTER_ALREADY;

    if (status == MR_STATUS_CONFLICT)
        return MR_COUNTER_CONFLICT;

    if (status == MR_STATUS_NO_SPACE)
        return MR_COUNTER_NO_SPACE;

    return MR_COUNTER_INVALID;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_TREE_HLSLI
