// gpu_multires_layers.h — GPU の世界(sim/gpu_multires)の、セルの成分ごとの層の量の置き場の像(T-1221・T-1240。ADR-0481 の 5・ADR-0489)。
// CPU の頁の層(MultiresCellPage::layers)を、GPU の層の置き場(並びは shaders/common/multires_layers.hlsli。頁の索引 + 空きのスタック)の
// 像にする・読み戻した像を CPU の頁の層へ戻す。GpuMultires が RecordUpload・Read で使い、層を合わせる段(shaders/sim/multires_layers.hlsl)が
// 間で付け替え、木の段(頁を返す・畳む)が層の頁を返す。
// 一様の枠の層(MultiresCellStore::uniformLayers)も枠ごとの組に写す・戻す(T-1240 の 4 段目)。
// 溢れを使う世界(T-1240 の 7 の 5)も溢れの成分の層まで物質で組に詰める(組は 1 セル・一様の枠 1 つに RX_MAX_CELL_SPECIES まで)。
// まだの所: GPU の段(層を合わせる段・木の段)が溢れの成分の量を読まないので、GpuMultires::RecordUpload は溢れを使う世界の層をまだ断る。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "sim/multires_nest.h"

namespace bicameral::sim {

    // 層の置き場の像(uint32 の並び。大きさは MrLayerStoreWords(セルの頁の数, capacity, 枠の数))と、使った層の頁の数(頁の番号の順に前から詰める)・
    // 一様の枠の層を持つ枠の数
    struct GpuLayerImage {
        std::vector<uint32_t> words;
        uint32_t used = 0;
        uint32_t uniformSlots = 0;
    };

    // nest の層のある頁を capacity 頁の置き場の像にする(頁の番号の順に層の頁 0, 1, … を配り、残りを空きのスタックに積む)。
    // 層の頁が足りない・層を持つ物質が RX_MAX_CELL_SPECIES 種を超えるセル / 一様の枠がある時は理由を返す
    [[nodiscard]] std::expected<GpuLayerImage, std::string> MakeGpuLayerImage(const MultiresNest& nest,
                                                                              uint32_t capacity);

    // 読み戻した像を nest の頁の層・一様の枠の層にする(全部これで置き換える。セルの値は読み戻した後の nest のもの)。
    // 頁の索引が層の頁を指す頁だけを読む(返した頁は索引から外れている)
    void ApplyGpuLayerImage(MultiresNest& nest, std::span<const uint32_t> words, uint32_t capacity);

}  // namespace bicameral::sim
