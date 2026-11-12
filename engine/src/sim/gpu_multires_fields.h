// gpu_multires_fields.h — GPU の世界の登録した欄(セル・面の欄の疎な頁)の置き場の像を作る・戻す(T-1252 段 2。ADR-0500 の 3・ADR-0532)。
// CPU の置き場(MultiresFieldStore: pageOf・空きのスタック・頁)を、GPU の置き場(並びは shaders/common/multires_field_pages.hlsli)の
// 像にする・読み戻した像を CPU の置き場へ戻す。頁の番号・空きのスタックの並びまでそのまま写す(GPU の段が同じ番号の取り方で続ける)。
// 集めて書く段の作業(NestGatherWork)も GPU の作業の並びに写す・戻す(shaders/sim/multires_field_gather.hlsl が読み書きする)。
// GPU の世界(sim/gpu_multires)には、配り手の欄・この刻みだけの欄を「欄の区画」(u4 の層の置き場の後ろ。並びは multires_field_pages.hlsli)
// として写す・戻す(MakeGpuFieldRegion・ApplyGpuFieldRegion。一様の値まで)。
// 区画は木の段(粗く・細かく・影・畳む・広げる・集めて書く)が読み書きする。まだの所: 密の欄・一般の欄。
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

    // GPU の世界の集めて書く段(GpuMultires::RecordGather)の報告(置き場の頭 words。CPU の NestGatherReport と同じ数:
    // expanded = 頁に広げた一様な枠・cellPageShortage = 頁が足りず一様のまま残った触った枠)
    [[nodiscard]] NestGatherReport GpuTreeGatherReport(std::span<const uint32_t> words);

    // GPU の世界が区画に載せる欄か(セル・面の欄で、配り手の欄かこの刻みだけの欄)
    [[nodiscard]] bool GpuCarriesField(const MultiresField& field);

    // 区画に載せる欄の数・値の数の和
    [[nodiscard]] uint32_t GpuCarriedFieldCount(const MultiresNest& nest);
    [[nodiscard]] uint32_t GpuCarriedFieldValues(const MultiresNest& nest);

    // 欄の区画の像(載せる欄を登録の順に。欄ごとの頁の容量 capacity・枠の数 slots・セルの頁の数 cellPages)。
    // 載せる欄が密・使った頁が容量を超える・頁や枠の数が合わない時は理由を返す
    [[nodiscard]] std::expected<std::vector<uint32_t>, std::string> MakeGpuFieldRegion(const MultiresNest& nest,
                                                                                       uint32_t capacity,
                                                                                       uint32_t slots,
                                                                                       uint32_t cellPages);

    // 読み戻した区画の像を載せた欄の置き場と一様の値へ戻す(表の登録の番号の欄へ)
    void ApplyGpuFieldRegion(MultiresNest& nest, std::span<const uint32_t> words);

    // 作業の像(uint32 の並び。大きさは MfWorkWords(世界の枠の数, 作業のブロックの数, 値の数)。段の結果の語は 0)
    [[nodiscard]] std::vector<uint32_t> MakeGpuGatherWorkImage(const NestGatherWork& work);

    // 読み戻した作業の像の値を work へ戻す(書いた枠は 0・尽きた枠は残る)。段の結果(MF_GATHER_*)は results へ(世界の枠ごと)
    void ApplyGpuGatherWorkImage(NestGatherWork& work, std::span<const uint32_t> words, std::vector<uint32_t>& results);

}  // namespace bicameral::sim
