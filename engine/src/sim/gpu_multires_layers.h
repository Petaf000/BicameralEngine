// gpu_multires_layers.h — GPU の世界(sim/gpu_multires)の、セルの成分ごとの層の量の置き場の像(T-1221・T-1240。ADR-0481 の 5・ADR-0489)。
// CPU の頁の層(MultiresCellPage::layers)を、GPU の層の置き場(並びは shaders/common/multires_layers.hlsli。頁の索引 + 空きのスタック)の
// 像にする・読み戻した像を CPU の頁の層へ戻す。GpuMultires が RecordUpload・Read で使い、層を合わせる段(shaders/sim/multires_layers.hlsl)が
// 間で付け替え、木の段(頁を返す・畳む)が層の頁を返す。
// まだの所(T-1240 の続き): 溢れを使う世界・一様の枠の層は写さない(写す像を作らずに理由を返す)。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "sim/multires_nest.h"

namespace bicameral::sim {

    // 層の置き場の像(uint32 の並び。大きさは MrLayerStoreWords(セルの頁の数, capacity))と、使った層の頁の数(頁の番号の順に前から詰める)
    struct GpuLayerImage {
        std::vector<uint32_t> words;
        uint32_t used = 0;
    };

    // nest の層のある頁を capacity 頁の置き場の像にする(頁の番号の順に層の頁 0, 1, … を配り、残りを空きのスタックに積む)。
    // 層の頁が足りない・溢れを使う世界に層がある・一様の枠の層がある時は理由を返す
    [[nodiscard]] std::expected<GpuLayerImage, std::string> MakeGpuLayerImage(const MultiresNest& nest,
                                                                              uint32_t capacity);

    // 読み戻した像を nest の頁の層にする(頁の層は全部これで置き換える。セルの値は読み戻した後の nest のもの)。
    // 頁の索引が層の頁を指す頁だけを読む(返した頁は索引から外れている)
    void ApplyGpuLayerImage(MultiresNest& nest, std::span<const uint32_t> words, uint32_t capacity);

}  // namespace bicameral::sim
