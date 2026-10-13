// material_templates.cpp — 材料のひな形・抽出の物質・既定の名前を ID に直すベイク(material_templates.h。T-0399)。
// 誤りには「どの分類の・どの鍵か」を付ける。パッケージの形の誤り(欄の綴り・型)は読む所(script/material_templates_package)が落とす。
#include "sim/material_templates.h"

#include <algorithm>
#include <format>
#include <utility>

#include "common/lab_box.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    namespace {

        // 材料 1 つ: 物質の名前 → ID(知らない物質・量 0・同じ物質を 2 回・置くコマンドに入らない数は誤り)
        std::expected<MaterialTemplate, std::string> BakeMaterial(const MaterialDefinition& definition,
                                                                  const BakedReactionTable& table) {
            const std::string where = std::format("material_templates.{}", definition.key);
            if (definition.contents.empty() || definition.contents.size() > lab::LAB_MAX_FILL_SPECIES)
                return std::unexpected(std::format("{}: 物質は 1〜{} 個", where, lab::LAB_MAX_FILL_SPECIES));

            MaterialTemplate baked{.key = definition.key, .label = definition.label, .contents = {}};
            for (const MaterialAmountDefinition& amount : definition.contents) {
                const uint32_t species = table.SpeciesId(amount.species);
                if (species == 0)
                    return std::unexpected(std::format("{}: 知らない物質 {}", where, amount.species));

                if (amount.micromoles == 0)
                    return std::unexpected(std::format("{}: {} の量が 0", where, amount.species));

                if (std::ranges::contains(baked.contents, species, &MaterialAmount::species))
                    return std::unexpected(std::format("{}: {} が 2 回ある", where, amount.species));

                baked.contents.push_back({.species = species, .micromoles = amount.micromoles});
            }

            return baked;
        }

        // 抽出の物質の枠 1 つ(書いていない枠は物質 0・幅 0)
        std::expected<ViewQuantityTemplate, std::string> BakeViewQuantity(
            const std::optional<ViewQuantityDefinition>& definition, uint32_t slot, const BakedReactionTable& table) {
            if (!definition)
                return ViewQuantityTemplate{};

            const uint32_t species = table.SpeciesId(definition->species);
            if (species == 0)
                return std::unexpected(
                    std::format("view_quantities.slot_{}: 知らない物質 {}", slot + 1, definition->species));

            if (definition->spanMicromoles == 0)
                return std::unexpected(std::format("view_quantities.slot_{}: 幅が 0", slot + 1));

            return ViewQuantityTemplate{.label = definition->label,
                                        .species = species,
                                        .spanMicromoles = definition->spanMicromoles,
                                        .depletion = definition->depletion};
        }

        // 既定の材料の鍵 → 添字(空なら無し。知らない鍵は誤り)
        std::expected<std::optional<uint32_t>, std::string> IndexOfMaterial(const MaterialTemplates& templates,
                                                                            const std::string& key,
                                                                            std::string_view field) {
            if (key.empty())
                return std::optional<uint32_t>{};

            const auto found = std::ranges::find(templates.materials, key, &MaterialTemplate::key);
            if (found == templates.materials.end())
                return std::unexpected(std::format("editor_defaults.{}: 知らない材料 {}", field, key));

            return static_cast<uint32_t>(found - templates.materials.begin());
        }

    }  // namespace

    std::expected<MaterialTemplates, std::string> BakeMaterialTemplates(const MaterialTemplateDefinition& definition,
                                                                        const BakedReactionTable& table) {
        MaterialTemplates templates;

        // --- 材料(並び = order → 鍵。定義の並びに依らない)---
        std::vector<const MaterialDefinition*> ordered;
        for (const MaterialDefinition& material : definition.materials)
            ordered.push_back(&material);

        std::ranges::sort(ordered, [](const MaterialDefinition* a, const MaterialDefinition* b) {
            return a->order != b->order ? a->order < b->order : a->key < b->key;
        });
        for (size_t i = 1; i < ordered.size(); ++i) {
            if (ordered[i]->key == ordered[i - 1]->key)
                return std::unexpected(std::format("material_templates.{} が 2 回ある", ordered[i]->key));
        }

        for (const MaterialDefinition* material : ordered) {
            auto baked = BakeMaterial(*material, table);
            if (!baked)
                return std::unexpected(baked.error());

            templates.materials.push_back(std::move(*baked));
        }

        // --- 抽出の物質と色の幅 ---
        for (uint32_t slot = 0; slot < VIEW_QUANTITY_SLOT_COUNT; ++slot) {
            auto view = BakeViewQuantity(definition.viewQuantities[slot], slot, table);
            if (!view)
                return std::unexpected(view.error());

            templates.viewQuantities[slot] = std::move(*view);
        }

        // --- 既定(箱を満たす材料には温度が要る)---
        const EditorDefaultsDefinition& editor = definition.editor;
        const auto labFill = IndexOfMaterial(templates, editor.labFill, "lab_fill");
        if (!labFill)
            return std::unexpected(labFill.error());

        const auto placeMaterial = IndexOfMaterial(templates, editor.placeMaterial, "place_material");
        if (!placeMaterial)
            return std::unexpected(placeMaterial.error());

        if (labFill->has_value() && editor.temperatureMilliKelvin <= 0)
            return std::unexpected("editor_defaults: lab_fill を書いたら temperature_mk(1 以上)も書く");

        templates.labFill = *labFill;
        templates.placeMaterial = *placeMaterial;
        templates.temperatureMilliKelvin = editor.temperatureMilliKelvin;

        return templates;
    }

    const MaterialTemplate* FindMaterialTemplate(const MaterialTemplates& templates, std::string_view key) {
        const auto found = std::ranges::find(templates.materials, key, &MaterialTemplate::key);

        return found != templates.materials.end() ? &*found : nullptr;
    }

}  // namespace bicameral::sim
