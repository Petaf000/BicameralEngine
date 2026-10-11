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

    ReactionTableDefinition MakeCombustionTestTable() {
        auto package = script::ReadPackageFolder(fs::path(BICAMERAL_DATA_PACKAGES_DIR) / TEST_PACKAGE);
        if (!package)
            StopWithoutTable(package.error());

        auto sandbox = script::LuauSandbox::Create(script::SandboxLimits{});
        if (!sandbox)
            StopWithoutTable(sandbox.error());

        (*sandbox)->Seal();
        const std::vector<script::PackageSource> packages{std::move(*package)};
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

    ReactionTableDefinition MakeSpeciesChangedTestTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.species.push_back({.name = "hydrogen",
                                      .composition = {{.element = "H", .count = 2}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 28836,
                                      .thermalConductivity = 18 * 5000});
        definition.species.push_back({.name = "ozone",
                                      .composition = {{.element = "O", .count = 3}},
                                      .formationEnthalpy = 142700,
                                      .heatCapacity = 39200,
                                      .thermalConductivity = 19 * 5000});
        std::erase_if(definition.species, [](const SpeciesDefinition& species) { return species.name == "cellulose"; });
        std::erase_if(definition.rules, [](const RuleDefinition& rule) {
            return std::ranges::any_of(rule.reactants,
                                       [](const RuleTerm& term) { return term.species == "cellulose"; });
        });

        return definition;
    }

}  // namespace bicameral::sim
