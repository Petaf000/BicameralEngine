// material_templates_package.cpp — 分類 material_templates・view_quantities・editor_defaults を読む(material_templates_package.h。T-0399)。
// ここで落とすのは形の誤り(欄の綴り・型・整数でない数・範囲の外)。物質の名前の検査はベイク(sim::BakeMaterialTemplates)。
#include "script/material_templates_package.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <utility>

#include "script/package_fields.h"

namespace bicameral::script {

    namespace {

        using sim::EditorDefaultsDefinition;
        using sim::MaterialAmountDefinition;
        using sim::MaterialDefinition;
        using sim::MaterialTemplateDefinition;
        using sim::ViewQuantityDefinition;

        constexpr int64_t INT32_LIMIT = std::numeric_limits<int32_t>::max();
        constexpr std::array<std::string_view, sim::VIEW_QUANTITY_SLOT_COUNT> VIEW_SLOT_KEYS = {"slot_1", "slot_2",
                                                                                                "slot_3"};

        // --- 欄を読む道具(整数・欄の検査は script/package_fields。文字列だけここに)---

        // 値の文字列(無い・文字列でない・空は誤り)
        std::expected<std::string, std::string> ReadText(const ScriptValue* value, std::string_view path) {
            if (value == nullptr || !value->IsString() || value->text.empty())
                return std::unexpected(std::format("{}: 空でない文字列で書く", path));

            return value->text;
        }

        std::expected<std::string, std::string> ReadTextField(const ScriptValue& table, std::string_view field,
                                                              std::string_view path) {
            return ReadText(table.Find(field), std::format("{}.{}", path, field));
        }

        // --- material_templates ---

        // contents = { { species = "…", micromoles = … }, … }(書いた順。鍵は 1, 2, 3…)
        std::expected<std::vector<MaterialAmountDefinition>, std::string> ReadContents(const ScriptValue& value,
                                                                                       std::string_view path) {
            constexpr std::array<std::string_view, 2> FIELDS = {"species", "micromoles"};
            if (!value.IsTable())
                return std::unexpected(std::format("{}: 表(列)で書く", path));

            std::vector<MaterialAmountDefinition> contents;
            for (const ScriptField& field : value.fields) {
                const std::string itemPath = std::format("{}[{}]", path, contents.size() + 1);
                const bool listed = field.key.IsNumber() &&
                                    field.key.number == static_cast<double>(contents.size() + 1);
                if (!listed)
                    return std::unexpected(std::format("{}: 列(1, 2, 3…)で書く", path));

                if (auto record = CheckRecord(field.value, FIELDS, itemPath); !record)
                    return std::unexpected(record.error());

                auto species = ReadTextField(field.value, "species", itemPath);
                if (!species)
                    return std::unexpected(species.error());

                const auto micromoles = ReadIntegerField(field.value, "micromoles", itemPath, 1,
                                                         PACKAGE_EXACT_INTEGER_LIMIT);
                if (!micromoles)
                    return std::unexpected(micromoles.error());

                contents.push_back({.species = std::move(*species), .micromoles = static_cast<uint64_t>(*micromoles)});
            }

            return contents;
        }

        std::expected<MaterialDefinition, std::string> ReadMaterial(const std::string& key, const ScriptValue& value,
                                                                    std::string_view path) {
            constexpr std::array<std::string_view, 3> FIELDS = {"label", "order", "contents"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            auto label = ReadTextField(value, "label", path);
            if (!label)
                return std::unexpected(label.error());

            const ScriptValue* orderValue = value.Find("order");
            int64_t order = 0;
            if (orderValue != nullptr) {
                const auto read = ReadInteger(*orderValue, std::format("{}.order", path), -INT32_LIMIT, INT32_LIMIT);
                if (!read)
                    return std::unexpected(read.error());

                order = *read;
            }

            const ScriptValue* contentsValue = value.Find("contents");
            if (contentsValue == nullptr)
                return std::unexpected(std::format("{}: 欄 contents がない", path));

            auto contents = ReadContents(*contentsValue, std::format("{}.contents", path));
            if (!contents)
                return std::unexpected(contents.error());

            return MaterialDefinition{
                .key = key, .label = std::move(*label), .order = order, .contents = std::move(*contents)};
        }

        // --- view_quantities ---

        std::expected<ViewQuantityDefinition, std::string> ReadViewQuantity(const ScriptValue& value,
                                                                            std::string_view path) {
            constexpr std::array<std::string_view, 4> FIELDS = {"label", "species", "span_micromoles", "depletion"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            auto label = ReadTextField(value, "label", path);
            if (!label)
                return std::unexpected(label.error());

            auto species = ReadTextField(value, "species", path);
            if (!species)
                return std::unexpected(species.error());

            const auto span = ReadIntegerField(value, "span_micromoles", path, 1, PACKAGE_EXACT_INTEGER_LIMIT);
            if (!span)
                return std::unexpected(span.error());

            const ScriptValue* depletion = value.Find("depletion");
            if (depletion != nullptr && depletion->kind != ScriptValue::Kind::Boolean)
                return std::unexpected(std::format("{}.depletion: true か false で書く", path));

            return ViewQuantityDefinition{.label = std::move(*label),
                                          .species = std::move(*species),
                                          .spanMicromoles = static_cast<uint64_t>(*span),
                                          .depletion = depletion != nullptr && depletion->boolean};
        }

        // --- 分類ごと ---

        const std::map<std::string, TableEntry>* FindCategory(const PackageSetResult& packages,
                                                              std::string_view category) {
            const auto found = packages.tables.find(std::string(category));

            return found != packages.tables.end() ? &found->second : nullptr;
        }

        std::expected<void, std::string> ReadMaterials(const PackageSetResult& packages,
                                                       MaterialTemplateDefinition& definition) {
            return ReadCategory(packages, MATERIAL_TEMPLATES_CATEGORY, ReadMaterial, definition.materials);
        }

        std::expected<void, std::string> ReadViewQuantities(const PackageSetResult& packages,
                                                            MaterialTemplateDefinition& definition) {
            const auto* entries = FindCategory(packages, VIEW_QUANTITIES_CATEGORY);
            if (entries == nullptr)
                return {};

            for (const auto& [key, entry] : *entries) {
                const std::string path = std::format("{}.{}", VIEW_QUANTITIES_CATEGORY, key);
                const auto slot = std::ranges::find(VIEW_SLOT_KEYS, key);
                if (slot == VIEW_SLOT_KEYS.end())
                    return std::unexpected(std::format("パッケージ {}: {}: 知らない鍵(slot_1〜slot_{})", entry.package,
                                                       path, sim::VIEW_QUANTITY_SLOT_COUNT));

                auto quantity = ReadViewQuantity(entry.value, path);
                if (!quantity)
                    return std::unexpected(std::format("パッケージ {}: {}", entry.package, quantity.error()));

                definition.viewQuantities[static_cast<size_t>(slot - VIEW_SLOT_KEYS.begin())] = std::move(*quantity);
            }

            return {};
        }

        std::expected<void, std::string> ReadEditorDefaults(const PackageSetResult& packages,
                                                            MaterialTemplateDefinition& definition) {
            const auto* entries = FindCategory(packages, EDITOR_DEFAULTS_CATEGORY);
            if (entries == nullptr)
                return {};

            EditorDefaultsDefinition& editor = definition.editor;
            for (const auto& [key, entry] : *entries) {
                const std::string path = std::format("{}.{}", EDITOR_DEFAULTS_CATEGORY, key);
                std::expected<void, std::string> read;
                if (key == "lab_fill" || key == "place_material") {
                    auto text = ReadText(&entry.value, path);
                    if (text)
                        (key == "lab_fill" ? editor.labFill : editor.placeMaterial) = std::move(*text);
                    else
                        read = std::unexpected(text.error());
                } else if (key == "temperature_mk") {
                    const auto temperature = ReadInteger(entry.value, path, 1, INT32_LIMIT);
                    if (temperature)
                        editor.temperatureMilliKelvin = static_cast<int32_t>(*temperature);
                    else
                        read = std::unexpected(temperature.error());
                } else
                    read = std::unexpected(
                        std::format("{}: 知らない鍵(lab_fill・place_material・temperature_mk)", path));

                if (!read)
                    return std::unexpected(std::format("パッケージ {}: {}", entry.package, read.error()));
            }

            return {};
        }

    }  // namespace

    std::expected<MaterialTemplateDefinition, std::string> ReadMaterialTemplates(const PackageSetResult& packages) {
        MaterialTemplateDefinition definition;
        if (auto result = ReadMaterials(packages, definition); !result)
            return std::unexpected(result.error());

        if (auto result = ReadViewQuantities(packages, definition); !result)
            return std::unexpected(result.error());

        if (auto result = ReadEditorDefaults(packages, definition); !result)
            return std::unexpected(result.error());

        return definition;
    }

}  // namespace bicameral::script
