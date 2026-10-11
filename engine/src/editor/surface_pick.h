// surface_pick.h — 筆の「面に沿って置く」(T-0332・docs/design/14 §2「実装(T-0332)」): カーソルの光線が最初に当たる物のセルの手前のセルを探す。
//
// データの流れ:
//   GPU が作って読み戻した物のセルの印(sim/probe_surface。1 セル 1 ビット。数フレーム前の世界)+ 窓の画素の光線(render/debug_camera)
//   → PickSurfaceCell が格子を光線に沿って 1 セルずつ進み(Amanatides–Woo の DDA)、最初に物のセルに入る直前の空いたセルと、
//     その時に越えた面の軸(面の向き。カーソルの下の形を描く断面の軸)を返す
//   → 筆の運び(editor/brush_stroke)が中心にする。置く位置はコマンドとして記録されるので、再生はここを通らない。
// 描画側・エディタ側だけ(View。D-107)。浮動小数点を使ってよい(シミュの外)。GPU を知らない(CPU だけのテスト surface_pick_test)。
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "render/debug_camera.h"

namespace bicameral::editor {

    struct SurfaceHit {
        render::CellCoordinate cell;  // 物のセルの手前の空いたセル(置く中心)
        uint32_t normalAxis = 0;      // 物のセルとの間の面の軸(0 = x・1 = y・2 = z)
    };

    // 印(語 w のビット b = セルの番号 32w + b。番号は x + N·y + N²·z)で、cell が物か
    [[nodiscard]] bool IsSurfaceSolid(std::span<const uint32_t> words, uint32_t gridSize,
                                      const render::CellCoordinate& cell);

    // 光線が格子の中で最初に入る物のセルの手前のセル。物に当たらない・印が空・格子に入った最初のセルが物(手前が無い)なら無し
    [[nodiscard]] std::optional<SurfaceHit> PickSurfaceCell(const render::CameraRay& ray,
                                                            std::span<const uint32_t> words, uint32_t gridSize);

}  // namespace bicameral::editor
