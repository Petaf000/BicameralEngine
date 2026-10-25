// reaction_test_table.cpp — テストが使う試験の反応表を試験のパッケージから読む(reaction_test_table.h。T-0169)。
// 型検査はしない(パッケージの型と読み込みの入り口は reaction_package_test が確かめる)。
#include "reaction_test_table.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/aliases.h"
#include "script/luau_package.h"
#include "script/luau_sandbox.h"
#include "script/reaction_package.h"
#include "script/standard_packages.h"

namespace bicameral::sim {

    namespace {

        constexpr std::string_view TEST_PACKAGE = "combustion_test";

        // 読めない時は理由を出して止まる(呼び手のテストは表なしでは何も確かめられない)
        [[noreturn]] void StopWithoutTable(const std::string& reason) {
            std::fprintf(stderr, "試験の反応表(data/packages/%.*s)を読めない: %s\n",
                         static_cast<int>(TEST_PACKAGE.size()), TEST_PACKAGE.data(), reason.c_str());
            std::exit(1);
        }

    }  // namespace

    std::vector<script::PackageSource> ReadCombustionTestPackages() {
        auto package = script::ReadPackageFolder(fs::path(BICAMERAL_DATA_PACKAGES_DIR) / TEST_PACKAGE);
        if (!package)
            StopWithoutTable(package.error());

        std::vector<script::PackageSource> packages{std::move(*package)};
        if (auto added = script::AddStandardDependencies(packages, fs::path(BICAMERAL_DATA_STANDARD_DIR)); !added)
            StopWithoutTable(added.error());

        return packages;
    }

    ReactionTableDefinition MakeCombustionTestTable() {
        auto sandbox = script::LuauSandbox::Create(script::SandboxLimits{});
        if (!sandbox)
            StopWithoutTable(sandbox.error());

        (*sandbox)->Seal();
        const std::vector<script::PackageSource> packages = ReadCombustionTestPackages();
        const auto loaded = script::LoadPackages(**sandbox, packages,
                                                 script::PackageLoadOptions{.basePackage = std::string(TEST_PACKAGE)});
        if (!loaded)
            StopWithoutTable(loaded.error());

        if (!loaded->rejected.empty())
            StopWithoutTable(loaded->rejected.front().reason);

        auto definition = script::ReadReactionTableDefinition(*loaded);
        if (!definition)
            StopWithoutTable(definition.error());

        return std::move(*definition);
    }

    void PruneMaterialTemplates(ReactionTableDefinition& definition) {
        const auto known = [&](const std::string& name) {
            return std::ranges::contains(definition.species, name, &SpeciesDefinition::name);
        };

        // --- 消えた物質を指す材料・抽出の枠と、消えた材料を指す既定 ---
        MaterialTemplateDefinition& templates = definition.templates;
        std::erase_if(templates.materials, [&](const MaterialDefinition& material) {
            return !std::ranges::all_of(material.contents,
                                        [&](const MaterialAmountDefinition& amount) { return known(amount.species); });
        });
        for (auto& slot : templates.viewQuantities) {
            if (slot && !known(slot->species))
                slot.reset();
        }

        const auto materialKnown = [&](const std::string& key) {
            return key.empty() || std::ranges::contains(templates.materials, key, &MaterialDefinition::key);
        };
        if (!materialKnown(templates.editor.placeMaterial))
            templates.editor.placeMaterial.clear();

        if (!materialKnown(templates.editor.labFill))
            templates.editor.labFill.clear();
    }

    ReactionTableDefinition MakeSpeciesChangedTestTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.species.push_back({.name = "hydrogen",
                                      .composition = {{.element = "H", .count = 2}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 28836,
                                      .thermalConductivity = 180});  // 現実の値(試験の倍率は T-1066 で外した)
        definition.species.push_back({.name = "ozone",
                                      .composition = {{.element = "O", .count = 3}},
                                      .formationEnthalpy = 142700,
                                      .heatCapacity = 39200,
                                      .thermalConductivity = 19});
        std::erase_if(definition.species, [](const SpeciesDefinition& species) { return species.name == "cellulose"; });
        std::erase_if(definition.rules, [](const RuleDefinition& rule) {
            return std::ranges::any_of(rule.reactants,
                                       [](const RuleTerm& term) { return term.species == "cellulose"; });
        });
        definition.worldLayout =
            {};  // 世界の組み立て(T-0398)は木箱のセルロースを指すので空にする(この表で初めの世界は作らない)

        PruneMaterialTemplates(definition);

        return definition;
    }

    void AddDewCondensationForTest(ReactionTableDefinition& definition, uint64_t ratePerSecond) {
        definition.species.push_back({.name = "test_dew",
                                      .composition = {{.element = "H", .count = 2}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -285830,
                                      .heatCapacity = 75300,
                                      .thermalConductivity = 600,
                                      .phase = SpeciesPhase::Solid});
        definition.rules.push_back(
            {.name = "test_dew_condensation",
             .reactants = {{.species = "water_vapor", .coefficient = 1, .firstOrder = true}},
             .products = {{.species = "test_dew", .coefficient = 1}},
             .rate = {.preExponentialMantissa = ratePerSecond, .preExponentialExponent10 = 0, .activationEnergy = 0}});
    }

}  // namespace bicameral::sim
