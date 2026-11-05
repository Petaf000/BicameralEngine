// multires_refine_halo.hlsli — 木を細かくする段の置かれ方(T-1188)が親のブロックの外の隣を読むための「隣の写し」の並び(T-1189。D-664・20 §10.5)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU は engine/src/sim/multires_refine_cells.cpp の SnapshotRefineHalos と PlaceTreeCell、
// GPU は shaders/sim/multires_refine_halo.hlsl(写しを作る段)と multires_refine_place.hlsli(読む)。
//
// 何を写すか: 細かくする要求の鎖の段ごとに、親のブロック P の八分の一(4³ の親のセル)の周り 1 の窓(6³)のうち、八分の一の外のセル。
// 置かれ方が読むのは、八分の一のセルの列 (x ± 1, z)・(x, z ± 1) と、その列の段 y − 1〜y + 1 だけなので、八分の一の外のセルは
// 軸ごとに 2 つの面(x・y・z)の 6 × 6 に入る:
//   面 0〜2 = P の外を向く面(八分の一の印のビットが 0 なら低い側 −1、1 なら高い側 8。隣のブロックのセル)
//   面 3〜5 = P の中を向く面(ビットが 0 なら 4、1 なら 3。P の別の八分の一のセル。読むのは子に覆われたセルだけ。T-1314)
// 写しの 1 つ = 6 面 × 36 = 216 個の詰まり(Q16。器 = 子 8 つ)。無いセルは MR_HALO_ABSENT。窓の中のセルの番号は 1 つ
// (P の外に出る軸があれば外の面〔x → y → z の順〕、無ければ八分の一の外に出る軸の中の面〔同じ順〕)。
// 写すのは要求の処理の割り当ての後・適用の前(刻みの前の木): 同じ刻みに別の要求が隣のブロックを細かくして(覆って)も・粗くしても、
// 読むのは刻みの前の詰まり(順番に依存しない)。隣は索引で同じレベルの本物のブロックだけ。同じレベルのブロックが無い所は無い
// (T-1176 の 1 本目の仮のまま: 横 → 傾き 0・下 → 詰まり切る・上 → 空)。
// 子に覆われたセル(覆われた親のセルは空・中身は子にある)は覆う子のブロックの子のセル 2³ から読む(T-1314。D-664 の「前線が通って
// 細かくなった所で砂山が勝手に動かない」): 子のセルの量を物質ごとに足し(量は子の和 = 粗くした親 × 8)、子の器の和で詰まりにする
// (置かれ方は子の平均)。細かくしたばかりの子なら和は親 × 8 ちょうどなので、細かくする前の親を読んだのとビット単位で同じ。
// 子のセル 2³ は子のブロックの同じ八分の一にあるので、孫に覆われていれば無い(孫までは降りない。2:1 の木では接する所に来ない)。
// 写しの置き場の番号は、鎖の段 d の子の枠を取った空きのスタックの位置(state.blockBase − 1 − d。許可した要求の段ごとに別々で、世界の枠の数より小さい)。
#ifndef BICAMERAL_MULTIRES_REFINE_HALO_HLSLI
#define BICAMERAL_MULTIRES_REFINE_HALO_HLSLI

#include "multires.hlsli"

MR_NAMESPACE_BEGIN

FX_CONST uint32_t MR_HALO_FACE_EDGE = 6;  // 面の一辺(八分の一の 4 + 両側 1)
FX_CONST uint32_t MR_HALO_FACE_CELLS = 36;
FX_CONST uint32_t MR_HALO_OUTER_FACES = 3;     // P の外を向く面(面 0〜2)。面 3〜5 は P の中を向く面
FX_CONST uint32_t MR_HALO_CELLS = 216;         // 6 面
FX_CONST int32_t MR_HALO_ABSENT = -1;          // 無いセル(同じレベルのブロックが無い・覆う子がさらに孫に覆われた)
FX_CONST uint32_t MR_HALO_NONE = 0xFFFFFFFFu;  // 写しを読まない(要求の外の細かくする・写しの無い世界)

// 面の外の座標(親のブロックの中の座標。八分の一の印のビット 0 → −1・1 → 8)
FX_FN int32_t MrHaloOutside(uint32_t octantBit) {
    return octantBit == 0u ? -1 : (int32_t)MR_BLOCK_EDGE;
}

// 八分の一の始まり(親のブロックの中の座標 1 軸)
FX_FN int32_t MrHaloOctantStart(uint32_t octantBit) {
    return (int32_t)(octantBit * MR_OCTANT_EDGE);
}

// 中を向く面の座標(親のブロックの中の座標。八分の一の印のビット 0 → 4・1 → 3。T-1314)
FX_FN int32_t MrHaloInside(uint32_t octantBit) {
    return octantBit == 0u ? (int32_t)MR_OCTANT_EDGE : (int32_t)MR_OCTANT_EDGE - 1;
}

// 写しの h 番目のセルの親のブロックの中の座標(軸 axis: 0 = x・1 = y・2 = z)。面 a の (u, v) は八分の一の始まりから −1〜4:
// x の面 = (外か中, u, v)・y の面 = (u, 外か中, v)・z の面 = (u, v, 外か中)
FX_FN int32_t MrHaloCellAxis(uint32_t h, uint32_t octant, uint32_t axis) {
    const uint32_t face = (h / MR_HALO_FACE_CELLS) % MR_HALO_OUTER_FACES;
    const bool inner = h >= MR_HALO_OUTER_FACES * MR_HALO_FACE_CELLS;
    const uint32_t rest = h % MR_HALO_FACE_CELLS;
    const int32_t u = (int32_t)(rest % MR_HALO_FACE_EDGE) - 1;
    const int32_t v = (int32_t)(rest / MR_HALO_FACE_EDGE) - 1;
    const uint32_t bit = (octant >> axis) & 1u;
    if (axis == face)
        return inner ? MrHaloInside(bit) : MrHaloOutside(bit);

    const bool first = (face == 0u && axis == 1u) || (face != 0u && axis == 0u);

    return MrHaloOctantStart(bit) + (first ? u : v);
}

// 八分の一の始まりからの座標が窓(−1〜4)の中か
FX_FN bool MrHaloInWindowAxis(int32_t relative) {
    return relative >= -1 && relative <= (int32_t)MR_OCTANT_EDGE;
}

// セル (x, y, z)(親のブロックの中の座標)が八分の一の周り 1 の窓の中か(八分の一の中も含む)
FX_FN bool MrHaloInWindow(int32_t x, int32_t y, int32_t z, uint32_t octant) {
    return MrHaloInWindowAxis(x - MrHaloOctantStart(octant & 1u)) &&
           MrHaloInWindowAxis(y - MrHaloOctantStart((octant >> 1) & 1u)) &&
           MrHaloInWindowAxis(z - MrHaloOctantStart((octant >> 2) & 1u));
}

// 八分の一の始まりからの座標が八分の一(0〜3)の外か
FX_FN bool MrHaloOutsideOctantAxis(int32_t relative) {
    return relative < 0 || relative >= (int32_t)MR_OCTANT_EDGE;
}

// 窓の中で八分の一の外のセル (x, y, z) の写しの番号。親のブロックの外に出る軸があれば外の面(x → y → z の順)、
// 無ければ八分の一の外に出る軸の中の面(同じ順。T-1314)
FX_FN uint32_t MrHaloIndexOf(int32_t x, int32_t y, int32_t z, uint32_t octant) {
    const int32_t edge = (int32_t)MR_BLOCK_EDGE;
    const bool outX = x < 0 || x >= edge;
    const bool outY = y < 0 || y >= edge;
    const bool outZ = z < 0 || z >= edge;
    const int32_t rx = x - MrHaloOctantStart(octant & 1u);
    const int32_t ry = y - MrHaloOctantStart((octant >> 1) & 1u);
    const int32_t rz = z - MrHaloOctantStart((octant >> 2) & 1u);
    const bool outside = outX || outY || outZ;
    const bool alongX = outside ? outX : MrHaloOutsideOctantAxis(rx);
    const bool alongY = outside ? outY : MrHaloOutsideOctantAxis(ry);
    uint32_t face = 2;
    int32_t u = rx;
    int32_t v = ry;
    if (alongX) {
        face = 0;
        u = ry;
        v = rz;
    } else if (alongY) {
        face = 1;
        u = rx;
        v = rz;
    }

    FX_ASSERT(u >= -1 && u <= 4 && v >= -1 && v <= 4);
    FX_ASSERT(outside || alongX || alongY || MrHaloOutsideOctantAxis(rz));

    const uint32_t faceBase = outside ? face : face + MR_HALO_OUTER_FACES;

    return (faceBase * MR_HALO_FACE_CELLS) + (uint32_t)(u + 1) + (MR_HALO_FACE_EDGE * (uint32_t)(v + 1));
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_REFINE_HALO_HLSLI
