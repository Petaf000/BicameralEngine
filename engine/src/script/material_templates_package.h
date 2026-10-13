// material_templates_package.h — パッケージの分類 material_templates・view_quantities・editor_defaults を
// sim::MaterialTemplateDefinition に読む(T-0399・D-454・ADR-0151。13 §2.6)。
//
// データの流れ: LoadPackages(合わせた「分類 → 鍵 → 値」)→ ReadMaterialTemplates(ReadReactionTableDefinition の最後)
//   → ReactionTableDefinition::templates → sim::BakeMaterialTemplates(名前 → ID・知らない物質は誤り)→ 実験室・筆・パネル・抽出・描画。
// 書き方(値は整数だけ。量は 1 セル〔0.5 m 角〕あたりの µmol):
//   material_templates = { wood = { label = "…", order = 2,
//                                   contents = { { species = "cellulose", micromoles = 38600000 }, … } } }  -- 物質は 1〜3 個・書いた順
//   view_quantities = { slot_1 = { label = "…", species = "oxygen", span_micromoles = 1063800, depletion = true } }  -- slot_1〜slot_3
//   editor_defaults = { lab_fill = "air", place_material = "wood", temperature_mk = 300000 }  -- どれも省ける
// 分類が無ければ空(ブランクの世界。D-455)。知らない欄・知らない鍵は誤り(綴りの誤りを黙って通さない)。
// 欄を変えたら、型の定義 data/types/bicameral.d.luau も変える(T-0140・ADR-0046)。ここは CPU だけ。
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "script/luau_package.h"
#include "sim/material_templates.h"

namespace bicameral::script {

    constexpr std::string_view MATERIAL_TEMPLATES_CATEGORY = "material_templates";
    constexpr std::string_view VIEW_QUANTITIES_CATEGORY = "view_quantities";
    constexpr std::string_view EDITOR_DEFAULTS_CATEGORY = "editor_defaults";

    // 合わせた表から材料のひな形・抽出の物質・既定を読む。誤りには「どのパッケージの・どの欄か」を付ける
    [[nodiscard]] std::expected<sim::MaterialTemplateDefinition, std::string> ReadMaterialTemplates(
        const PackageSetResult& packages);

}  // namespace bicameral::script
