// gpu_multires_fields.h — GPU の世界の登録した欄(セル・面の欄の疎な頁)の置き場の像を作る・戻す(T-1252 段 2。ADR-0500 の 3・ADR-0532)。
// CPU の置き場(MultiresFieldStore: pageOf・空きのスタック・頁)を、GPU の置き場(並びは shaders/common/multires_field_pages.hlsli)の
// 像にする・読み戻した像を CPU の置き場へ戻す。頁の番号・空きのスタックの並びまでそのまま写す(GPU の段が同じ番号の取り方で続ける)。
// 集めて書く段の作業(NestGatherWork)も GPU の作業の並びに写す・戻す(shaders/sim/multires_field_gather.hlsl が読み書きする)。
// まだの所(T-1252 の段 2 の残り): 一様の値(枠 × 値の数)・密の欄・GPU の世界(sim/gpu_multires)の木の段への載せ。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "sim/multires_fields.h"
#include "sim/multires_nest.h"

namespace bicameral::sim {

    // 欄 field の置き場の像(uint32 の並び。大きさは MfStoreWords(セルの頁の数, capacity, 値の数))。
    // 疎でない・セルの欄でない・使った頁が capacity を超える時は理由を返す
    [[nodiscard]] std::expected<std::vector<uint32_t>, std::string> MakeGpuFieldImage(const MultiresNest& nest,
                                                                                      uint32_t field,
                                                                                      uint32_t capacity);

    // 読み戻した像を欄 field の置き場にする(pageOf・空きのスタック・使った高さまでの頁を置き換える。頁の予算はそのまま)
    void ApplyGpuFieldImage(MultiresNest& nest, uint32_t field, std::span<const uint32_t> words);

    // 像の集めて書く段の報告(CPU の NestGatherReport の形。expanded は「触った一様な枠」= 木の段が広げる数・cellPageShortage は 0)
    [[nodiscard]] NestGatherReport GpuFieldGatherReport(std::span<const uint32_t> words);

    // 作業の像(uint32 の並び。大きさは MfWorkWords(世界の枠の数, 作業のブロックの数, 値の数)。段の結果の語は 0)
    [[nodiscard]] std::vector<uint32_t> MakeGpuGatherWorkImage(const NestGatherWork& work);

    // 読み戻した作業の像の値を work へ戻す(書いた枠は 0・尽きた枠は残る)。段の結果(MF_GATHER_*)は results へ(世界の枠ごと)
    void ApplyGpuGatherWorkImage(NestGatherWork& work, std::span<const uint32_t> words, std::vector<uint32_t>& results);

}  // namespace bicameral::sim
