// multires_refine_halo.hlsli — 木を細かくする段の置かれ方(T-1188)が親のブロックの外の隣を読むための「隣の写し」の並び(T-1189。D-664・20 §10.5)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU は engine/src/sim/multires_refine_cells.cpp の SnapshotRefineHalos と PlaceTreeCell、
// GPU は shaders/sim/multires_refine_halo.hlsl(写しを作る段)と multires_refine_place.hlsli(読む)。
//
// 何を写すか: 細かくする要求の鎖の段ごとに、親のブロック P の八分の一(4³ の親のセル)の周り 1 の窓(6³)のうち、P の外に出るセル。
// 置かれ方が読むのは、八分の一のセルの列 (x ± 1, z)・(x, z ± 1) と、その列の段 y − 1〜y + 1 だけなので、外に出るセルは
// 外を向く 3 つの面(x・y・z。八分の一の印のビットが 0 なら低い側 −1、1 なら高い側 8)の 6 × 6 に入る。
// 写しの 1 つ = 3 面 × 36 = 108 個の詰まり(Q16。器 = 子 8 つ)。無いセルは MR_HALO_ABSENT。
// 写すのは要求の処理の割り当ての後・適用の前(刻みの前の木): 同じ刻みに別の要求が隣のブロックを細かくして(覆って)も・粗くしても、
// 読むのは刻みの前の詰まり(順番に依存しない)。隣は索引で同じレベルの本物のブロックだけ。無い・子に覆われたセルは無い(T-1176 の 1 本目の仮のまま:
// 横 → 傾き 0・下 → 詰まり切る・上 → 空)。
// 写しの置き場の番号は、鎖の段 d の子の枠を取った空きのスタックの位置(state.blockBase − 1 − d。許可した要求の段ごとに別々で、世界の枠の数より小さい)。
#ifndef BICAMERAL_MULTIRES_REFINE_HALO_HLSLI
#define BICAMERAL_MULTIRES_REFINE_HALO_HLSLI

#include "multires.hlsli"

MR_NAMESPACE_BEGIN

FX_CONST uint32_t MR_HALO_FACE_EDGE = 6;  // 面の一辺(八分の一の 4 + 両側 1)
FX_CONST uint32_t MR_HALO_FACE_CELLS = 36;
FX_CONST uint32_t MR_HALO_CELLS = 108;         // 3 面
FX_CONST int32_t MR_HALO_ABSENT = -1;          // 無いセル(ブロックが無い・子に覆われた)
FX_CONST uint32_t MR_HALO_NONE = 0xFFFFFFFFu;  // 写しを読まない(要求の外の細かくする・写しの無い世界)

// 面の外の座標(親のブロックの中の座標。八分の一の印のビット 0 → −1・1 → 8)
FX_FN int32_t MrHaloOutside(uint32_t octantBit) {
    return octantBit == 0u ? -1 : (int32_t)MR_BLOCK_EDGE;
}

// 八分の一の始まり(親のブロックの中の座標 1 軸)
FX_FN int32_t MrHaloOctantStart(uint32_t octantBit) {
    return (int32_t)(octantBit * MR_OCTANT_EDGE);
}

// 写しの h 番目のセルの親のブロックの中の座標(軸 axis: 0 = x・1 = y・2 = z)。面 a の (u, v) は八分の一の始まりから −1〜4:
// x の面 = (外, u, v)・y の面 = (u, 外, v)・z の面 = (u, v, 外)
FX_FN int32_t MrHaloCellAxis(uint32_t h, uint32_t octant, uint32_t axis) {
    const uint32_t face = h / MR_HALO_FACE_CELLS;
    const uint32_t rest = h % MR_HALO_FACE_CELLS;
    const int32_t u = (int32_t)(rest % MR_HALO_FACE_EDGE) - 1;
    const int32_t v = (int32_t)(rest / MR_HALO_FACE_EDGE) - 1;
    const uint32_t bit = (octant >> axis) & 1u;
    if (axis == face)
        return MrHaloOutside(bit);

    const bool first = (face == 0u && axis == 1u) || (face != 0u && axis == 0u);

    return MrHaloOctantStart(bit) + (first ? u : v);
}

// 親のブロックの外のセル (x, y, z)(窓の中)の写しの番号。x が外なら x の面、でなければ y、でなければ z の面
FX_FN uint32_t MrHaloIndexOf(int32_t x, int32_t y, int32_t z, uint32_t octant) {
    const int32_t edge = (int32_t)MR_BLOCK_EDGE;
    const bool outX = x < 0 || x >= edge;
    const bool outY = y < 0 || y >= edge;
    const int32_t rx = x - MrHaloOctantStart(octant & 1u);
    const int32_t ry = y - MrHaloOctantStart((octant >> 1) & 1u);
    const int32_t rz = z - MrHaloOctantStart((octant >> 2) & 1u);
    uint32_t face = 2;
    int32_t u = rx;
    int32_t v = ry;
    if (outX) {
        face = 0;
        u = ry;
        v = rz;
    } else if (outY) {
        face = 1;
        u = rx;
        v = rz;
    }

    FX_ASSERT(u >= -1 && u <= 4 && v >= -1 && v <= 4);

    return (face * MR_HALO_FACE_CELLS) + (uint32_t)(u + 1) + (MR_HALO_FACE_EDGE * (uint32_t)(v + 1));
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_REFINE_HALO_HLSLI
