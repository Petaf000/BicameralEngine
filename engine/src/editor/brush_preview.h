// brush_preview.h — カーソルの下に筆の形を描くための線を作る(T-0279。docs/design/14 §2「実装(T-0279)」)。
//
// データの流れ:
//   窓のカーソルの画素(render/debug_view_controller の CursorCell。断面のセル)
//   → 筆のパネル(editor/brush_panel の PreviewShape)がそのセルを中心にした今の筆の形 → sim::MakePlaceCommand の語
//   → BuildBrushPreview が、置くコマンドと同じ読み方(common/probe_place.hlsli の ProbeDecodePlace・ProbePlaceCovers)で
//     「断面の上で形が覆うセルの縁」と「形を囲む箱(格子の中に切る)」の線分を格子の座標で作る
//   → ProjectBrushPreview がカメラ(render/debug_camera の ProjectToPixel)で画面の線分にする
//   → エディタの殻(editor/editor_overlay)が ImGui の背景の描画リストに引く。
// 描画側だけ(View の状態。D-107)。世界にも再生ファイルにも入らない。浮動小数点を使ってよい(シミュの外)。
// GPU も D3D12 も ImGui も知らない(CPU だけのテスト brush_preview_test)。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "render/debug_camera.h"

namespace bicameral::editor {

    // 格子の座標の線分(セル (x, y, z) は [x, x + 1) × [y, y + 1) × [z, z + 1)。断面の線は面の真ん中 plane + 0.5)
    struct BrushPreviewSegment {
        render::Vector3 from;
        render::Vector3 to;
        bool onSlice = false;  // 断面の上で覆うセルの縁(false = 形を囲む箱)
    };

    struct BrushPreview {
        uint32_t sliceCells = 0;  // 断面の上で形が覆うセルの数(置くと当たるセル。0 なら断面は形に掛からない)
        std::vector<BrushPreviewSegment> segments;
    };

    // placePayload = 置くコマンドの語(sim::MakePlaceCommand。物質は見ない)。sliceAxis・slicePlane = 断面(クリックが当たる面)
    [[nodiscard]] BrushPreview BuildBrushPreview(std::span<const uint32_t> placePayload, uint32_t sliceAxis,
                                                 uint32_t slicePlane);

    // 画面の線分(画素。左上が 0)
    struct BrushPreviewLine {
        std::array<float, 2> from{};
        std::array<float, 2> to{};
        bool onSlice = false;
    };

    // 線分を画面へ(どちらかの端がカメラの後ろなら落とす)
    [[nodiscard]] std::vector<BrushPreviewLine> ProjectBrushPreview(const BrushPreview& preview,
                                                                    const render::CameraBasis& basis, uint32_t width,
                                                                    uint32_t height);

}  // namespace bicameral::editor
