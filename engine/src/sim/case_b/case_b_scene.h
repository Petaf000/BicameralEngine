// case_b_scene.h — 案 B の試作の場面(B1。T-0867。21 §5.2)。場面は絵(文字の行)+ 凡例 + 物質 + 見る範囲で書き、
// 細かさ(レベル)を選んで箱を作る。同じ場面から 0.5 m と 2 m の箱を作れる(B-c の「弱い差の戻る時間が 0.5 m と 2 m で同じか」)。
// 絵は 0.5 m のセルで、上の行が上(重さの逆向き = +y)。z は 1 セルの厚さ(どのレベルでも 1 セル)。
// 粗いレベルは絵の factor × factor の区画の割合を平均し、細かいレベルは絵の 1 文字を 2 × 2 に分ける(体積あたりの量は同じ)。
// 気体は、凝縮相(液体・粒そのもの・据わる固体)と物の残りの空きに、凡例の圧力と温度で入れる(孔の中の空気も)。
// データの流れ: Make*Scene()(場面の定義)→ BuildScene(箱。case_b_box)→ 段を差し込んで StepBox・計器(case_b_gauge)。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sim/case_b/case_b_box.h"

namespace bicameral::sim::case_b {

    struct VolumeFill {
        std::string material;
        uint32_t volumePct = 0;  // セルの体積の何 %(粒は粒そのものの体積。詰まった砂 = packed_fraction_pct)
    };

    struct GasFill {
        std::string material;
        uint32_t molePermille = 0;  // 気体の中の物質量の割合(‰。合計 1000)
    };

    struct LegendEntry {
        char symbol = ' ';
        std::vector<VolumeFill> condensed;
        uint64_t gasPressurePa = 0;  // 空きに入れる気体の圧力(0 = 真空)
        std::vector<GasFill> gas;
        uint32_t bodyPct = 0;  // 物の占める体積(T-0820 の口)
    };

    // 見る範囲(絵の 0.5 m のセルで。[begin, end))
    struct SceneRegion {
        std::string name;
        uint32_t xBegin = 0;
        uint32_t xEnd = 0;
        uint32_t yBegin = 0;  // 下から数える
        uint32_t yEnd = 0;
    };

    struct Scene {
        std::string name;
        std::string note;  // 何を確かめる場面か(21 §5.2 の行・合格の目安)
        std::vector<MaterialDefinition> materials;
        EnvironmentDefinition environment;
        std::vector<LegendEntry> legend;
        std::vector<std::string> rows;  // 上の行が上
        uint32_t temperatureMilliKelvin = 300000;
        std::vector<SceneRegion> regions;
    };

    // 箱の範囲(箱のセルで。[begin, end))
    struct BoxRegion {
        uint32_t xBegin = 0;
        uint32_t xEnd = 0;
        uint32_t yBegin = 0;
        uint32_t yEnd = 0;
    };

    // 場面の絵の大きさ(0.5 m のセル)
    [[nodiscard]] uint32_t SceneWidth(const Scene& scene);
    [[nodiscard]] uint32_t SceneHeight(const Scene& scene);

    // 箱を作る(level は −2〜2。粗くするなら絵の大きさが 2^-level で割り切れること)。初めの帳簿は作った後の中身
    [[nodiscard]] std::expected<Box, std::string> BuildScene(const Scene& scene, int32_t level,
                                                             MomentumLayout layout = MomentumLayout::PerPhase,
                                                             Diagnostics* diagnostics = nullptr);

    // 場面の見る範囲を、レベル level の箱のセルで。名前が無ければ std::nullopt
    [[nodiscard]] std::optional<BoxRegion> FindRegion(const Scene& scene, std::string_view name, int32_t level);

    // --- 場面(21 §5.2 の B1 の 5 つ + 弱い差)---
    // 物質: air(気体)・water(液体 1000 kg/m³)・sand(粒 300 µm・粒そのもの 2650 kg/m³・34°・詰まり 60 %)・stone(据わる)
    [[nodiscard]] std::vector<MaterialDefinition> StandardMaterials();

    [[nodiscard]] Scene MakeSandPileScene();   // 砂山(崩れる角度。R3)
    [[nodiscard]] Scene MakeSandLeveeScene();  // 砂の堤と水(染みる・崩れる。R2・R4。浸食は 2 周目)
    [[nodiscard]] Scene MakeDamBreakScene();   // ダムの決壊(T-0610 と同じ形。R1)
    [[nodiscard]] Scene MakeSiphonScene(
        uint32_t crestCells = 16);                 // サイフォン(頂上の高さを変えて切れる高さを見る。R1・R6a)
    [[nodiscard]] Scene MakeTrappedAirScene();     // 閉じた空気の下の水(潜水鐘。R1)
    [[nodiscard]] Scene MakePressureRelaxScene();  // 弱い圧力の差が戻る(閉じた空気の箱。R1 の B-c)

    // R1(T-0868)で足した場面
    [[nodiscard]] Scene MakeStillPondScene();         // 静かな池(静水圧・水面の圧力の揺れ・ほぼ止まる)
    [[nodiscard]] Scene MakeConnectedVesselsScene();  // 連通管(左 4 m・右 1 m の水位が下の管でそろう。釣り合いは 2.5 m)

    [[nodiscard]] std::vector<Scene> AllScenes();

}  // namespace bicameral::sim::case_b
