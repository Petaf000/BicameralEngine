// world_edge.hlsli — 世界の端(運ぶ計算の共通の土台の段 3。T-0721・ADR-0337 Proposed。設計 20 §1・§2.6・D-514)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。世界(World)の面の隣が開いた端を MR_NEIGHBOR_EDGE で返し(WeEdgeNeighbor)、
// 当てる段(transport_apply.hlsli の TxFlowCell)が外のセル(World::Outside)と流して帳簿へ(World::AddEdge)。GPU の段は後のチケット。
//
// 外との境は世界の端だけ(区画の箱は無い)。端は 3 つ(上 = 空・下 = 地の底・横の 4 面)で、作り手が環境の world_edges に
// open / wall を書く(T-0710。sim::WorldEdges)。
//   - wall: 何も通さない(面の隣が無い = MR_NEIGHBOR_NONE。T-0721 より前の既定と同じ)
//   - open: 世界の箱のすぐ外に「外のセル」(基準の大気をその高さで読んだセル)が同じレベルで並んでいるとして、面を通して流れる。
//     外のセルは値を変えない(無限の溜め)。流れた分は世界の帳簿の「世界の端」の列へ(出 = 外へ出た・入 = 外から入った)。
// 流れの種類から見ると、開いた端の面は「同じレベルの面で、相手の見立てが外のセルから来る」だけ(種類は外のセルを読むだけで、
// 端の場合分けを持たない)。熱は外の温度へ、気体は外の大気と出入り、液体は外のセルが空なので出ていくだけ、が同じ式から出る。
// 基準の大気の無い世界(ブランク)で開いた端は真空(外のセルが空 = 熱は伝わらない・気体は出ていくだけ)。
//
// 世界の上の軸は WE_UP_AXIS の 1 か所だけで決まる(上 = +y。場面・パッケージの座標の「上」は y〔D-518。Godot・T-0619 と同じ〕。T-0772・ADR-0338)。
// 世界の端・外の柱・MR の気体(sim/multires_gas の気体の箱の軸の写し WeAxisUpLast)がこれを読む。
// レベル 0 のセルの縦の座標 U の高さは海面から U × 0.5 m(セルの下の面。仮の世界の気体の区画の床 0 と同じ)。
// 世界の箱は根のレベルのセルの座標(low を含み high を含まない)。
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_WORLD_EDGE_HLSLI
#define BICAMERAL_WORLD_EDGE_HLSLI

#include "multires_conduction.hlsli"

MR_NAMESPACE_BEGIN

// --- 面の隣の種類(multires_conduction.hlsli の MR_NEIGHBOR_* の続き)---
FX_CONST uint32_t
    MR_NEIGHBOR_EDGE = 4;  // 開いた世界の端(外のセルと同じレベルの面として流れる。相手は変わらず、帳簿に載る)

// --- 端の 3 つ(どの端の外か)---
FX_CONST uint32_t WE_EDGE_INSIDE = 0;  // 世界の中(端ではない)
FX_CONST uint32_t WE_EDGE_TOP = 1;     // 上(空。縦の軸の +)
FX_CONST uint32_t WE_EDGE_BOTTOM = 2;  // 下(地の底。縦の軸の −)
FX_CONST uint32_t WE_EDGE_SIDES = 3;   // 横の 4 面(縦でない 2 つの軸の ±)

// --- 端の種類(sim::WorldEdge と同じ値)---
FX_CONST uint32_t WE_WALL = 0;
FX_CONST uint32_t WE_OPEN = 1;

FX_CONST uint32_t WE_UP_AXIS = 1;  // 世界の上の軸(0 = x・1 = y・2 = z)。y(D-518。ここだけを替えれば上が替わる)

// 縦を最後に置いた軸の順(横 1・横 2・縦)の order 番目の世界の軸。縦が z の箱(気体の箱 GasConfig は z が上)へ写す時に使う
// (上が z なら x・y・z のまま、y なら x・z・y = 仮の世界の GRID_AXIS_OF_GAS_AXIS と同じ)
FX_FN uint32_t WeAxisUpLast(uint32_t order) {
    if (order == 2)
        return WE_UP_AXIS;

    if (order == 0)
        return WE_UP_AXIS == 0 ? 1u : 0u;

    return WE_UP_AXIS == 2 ? 1u : 2u;
}

// ブロックの中の番号 index のセルの軸 axis の座標・ブロックの原点の軸 axis の座標
FX_FN uint32_t WeCellAxis(uint32_t index, uint32_t axis) {
    return axis == 0 ? MrCellX(index) : (axis == 1 ? MrCellY(index) : MrCellZ(index));
}

FX_FN int64_t WeOriginAxis(MrBlock block, uint32_t axis) {
    return axis == 0 ? block.originX : (axis == 1 ? block.originY : block.originZ);
}

// 世界の箱と端の種類(世界のデータ = 環境。状態に入らない)。既定(全部 0)は全部 wall = 端を流さない
struct WeWorld {
    int32_t rootLevel;  // 箱の座標のレベル(MR の根のレベル)

    // --- 世界の箱(根のレベルのセルの座標。low を含み high を含まない)---
    int64_t lowX;
    int64_t lowY;
    int64_t lowZ;
    int64_t highX;
    int64_t highY;
    int64_t highZ;

    // --- 端の種類(WE_WALL / WE_OPEN)---
    uint32_t top;
    uint32_t bottom;
    uint32_t sides;
};

// 軸 axis の箱の下の端・上の端(根のレベル)。動的な添字の配列にしない(T-0397)
FX_FN int64_t WeLow(WeWorld world, uint32_t axis) {
    return axis == 0 ? world.lowX : (axis == 1 ? world.lowY : world.lowZ);
}

FX_FN int64_t WeHigh(WeWorld world, uint32_t axis) {
    return axis == 0 ? world.highX : (axis == 1 ? world.highY : world.highZ);
}

// 端 edge の種類(WE_EDGE_INSIDE は wall と同じ = 流さない)
FX_FN uint32_t WeEdgeKind(WeWorld world, uint32_t edge) {
    if (edge == WE_EDGE_TOP)
        return world.top;

    if (edge == WE_EDGE_BOTTOM)
        return world.bottom;

    return edge == WE_EDGE_SIDES ? world.sides : WE_WALL;
}

// 本物のブロック block のセル index の面 face(軸 = face / 2、正の向き = face % 2)の先が、世界の箱のどの端の外か。
// 先のセルは面の軸の座標だけが自分と違うので、その軸だけを箱と比べる(レベル level の箱 = 根の箱 × 2^(level − rootLevel))
FX_FN uint32_t WeFaceEdge(WeWorld world, MrBlock block, uint32_t index, uint32_t face) {
    const uint32_t axis = face >> 1;
    const bool positive = (face & 1u) != 0;
    const int64_t outside = WeOriginAxis(block, axis) + (int64_t)WeCellAxis(index, axis) + (positive ? 1 : -1);
    const uint32_t shift = (uint32_t)(block.level - world.rootLevel);
    if (outside >= WeLow(world, axis) * ((int64_t)1 << shift) && outside < WeHigh(world, axis) * ((int64_t)1 << shift))
        return WE_EDGE_INSIDE;

    if (axis != WE_UP_AXIS)
        return WE_EDGE_SIDES;

    return positive ? WE_EDGE_TOP : WE_EDGE_BOTTOM;
}

// 開いた端の面か(本物のブロックだけ。影のブロックの外は今まで通り断熱 = 影は自分の中だけ)
FX_FN bool WeIsOpenFace(WeWorld world, MrBlock block, uint32_t index, uint32_t face) {
    if (block.kind != MR_BLOCK_REAL)
        return false;

    return WeEdgeKind(world, WeFaceEdge(world, block, index, face)) == WE_OPEN;
}

// 外のセルの高さ: レベル level のセルの縦の座標 up の中心を含む、レベル 0 のセルの縦の座標(外の柱の行を選ぶ)。
// レベルの単位は 8^-level なので、同じ密度・温度の外のセルはどのレベルでも同じ数(柱はレベル 0 の 1 列で足りる。設計 20 §3)
FX_FN int64_t WeLevelZeroUp(int32_t level, int64_t up) {
    if (level >= 0)
        return up >> (uint32_t)level;

    const uint32_t coarser = (uint32_t)(-level);

    return (up * ((int64_t)1 << coarser)) + ((int64_t)1 << (coarser - 1));
}

// 面 face の先の外のセルの縦の座標(レベル block.level の単位。横の面は自分と同じ高さ)
FX_FN int64_t WeOutsideUp(MrBlock block, uint32_t index, uint32_t face) {
    const int64_t up = WeOriginAxis(block, WE_UP_AXIS) + (int64_t)WeCellAxis(index, WE_UP_AXIS);
    if ((face >> 1) != WE_UP_AXIS)
        return up;

    return up + ((face & 1u) != 0 ? 1 : -1);
}

// 外の柱の行(base = 柱の 0 行目のレベル 0 の縦の座標・rows = 行の数。柱の外の高さは端の行の値のまま。呼ぶ側の約束: rows ≥ 1)。
// 環境から作る柱(sim::MakeMultiresWorldEdges)は世界の箱の中の層だけを持つので、上下の外は端の層の基準を読む
// (仮の世界の気体の開いた境界の外と同じ。T-0772)
FX_FN uint32_t WeColumnRow(int64_t levelZeroUp, int64_t base, uint32_t rows) {
    const int64_t row = levelZeroUp - base;
    if (row < 0)
        return 0;

    return row >= (int64_t)rows ? rows - 1 : (uint32_t)row;
}

// 面の先が世界の外(MrFindFaceNeighbor が MR_NEIGHBOR_NONE)の時の面の隣: 開いた端なら MR_NEIGHBOR_EDGE(index = 外の柱の行)、
// wall・影の外・世界の中の木の穴・外の柱が無い(columnRows = 0 = 真空。熱は伝わらない)なら MR_NEIGHBOR_NONE
FX_FN MrFaceNeighbor WeEdgeNeighbor(WeWorld world, int64_t columnBase, uint32_t columnRows, MrBlock block,
                                    uint32_t index, uint32_t face) {
    if (columnRows == 0 || !WeIsOpenFace(world, block, index, face))
        return MrMakeFaceNeighbor(MR_NEIGHBOR_NONE, MR_NO_BLOCK, 0, 0);

    const int64_t height = WeLevelZeroUp(block.level, WeOutsideUp(block, index, face));

    return MrMakeFaceNeighbor(MR_NEIGHBOR_EDGE, MR_NO_BLOCK, WeColumnRow(height, columnBase, columnRows), 0);
}

MR_NAMESPACE_END

#endif  // BICAMERAL_WORLD_EDGE_HLSLI
