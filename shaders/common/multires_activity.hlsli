// multires_activity.hlsli — 多重解像度の木の上の活性: どのブロックを刻むか・面の隣を起こす(05 §4・06 §2・17 §5「活性」。T-0100)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/multires_activity.cpp の StepActive)と
// GPU(shaders/sim/multires_activity_graph.hlsl の ActivitySeedNode・WakeFaceNode・ActivityStepNode、
// 静かな葉を粗くする要求は multires_tree.hlsl の TreeQuiet。T-0101)が同じ関数を呼ぶ。
//
// 活性の規則(仮の世界の T-0005・T-0089 を木に広げたもの): 刻み t で刻む世界のブロック = 種とその面の隣。
//   種 = 刻み t − 1 に進める反応の規則があったブロック(D-424: 無ければ刻みが変わっても変わらない)+ 刻み t に木の変更でつつかれたブロック。
//   待ちの丸め(ADR-0018。CPU は T-0115、GPU は T-0121)では、種 = 見出しの起こす刻み(wakeTick)が来たブロック + つつかれたブロック
//   (変わったブロックは次の刻みに起こす)。起こす刻みの前は、刻んでも何も変わらない。
//   面の隣 = 刻むセルどうしが面で接するブロック。レベルが違ってよい: 粗い側(親の覆われていないセル)は索引を上へ引いて、
//   細かい側(子孫が覆う面)は面に接する八分の一を子へたどって(再帰)見つける。
// 木は「ブロックの覆われていない八分の一(4³ セル)」で世界を隙間なく分けるので、隣は「八分の一の箱 × 6 面」ごとに求まる。
//
// Tree の約束(テンプレート): MrBlock Block(uint32_t slot) と uint32_t Lookup(level, originX, originY, originZ)(索引。無ければ MR_NO_BLOCK)。
#ifndef BICAMERAL_MULTIRES_ACTIVITY_HLSLI
#define BICAMERAL_MULTIRES_ACTIVITY_HLSLI

#include "multires_tree.hlsli"

MR_NAMESPACE_BEGIN

// --- 大きさ ------------------------------------------------------------------------------------
FX_CONST uint32_t MR_FACES = 6;         // 面(軸 = face / 2、正の向き = face % 2)
FX_CONST uint32_t MR_FACE_OCTANTS = 4;  // 1 つの面に接する八分の一の数
FX_CONST uint32_t MR_WAKE_CHECKS = 48;  // 1 つのブロックの (八分の一, 面) の組
FX_CONST uint32_t
    MR_MAX_WAKE_DEPTH = 28;  // 面を細かい側へたどる再帰の上限(Work Graph の深さ 32 = 種 1 + 面 29 + 刻む 1 + 余り 1)

// 活性の一覧(GPU の入力。バイトの位置。先頭は D3D12_NODE_GPU_INPUT そのもの。gpu_multires.cpp の static_assert)。
// レコードは枠の番号(uint32)。レコード 0 は何もしない(MR_NO_BLOCK。0 件にしないため)
FX_CONST uint32_t MR_ACTIVITY_NUM_RECORDS = 4;  // D3D12_NODE_GPU_INPUT::NumRecords(書けた数 + 1)
FX_CONST uint32_t MR_ACTIVITY_RESERVED = 24;    // 次に書く番号(最初 1)
FX_CONST uint32_t MR_ACTIVITY_DROPPED = 28;     // 一覧が一杯で落とした数(0 でなければ活性の結果は信用できない)
FX_CONST uint32_t MR_ACTIVITY_RECORDS = 32;

// 一覧のレコードの数(空のレコード + 世界の枠 × 2 + 1 刻みにつつかれる数の上限)。待ちの丸め(T-0124)では刻みの間に入った種と
// 起こす段(WakeDue)の種で同じ枠が 2 回入りうる(重ねて刻むことはない: ScheduleBlock が 1 刻みに 1 回にする)
FX_FN uint32_t MrActivityCapacity(uint32_t worldBlocks) {
    return 1 + (worldBlocks * 2) + (MR_MAX_REQUESTS * (MR_MAX_CHAIN_LEVELS + 1));
}

// 刻み tick に刻んだ印(MrBlock::activeTick。0 は「まだ刻んでいない」。2^32 刻みで一周する)
FX_FN uint32_t MrActivityMark(uint64_t tick) {
    return (uint32_t)tick + 1;
}

// --- 構造体 ------------------------------------------------------------------------------------

// 八分の一の箱の面を越えた先
struct MrAcross {
    uint32_t inside;  // 1 なら同じブロックの八分の一 octant
    uint32_t octant;

    // --- 外なら、越えた先の 4³ の箱の原点(このブロックのレベルの座標)---
    int64_t x;
    int64_t y;
    int64_t z;
};

// (八分の一, 面) から起こすもの
struct MrWake {
    uint32_t schedule;  // 起こすブロック(無ければ MR_NO_BLOCK)
    uint32_t descend;   // 面をたどって細かい側へ降りるブロック(無ければ MR_NO_BLOCK)
};

// --- 関数 --------------------------------------------------------------------------------------

FX_FN MrAcross MrAcrossFace(MrBlock block, uint32_t octant, uint32_t face) {
    const uint32_t axis = face >> 1;
    const bool positive = (face & 1u) != 0;
    const uint32_t bit = (octant >> axis) & 1u;

    MrAcross across;
    across.inside = (positive ? bit == 0 : bit == 1) ? 1u : 0u;
    across.octant = octant ^ (1u << axis);
    across.x = block.originX + (int64_t)((octant & 1u) * MR_OCTANT_EDGE);
    across.y = block.originY + (int64_t)(((octant >> 1) & 1u) * MR_OCTANT_EDGE);
    across.z = block.originZ + (int64_t)(((octant >> 2) & 1u) * MR_OCTANT_EDGE);

    const int64_t step = positive ? (int64_t)MR_OCTANT_EDGE : -(int64_t)MR_OCTANT_EDGE;
    if (axis == 0)
        across.x += step;
    else if (axis == 1)
        across.y += step;
    else
        across.z += step;

    return across;
}

// 面 face の向きに進んで入ってきたブロックの、手前の面に接する i 番目(0〜3)の八分の一
FX_FN uint32_t MrNearOctant(uint32_t face, uint32_t i) {
    const uint32_t axis = face >> 1;
    const uint32_t nearBit = (face & 1u) != 0 ? 0u : 1u;
    const uint32_t first = (axis + 1) % 3;
    const uint32_t second = (axis + 2) % 3;

    return (nearBit << axis) | ((i & 1u) << first) | (((i >> 1) & 1u) << second);
}

// レベル level の座標の点を含む、level 以下で最も細かい本物のブロック(根のレベルまで引く。無ければ MR_NO_BLOCK)
template <typename Tree>
FX_FN uint32_t MrFindContaining(Tree tree, int32_t level, int64_t x, int64_t y, int64_t z, int32_t rootLevel) {
    for (int32_t candidate = level; candidate >= rootLevel; --candidate) {
        const uint32_t shift = (uint32_t)(level - candidate);
        const uint32_t slot = tree.Lookup(candidate, MrBlockOriginOf(x >> shift), MrBlockOriginOf(y >> shift),
                                          MrBlockOriginOf(z >> shift));
        if (slot != MR_NO_BLOCK)
            return slot;
    }

    return MR_NO_BLOCK;
}

// ブロックの八分の一 octant の面 face の先で起こすもの。八分の一が覆われている(刻むセルが無い)なら何もしない。
// 先が同じブロックの覆われていない八分の一なら自分なので何もしない
template <typename Tree>
FX_FN MrWake MrWakeAcross(Tree tree, MrBlock block, uint32_t octant, uint32_t face, int32_t rootLevel) {
    MrWake wake;
    wake.schedule = MR_NO_BLOCK;
    wake.descend = MR_NO_BLOCK;
    if (block.children[octant] != MR_NO_BLOCK)
        return wake;

    const MrAcross across = MrAcrossFace(block, octant, face);
    if (across.inside != 0) {
        wake.descend = block.children[across.octant];
        return wake;
    }

    // --- 外: 先の箱を含む最も細かいブロック。粗ければそのセル、同じレベルなら八分の一が覆われているかで分ける ---
    const uint32_t slot = MrFindContaining(tree, block.level, across.x, across.y, across.z, rootLevel);
    if (slot == MR_NO_BLOCK)
        return wake;

    const MrBlock found = tree.Block(slot);
    if (found.level < block.level) {
        wake.schedule = slot;
        return wake;
    }

    const uint32_t foundOctant = (uint32_t)((across.x - found.originX) >> 2) |
                                 ((uint32_t)((across.y - found.originY) >> 2) << 1) |
                                 ((uint32_t)((across.z - found.originZ) >> 2) << 2);
    if (found.children[foundOctant] == MR_NO_BLOCK)
        wake.schedule = slot;
    else
        wake.descend = found.children[foundOctant];

    return wake;
}

// --- 静かなブロックを粗くする(T-0101。05 §4)---------------------------------------------------
// 忙しさの印 busyTick = ブロックが最後に変わった刻みの印。刻んでセルが 1 つでも変わった時(ActivityStepNode)と、木の変更で
// つつかれた時(要求の処理が MR_BUSY_POKED を書き、種として起こす ActivitySeedNode がその刻みの印にする)に書く
// (同じ刻みの書き手は同じ値なので順に依存しない)。「進める規則がある」(種になる)とは分ける: 規則が進めると言っても
// セルが変わらないまま毎刻み種になり続けることがある(燃え尽きかけの木箱。T-0101 で見つけた。17 §5)。
// 刻み t の要求の処理の前に、印が N(MR_QUIET_TICKS)刻みより古い本物の葉を粗くする要求を、世界の枠の順に作る(TreeQuiet)。
// 印は 64bit(MrChangeMark。T-0115)。GPU はまだ 32bit の刻みの印を書く(2^32 刻みで一周。T-0121 で 64bit に)。

// 粗くするまでに続けて静かでなければならない刻みの数(2026-10-04 ユーザー決定: 定数。測って調整する)
FX_CONST uint32_t MR_QUIET_TICKS = 16;

// 本物の葉(根でない・本物の子が無い)で、刻み mark(MrActivityMark)までに N 刻みを超えて変わっていない
FX_FN bool MrIsQuietLeaf(MrBlock block, uint64_t mark) {
    return block.kind == MR_BLOCK_REAL && block.parent != MR_NO_BLOCK && !MrHasRealChild(block) &&
           block.busyTick != MR_BUSY_POKED && mark - block.busyTick > MR_QUIET_TICKS;
}

// 頁を畳めるか調べる刻みか(T-0103。17 §5「頁を畳む」): 世界の本物のブロックで、頁を持ち・端数が無く・つつかれたままでなく、
// 刻み mark でちょうど静かになった(N 刻みを初めて超えた。MrIsQuietLeaf と同じ N)。葉でなくても(根・子のある親も)調べる。
// 調べるのはこの 1 回だけ: 頁のセル・子・端数を変える所(刻む・木の変更・頁に広げる)は全部忙しさの印を書くので、
// 変わった後にはまた N 刻み後に 1 回調べる。静かなまま一様でないブロックを毎刻み調べ直さない
FX_FN bool MrWantsFoldCheck(MrBlock block, uint64_t mark) {
    return block.kind == MR_BLOCK_REAL && !MrIsUniform(block) && block.fraction == MR_NO_FRACTION &&
           block.busyTick != MR_BUSY_POKED && mark - block.busyTick == MR_QUIET_TICKS + 1;
}

// 端数の枠を帳簿へ移して返す刻みか(T-0104。ADR-0017 追記): 世界の本物のブロックで、端数の枠を持ち・つつかれたままでなく、
// 刻み mark でちょうど静かになった(MrWantsFoldCheck と同じ時)。端数はどのセルでも 1 単位未満 = 計器で測れない差なので、
// 許容差つきで畳む時(MrIsExactFold でない)だけ、許容差の値によらず返す
FX_FN bool MrWantsFractionReturn(MrBlock block, uint64_t mark) {
    return block.kind == MR_BLOCK_REAL && block.fraction != MR_NO_FRACTION && block.busyTick != MR_BUSY_POKED &&
           mark - block.busyTick == MR_QUIET_TICKS + 1;
}

// 刻んでセルが変わったか(全部の欄をビットで比べる。並びの余りは比べない)
FX_FN bool MrCellChanged(RxCell before, RxCell after) {
    return !MrSameCell(before, after);
}

// 枠 slot を粗くする要求を作るか。静かな葉で、同じ親の八分の一の番号が小さい兄弟に静かな葉が無い
// (1 刻みに 1 つの親は 1 つの要求しか通らない〔17 §5〕ので、負ける要求で一覧を埋めない)
template <typename Tree>
FX_FN bool MrWantsQuietCoarsen(Tree tree, uint32_t slot, uint64_t mark) {
    const MrBlock block = tree.Block(slot);
    if (!MrIsQuietLeaf(block, mark))
        return false;

    const MrBlock parent = tree.Block(block.parent);
    for (uint32_t octant = 0; octant < block.parentOctant; ++octant) {
        const uint32_t sibling = parent.children[octant];
        if (sibling != MR_NO_BLOCK && MrIsQuietLeaf(tree.Block(sibling), mark))
            return false;
    }

    return true;
}

// ブロックを親へ戻す要求
FX_FN MrRequest MrMakeQuietCoarsenRequest(MrBlock block) {
    return MrMakeRequest(MR_REQUEST_COARSEN, block.level, block.originX, block.originY, block.originZ);
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_ACTIVITY_HLSLI
