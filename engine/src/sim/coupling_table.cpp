// coupling_table.cpp — 流れどうしの結合(drags・machines)のベイクと、共通の検査(coupling_table.h。T-0536・ADR-0238)。
// 押さえ方と効率の上限は押す構造の模型(tests/push_model_test の BakeMachine)・引きずりの模型(tests/drag_model_test)と同じ式。
// 浮動小数点は使わない。
#include "sim/coupling_table.h"

#include <algorithm>
#include <format>
#include <utility>

#include "common/fixed.hlsli"
#include "common/units.hlsli"
#include "core/aliases.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    namespace {

        constexpr int64_t ONE_Q32 = int64_t{1} << 32;
        constexpr int64_t MILLIJOULES_PER_JOULE = 1000;
        constexpr int64_t MICROMOLES_PER_MOLE = 1000000;

        std::string_view FormCategory(CouplingForm form) {
            return form == CouplingForm::Drag ? "drags" : "machines";
        }

        // 分類ごとに使える種類(drags: 熱と成分の交差。machines: 蓄え X は仕事・熱・成分、押す Y は気体・熱・成分)
        bool Allowed(CouplingForm form, bool driver, CouplingQuantityKind kind) {
            if (form == CouplingForm::Drag)
                return kind == CouplingQuantityKind::Heat || kind == CouplingQuantityKind::Species;

            if (driver)
                return kind != CouplingQuantityKind::Gas;

            return kind != CouplingQuantityKind::Work;
        }

        std::expected<uint32_t, std::string> SpeciesIdOf(const std::string& name, const BakedReactionTable& table,
                                                         std::string_view path) {
            const uint32_t id = table.SpeciesId(name);
            if (id == 0)
                return std::unexpected(std::format("{}: 物質 {} が表に無い", path, name));

            return id;
        }

        std::expected<CouplingQuantity, std::string> BakeQuantity(const CouplingQuantityDefinition& definition,
                                                                  const BakedReactionTable& table,
                                                                  std::string_view path) {
            CouplingQuantity quantity{.kind = definition.kind, .species = 0};
            if (definition.kind != CouplingQuantityKind::Species)
                return quantity;

            const auto id = SpeciesIdOf(definition.species, table, path);
            if (!id)
                return std::unexpected(id.error());

            quantity.species = *id;

            return quantity;
        }

        // 費用 w(J/mol)→ mJ/µmol の Q16、速さ P(W)→ mJ/刻み(どちらも 0 の向きに切り捨て。w と P はどんな正の値でも法則を破らない)
        void BakeMachineNumbers(const CouplingDefinition& definition, Coupling& coupling, BakedCouplings& out,
                                std::string_view path) {
            coupling.costQ16 = definition.costJoulesPerMole * MILLIJOULES_PER_JOULE * COUPLING_ONE_Q16 /
                               MICROMOLES_PER_MOLE;
            coupling.speedPerTick = definition.speedWatts * MILLIJOULES_PER_JOULE /
                                    static_cast<int64_t>(fx::TICKS_PER_SECOND);
            if (coupling.costQ16 > 0 && coupling.speedPerTick >= 0)
                return;

            coupling.disabled = true;
            coupling.costQ16 = COUPLING_ONE_Q16;
            coupling.speedPerTick = 0;
            out.warnings.push_back(
                std::format("{}: 費用 cost_j_per_mol は正(刻みの単位で 0 にならない大きさ)・速さ speed_w は 0 "
                            "以上で書く。止めた構造にした",
                            path));
        }

        std::expected<Coupling, std::string> BakeOne(const CouplingDefinition& definition,
                                                     const BakedReactionTable& table, BakedCouplings& out) {
            const std::string path = std::format("{}.{}", FormCategory(definition.form), definition.key);
            if (!Allowed(definition.form, true, definition.driver.kind) ||
                !Allowed(definition.form, false, definition.driven.kind))
                return std::unexpected(
                    std::format("{}: この分類で使えない流れの種類(drags は heat・species、machines の "
                                "store は work・heat・species、push は gas・heat・species)",
                                path));

            const auto driver = BakeQuantity(definition.driver, table, path);
            if (!driver)
                return std::unexpected(driver.error());

            const auto driven = BakeQuantity(definition.driven, table, path);
            if (!driven)
                return std::unexpected(driven.error());

            if (*driver == *driven)
                return std::unexpected(std::format("{}: 流れ X と Y が同じ(自分を引きずる・押す結合は無い)", path));

            Coupling coupling{.key = definition.key, .form = definition.form, .driver = *driver, .driven = *driven};
            if (!definition.withinSpecies.empty()) {
                const auto within = SpeciesIdOf(definition.withinSpecies, table, path);
                if (!within)
                    return std::unexpected(within.error());

                coupling.withinSpecies = *within;
            }

            // --- 共通の検査: |q| ≤ 1(ADR-0220・ADR-0228)---
            const ClampedCoupling clamped = ClampCouplingQ16(definition.couplingQ16);
            coupling.writtenCouplingQ16 = definition.couplingQ16;
            coupling.couplingQ16 = clamped.couplingQ16;
            coupling.peakEfficiencyQ16 = PeakEfficiencyQ16(clamped.couplingQ16);
            if (clamped.clamped)
                out.warnings.push_back(std::format(
                    "{}: 結合の強さ |q| > 1 は永久機関になる(坂を上る組・効率 > 1 の状態がある)。{} に押さえた", path,
                    clamped.couplingQ16 < 0 ? "-1" : "1"));

            if (definition.form == CouplingForm::Machine)
                BakeMachineNumbers(definition, coupling, out, path);

            return coupling;
        }

    }  // namespace

    ClampedCoupling ClampCouplingQ16(int64_t couplingQ16) {
        if (fx::FxAbsU64(couplingQ16) <= static_cast<uint64_t>(COUPLING_ONE_Q16))
            return {.couplingQ16 = couplingQ16, .clamped = false};

        return {.couplingQ16 = couplingQ16 < 0 ? -COUPLING_ONE_Q16 : COUPLING_ONE_Q16, .clamped = true};
    }

    int64_t PeakEfficiencyQ16(int64_t couplingQ16) {
        const uint64_t slipQ32 = static_cast<uint64_t>(ONE_Q32 - (couplingQ16 * couplingQ16));
        const auto rootQ16 = static_cast<int64_t>(fx::FxSqrtU64(slipQ32));

        return (COUPLING_ONE_Q16 - rootQ16) * COUPLING_ONE_Q16 / (COUPLING_ONE_Q16 + rootQ16);
    }

    std::expected<BakedCouplings, std::string> BakeCouplings(const std::vector<CouplingDefinition>& definitions,
                                                             const BakedReactionTable& table) {
        std::vector<const CouplingDefinition*> ordered;
        for (const CouplingDefinition& definition : definitions)
            ordered.push_back(&definition);

        rng::sort(ordered, {}, [](const CouplingDefinition* definition) {
            return std::pair(static_cast<uint8_t>(definition->form), definition->key);
        });

        BakedCouplings baked;
        for (size_t index = 0; index < ordered.size(); ++index) {
            const CouplingDefinition& definition = *ordered[index];
            if (index > 0 && ordered[index - 1]->form == definition.form && ordered[index - 1]->key == definition.key)
                return std::unexpected(std::format("{}.{} が 2 回ある", FormCategory(definition.form), definition.key));

            auto coupling = BakeOne(definition, table, baked);
            if (!coupling)
                return std::unexpected(coupling.error());

            baked.couplings.push_back(std::move(*coupling));
        }

        return baked;
    }

    std::string CouplingQuantityName(const CouplingQuantity& quantity, const BakedReactionTable& table) {
        switch (quantity.kind) {
            case CouplingQuantityKind::Heat: return "heat";
            case CouplingQuantityKind::Work: return "work";
            case CouplingQuantityKind::Gas: return "gas";
            case CouplingQuantityKind::Species: break;
        }

        const bool known = quantity.species < table.speciesNames.size();

        return std::format("species:{}", known ? table.speciesNames[quantity.species] : std::string("?"));
    }

}  // namespace bicameral::sim
