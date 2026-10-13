// material_templates.h — エディタと表示が使う中身の値(材料のひな形・抽出に写す物質と表示の幅・実験室と置く操作の既定)の型と、
// 物質の名前を ID に直すベイク(T-0399・D-454・ADR-0151。13 §2.6・14 §2)。
//
// データの流れ: パッケージの分類 material_templates・view_quantities・editor_defaults(script/material_templates_package が読む)
//   → MaterialTemplateDefinition(名前で書いた値。ReactionTableDefinition::templates)
//   → BakeMaterialTemplates(BakeReactionTable の最後。物質の名前 → ID・検査)→ MaterialTemplates(BakedReactionTable::templates)
//   → 実験室・筆・パネルの材料(sim::MakeLabMaterials)・実験室の箱を満たす材料(MakeLabBoxNest)・抽出に写す物質(ProbeViewSpecies)・
//     描画の色の幅(render::DebugViewController)。
// エンジンは既定の値を持たない(D-454・D-455): 分類が無い(ブランクの世界)なら材料は 0 個・箱は空のセル・抽出の物質は 0 番(使わない)。
// 反応表と同じ読み込み・同じ表の版に入る(T-0398 の共有の決め事)ので、再生ファイルの表の中身からも同じ値に作り直せる。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bicameral::sim {

    struct BakedReactionTable;

    // 抽出に写す物質の数(common/probe_sim.hlsli の PROBE_VIEW_SPECIES_COUNT と同じ。probe_sim.cpp が確かめる)
    inline constexpr uint32_t VIEW_QUANTITY_SLOT_COUNT = 3;

    // --- 定義(パッケージに書いた名前と値)---

    struct MaterialAmountDefinition {
        std::string species;      // 物質の名前
        uint64_t micromoles = 0;  // 1 セルあたり(µmol)
    };

    // 材料のひな形 1 つ(分類 material_templates の 1 つの鍵)
    struct MaterialDefinition {
        std::string key;                                 // 鍵(自動の確かめはこれで探す)
        std::string label;                               // パネルに出す名前
        int64_t order = 0;                               // パネルの並び(小さい順。同じなら鍵の順)
        std::vector<MaterialAmountDefinition> contents;  // 書いた順(1〜3 個。置くコマンドの上限)
    };

    // 抽出に写す物質 1 つと、描画の色の幅(分類 view_quantities の鍵 slot_1〜slot_3)
    struct ViewQuantityDefinition {
        std::string label;            // 表示の量の名前(「色分けする量」の切り替え)
        std::string species;          // 物質の名前
        uint64_t spanMicromoles = 0;  // この量で色が 1(1 セルあたり µmol)
        bool depletion = false;       // true: 幅からの減り(幅 − 量)を塗る(空気の酸素の減りなど)
    };

    // 実験室と置く操作の既定(分類 editor_defaults)
    struct EditorDefaultsDefinition {
        std::string labFill;                 // 実験室の箱を満たす材料の鍵(空 = 空のセル)
        std::string placeMaterial;           // 実験室と筆の「置く」で初めに選ぶ材料の鍵(空 = 一覧の最初)
        int32_t temperatureMilliKelvin = 0;  // 箱を満たす温度・置く温度の初め(0 = 書いていない)
    };

    struct MaterialTemplateDefinition {
        std::vector<MaterialDefinition> materials;
        std::array<std::optional<ViewQuantityDefinition>, VIEW_QUANTITY_SLOT_COUNT> viewQuantities;
        EditorDefaultsDefinition editor;
    };

    // --- ベイクした値(物質 ID)---

    struct MaterialAmount {
        uint32_t species = 0;  // 物質 ID
        uint64_t micromoles = 0;
    };

    struct MaterialTemplate {
        std::string key;
        std::string label;
        std::vector<MaterialAmount> contents;
    };

    // 書いていない枠は物質 0(抽出は 0 を写す)・幅 0(描画は塗らない)
    struct ViewQuantityTemplate {
        std::string label;
        uint32_t species = 0;
        uint64_t spanMicromoles = 0;
        bool depletion = false;
    };

    struct MaterialTemplates {
        std::vector<MaterialTemplate> materials;  // 並び = order → 鍵
        std::array<ViewQuantityTemplate, VIEW_QUANTITY_SLOT_COUNT> viewQuantities;
        std::optional<uint32_t> labFill;        // materials の添字
        std::optional<uint32_t> placeMaterial;  // materials の添字
        int32_t temperatureMilliKelvin = 0;     // 0 = 書いていない
    };

    // 名前を ID に直して確かめる(知らない物質・知らない材料の鍵・量 0・同じ物質を 2 回・4 つ以上の物質・箱を満たす材料に温度が無い、は誤り)。
    // table は物質までベイクした表(SpeciesId が引ける)
    [[nodiscard]] std::expected<MaterialTemplates, std::string> BakeMaterialTemplates(
        const MaterialTemplateDefinition& definition, const BakedReactionTable& table);

    // 鍵で材料を探す(無ければ nullptr)
    [[nodiscard]] const MaterialTemplate* FindMaterialTemplate(const MaterialTemplates& templates,
                                                               std::string_view key);

}  // namespace bicameral::sim
