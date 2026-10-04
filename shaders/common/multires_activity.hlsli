// multires_activity.hlsli — 多重解像度の木の上の活性: どのブロックを刻むか・面の隣を起こす(05 §4・06 §2・17 §5「活性」。T-0100)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/multires_activity.cpp の StepActive)と
// GPU(shaders/sim/multires_graph.hlsl の ActivitySeedNode・WakeFaceNode・ActivityStepNode)が同じ関数を呼ぶ。
//
// 活性の規則(仮の世界の T-0005・T-0089 を木に広げたもの): 刻み t で刻む世界のブロック = 種とその面の隣。
//   種 = 刻み t − 1 に進める反応の規則があったブロック(D-424: 無ければ刻みが変わっても変わらない)+ 刻み t に木の変更でつつかれたブロック。
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

// 一覧のレコードの数(空のレコード + 世界の枠 + 1 刻みにつつかれる数の上限)
FX_FN uint32_t MrActivityCapacity(uint32_t worldBlocks) {
    return 1 + worldBlocks + (MR_MAX_REQUESTS * (MR_MAX_CHAIN_LEVELS + 1));
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

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_ACTIVITY_HLSLI
