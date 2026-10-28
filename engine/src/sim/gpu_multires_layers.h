// gpu_multires_layers.h — GPU の世界(sim/gpu_multires)の、セルの成分ごとの層の量の置き場の像(T-1221。ADR-0481 の 5・ADR-0489)。
// CPU の頁の層(MultiresCellPage::layers)を、GPU の層の置き場(並びは shaders/common/multires_layers.hlsli)の像にする・読み戻した像を
// CPU の頁の層へ戻す。GpuMultires が RecordUpload・Read で使い、層を合わせる段(shaders/sim/multires_layers.hlsl)が間で付け替える。
// 1 段目(T-1221)の限り: 溢れを使う世界・一様の枠の層は写さない(写す像を作らずに理由を返す。分けたもの T-1240)。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "sim/multires_nest.h"

namespace bicameral::sim {

    // 層の置き場の像(uint32 の並び。大きさは MrLayerStoreWords(capacity))と、使った層の頁の数(前から詰める)
    struct GpuLayerImage {
        std::vector<uint32_t> words;
        uint32_t used = 0;
    };

    // nest の層のある頁を capacity 頁の置き場の像にする(頁の番号の順に層の頁を詰める。持ち主の枠はその頁を持つ枠)。
    // 層の頁が足りない・溢れを使う世界に層がある・一様の枠の層がある時は理由を返す
    [[nodiscard]] std::expected<GpuLayerImage, std::string> MakeGpuLayerImage(const MultiresNest& nest,
                                                                              uint32_t capacity);

    // 読み戻した像を nest の頁の層にする(頁の層は全部これで置き換える。セルの値は読み戻した後の nest のもの)。
    // 持ち主の枠がもうその頁を持っていない層の頁は読まない(GPU の層を合わせる段と同じ)
    void ApplyGpuLayerImage(MultiresNest& nest, std::span<const uint32_t> words, uint32_t capacity);

}  // namespace bicameral::sim
