// material_rows.cpp — 材料を表の全物質から組む行(material_rows.h)。T-0220 の lab_panel から移した。
#include "editor/material_rows.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

namespace bicameral::editor {
    namespace {

        constexpr double MICROMOLES_PER_MOLE = 1e6;
        constexpr double MILLIGRAMS_PER_GRAM = 1e3;
        constexpr double PASCALS_PER_KILOPASCAL = 1e3;
        constexpr double MAX_AMOUNT_INPUT = 1e9;  // 入力の上限(mol・g・kPa。整数に直す時に溢れないように)

    }  // namespace

    uint64_t MaterialRowMicromoles(const sim::BakedReactionTable& table, const MaterialRow& row,
                                   uint32_t temperatureMilliKelvin) {
        if (row.species <= 0 || static_cast<size_t>(row.species) >= table.molarMasses.size() || row.amount <= 0.0)
            return 0;

        const double amount = std::min(row.amount, MAX_AMOUNT_INPUT);
        switch (row.unit) {
            case AmountUnit::Mole: return static_cast<uint64_t>(std::llround(amount * MICROMOLES_PER_MOLE));
            case AmountUnit::Gram:
                return sim::LabMassMicromoles(static_cast<uint64_t>(std::llround(amount * MILLIGRAMS_PER_GRAM)),
                                              table.molarMasses[static_cast<size_t>(row.species)]);
            case AmountUnit::KiloPascal:
                return sim::LabGasMicromoles(static_cast<uint64_t>(std::llround(amount * PASCALS_PER_KILOPASCAL)),
                                             temperatureMilliKelvin);
        }

        return 0;
    }

    std::vector<sim::SpeciesAmount> ComposeMaterial(const sim::BakedReactionTable& table,
                                                    std::span<const MaterialRow> rows,
                                                    uint32_t temperatureMilliKelvin) {
        std::vector<sim::SpeciesAmount> contents;
        for (const MaterialRow& row : rows) {
            const uint64_t amount = MaterialRowMicromoles(table, row, temperatureMilliKelvin);
            const auto species = static_cast<uint32_t>(row.species);
            const bool repeated = std::ranges::any_of(
                contents, [&](const sim::SpeciesAmount& c) { return c.species == species; });
            if (amount > 0 && !repeated)
                contents.push_back({.species = species, .amount = amount});
        }

        return contents;
    }

    void LoadMaterialRows(const sim::LabMaterial& material, MaterialRows& rows) {
        rows = {};
        for (size_t i = 0; i < std::min(material.contents.size(), rows.size()); ++i) {
            rows[i] = {.species = static_cast<int>(material.contents[i].species),
                       .amount = static_cast<double>(material.contents[i].amount) / MICROMOLES_PER_MOLE,
                       .unit = AmountUnit::Mole};
        }
    }

    void RemapMaterialRows(const sim::BakedReactionTable& from, const sim::BakedReactionTable& to, MaterialRows& rows) {
        for (MaterialRow& row : rows) {
            const auto oldId = static_cast<size_t>(row.species);
            const bool known = row.species > 0 && oldId < from.speciesNames.size();
            row.species = known ? static_cast<int>(to.SpeciesId(from.speciesNames[oldId])) : 0;
        }
    }

    void BuildMaterialRowsUi(const sim::BakedReactionTable& table, MaterialRows& rows,
                             uint32_t temperatureMilliKelvin) {
        constexpr std::array<const char*, 3> UNIT_NAMES = {"mol", "g", "kPa(分圧)"};
        for (size_t i = 0; i < rows.size(); ++i) {
            MaterialRow& row = rows[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(150.0f);
            const auto speciesIndex = static_cast<size_t>(std::max(row.species, 0));
            const char* name = row.species > 0 && speciesIndex < table.speciesNames.size()
                                   ? table.speciesNames[speciesIndex].c_str()
                                   : "(なし)";
            if (ImGui::BeginCombo("##species", name)) {
                if (ImGui::Selectable("(なし)", row.species <= 0))
                    row.species = 0;

                for (size_t id = 1; id < table.speciesNames.size(); ++id) {
                    if (ImGui::Selectable(table.speciesNames[id].c_str(), speciesIndex == id))
                        row.species = static_cast<int>(id);
                }

                ImGui::EndCombo();
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::InputDouble("##amount", &row.amount, 0.0, 0.0, "%.4g");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            int unit = static_cast<int>(row.unit);
            if (ImGui::Combo("##unit", &unit, UNIT_NAMES.data(), static_cast<int>(UNIT_NAMES.size())))
                row.unit = static_cast<AmountUnit>(unit);

            ImGui::SameLine();
            ImGui::Text("= %llu µmol",
                        static_cast<unsigned long long>(MaterialRowMicromoles(table, row, temperatureMilliKelvin)));
            ImGui::PopID();
        }
    }

}  // namespace bicameral::editor
