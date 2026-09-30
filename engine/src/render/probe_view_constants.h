// probe_view_constants.h — デバッグ表示(render/probe_view)のフレームの定数。CPU がフレームごとにアップロードのバッファへ書き、
// shaders/render/probe_view.hlsl が ByteAddressBuffer で読む(並びと値の意味は両方で同じにする)。D3D12 に依存しない(単体テストから使う)。
#pragma once

#include <array>
#include <cstdint>

namespace bicameral::render {

    // 表示の仕方(probe_view.hlsl の VIEW_MODE_*)
    enum class DebugViewMode : uint8_t {
        Volume = 0,             // 吸収と発光(熱い所ほど明るく、濃い)
        MaximumProjection = 1,  // 光線の上の最大値(どこに熱があるか)
        Slice = 2,              // 断面だけ
    };

    // flags のビット(probe_view.hlsl の VIEW_FLAG_*)
    inline constexpr uint32_t VIEW_FLAG_ACTIVE_BLOCKS = 1u << 0;  // 活性なブロックを重ねる
    inline constexpr uint32_t VIEW_FLAG_LOGARITHMIC = 1u << 1;    // 色は対数(無ければ線形)

    struct ProbeViewConstants {
        uint32_t extractionIndex = 0;  // 読む抽出(0〜2)
        uint32_t width = 0;            // 描く大きさ(px)
        uint32_t height = 0;
        uint32_t flags = 0;
        uint32_t mode = 0;  // DebugViewMode
        uint32_t sliceAxis = 2;
        uint32_t slicePosition = 0;
        uint32_t orthographic = 0;
        // カメラ(render/debug_camera.h の CameraBasis。w は使わない)
        std::array<float, 4> position{};
        std::array<float, 4> forward{};
        std::array<float, 4> right{};
        std::array<float, 4> up{};
    };

    static_assert(sizeof(ProbeViewConstants) == 96, "probe_view.hlsl の読み方と合わせる");

}  // namespace bicameral::render
