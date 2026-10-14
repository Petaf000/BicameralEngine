// standard_packages_test.cpp — 標準パッケージ(data/standard)と環境ミキサーの足す一覧(T-0373・ADR-0182・ADR-0183)を CPU だけで確かめる。
// 見るもの: 棚の一覧(名前・見せる名前・依存)/ 足す・外すが依存をたどる / 依存が足りない一覧・棚に無い名前を落とす /
//           各パッケージを依存と一緒に足すとベイクが通り(文献の反応熱との食い違いも無い)、全部足した表が期待どおり
//           (元素・物質・規則の数・地球の環境 = 試験の写し StandardWorldPhysics・基準の大気・空気で満たすセル)/
//           足す順に依らず同じ表の版 / 何も足さないとブランクの表と同じ版 / サンプルの世界に足すとぶつかる鍵のパッケージは読まない /
//           基準の大気の検査(割合の合計・知らない物質・温度と圧力の有無)/ マニフェストの title / ホットリロードの読む元の差し替え。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "script/standard_packages.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "script/environment_package.h"
#include "script/luau_sandbox.h"
#include "script/reaction_hot_reload.h"
#include "script/reaction_table_loader.h"
#include "standard_world_physics.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::script;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    using Names = std::vector<std::string>;

    const Names ALL_PACKAGES = {"earth_air", "earth_environment", "real_elements", "real_matter", "real_reactions"};

    bool Contains(std::string_view text, std::string_view part) {
        if (text.find(part) != std::string_view::npos)
            return true;

        std::printf("  '%.*s' に '%.*s' が無い\n", static_cast<int>(text.size()), text.data(),
                    static_cast<int>(part.size()), part.data());

        return false;
    }

    std::expected<StandardCatalog, std::string> Catalog() {
        return ReadStandardCatalog(fs::path(BICAMERAL_DATA_STANDARD_DIR));
    }

    // ブランクの世界に標準パッケージだけを足して読む(窓の --world blank --mix と同じ読み方)
    std::expected<LoadedReactionTable, std::string> LoadMix(const StandardCatalog& catalog, const Names& mix) {
        auto folders = MixFolders(catalog, mix);
        if (!folders)
            return std::unexpected(folders.error());

        return LoadReactionTable({.basePackage = {}, .readPackageRoot = false, .addedPackages = std::move(*folders)});
    }

    // --- 棚と足す一覧 ---

    void TestCatalog() {
        const auto catalog = Catalog();
        if (!catalog) {
            std::printf("FAILED: 棚を読めない: %s\n", catalog.error().c_str());
            ++failureCount;

            return;
        }

        Names names;
        for (const StandardPackage& package : catalog->packages)
            names.push_back(package.name);

        EXPECT(names == ALL_PACKAGES);

        const StandardPackage* matter = catalog->Find("real_matter");
        EXPECT(matter != nullptr && matter->title == "現実の物質" && matter->depends == Names{"real_elements"});

        const StandardPackage* elements = catalog->Find("real_elements");
        EXPECT(elements != nullptr && elements->depends.empty());
        EXPECT(catalog->Find("combustion_test") == nullptr);
    }

    void TestMixAddRemove() {
        const auto catalog = Catalog();
        if (!catalog)
            return;

        // 足すと依存も入る(名前の順)
        const auto reactions = MixAdd(*catalog, {}, "real_reactions");
        EXPECT(reactions && *reactions == (Names{"real_elements", "real_matter", "real_reactions"}));

        const auto withEarth = MixAdd(*catalog, *reactions, "earth_environment");
        EXPECT(withEarth &&
               *withEarth == (Names{"earth_environment", "real_elements", "real_matter", "real_reactions"}));

        const auto unknown = MixAdd(*catalog, {}, "no_such_package");
        EXPECT(!unknown && Contains(unknown.error(), "no_such_package"));

        // 外すと、それに依存するものも外れる
        EXPECT(MixRemove(*catalog, ALL_PACKAGES, "real_matter") == Names{"real_elements"});
        EXPECT(MixRemove(*catalog, ALL_PACKAGES, "real_elements").empty());
        EXPECT(MixRemove(*catalog, ALL_PACKAGES, "earth_air") ==
               (Names{"earth_environment", "real_elements", "real_matter", "real_reactions"}));
        EXPECT(MixDependents(*catalog, ALL_PACKAGES, "real_matter") ==
               (Names{"earth_air", "earth_environment", "real_reactions"}));

        // 依存が足りない一覧・棚に無い名前は読むフォルダにしない
        const auto missing = MixFolders(*catalog, Names{"real_reactions"});
        EXPECT(!missing && Contains(missing.error(), "real_reactions は real_matter が要る"));
        EXPECT(!MixFolders(*catalog, Names{"no_such_package"}));
        const auto folders = MixFolders(*catalog, ALL_PACKAGES);
        EXPECT(folders && folders->size() == ALL_PACKAGES.size());

        // --mix の書き方
        const auto parsed = ParseMixList("real_elements,real_matter");
        EXPECT(parsed && *parsed == (Names{"real_elements", "real_matter"}));
        EXPECT(!ParseMixList("real_elements,,real_matter"));
        EXPECT(!ParseMixList("Real"));
    }

    // --- 中身 ---

    // 各パッケージを依存と一緒に足すと、ベイクが通り、読まなかったパッケージも食い違いの警告も無い
    void TestEachPackageLoadsWithDependencies() {
        const auto catalog = Catalog();
        if (!catalog)
            return;

        for (const std::string& name : ALL_PACKAGES) {
            const auto mix = MixAdd(*catalog, {}, name);
            if (!mix) {
                std::printf("FAILED: %s の依存をたどれない: %s\n", name.c_str(), mix.error().c_str());
                ++failureCount;
                continue;
            }

            const auto loaded = LoadMix(*catalog, *mix);
            if (!loaded) {
                std::printf("FAILED: %s を足せない: %s\n", name.c_str(), loaded.error().c_str());
                ++failureCount;
                continue;
            }

            EXPECT(loaded->rejected.empty());
            // 坂の警告(T-0492)は除く: 標準の real_reactions の boudouard は一方向で 949 K 以下で坂を上る(D-487 ②。警告して動かす)
            EXPECT(std::ranges::none_of(loaded->table.warnings, [](const std::string& warning) {
                return !warning.starts_with(sim::SLOPE_WARNING_PREFIX);
            }));
            EXPECT(loaded->loadOrder.size() == mix->size());
        }
    }

    void TestFullMix() {
        const auto catalog = Catalog();
        if (!catalog)
            return;

        const auto loaded = LoadMix(*catalog, ALL_PACKAGES);
        if (!loaded) {
            std::printf("FAILED: 全部を足せない: %s\n", loaded.error().c_str());
            ++failureCount;

            return;
        }

        const sim::BakedReactionTable& table = loaded->table;
        EXPECT(table.elementNames.size() == 5);
        EXPECT(table.speciesNames.size() == 1 + 8);  // 0 番は使わない
        EXPECT(table.rules.size() == 5);
        EXPECT(table.SpeciesId("mana_test") == 0);  // 魔素は標準パッケージに無い(サンプルの試験の物質)

        // 地球の環境 = 試験の写し(tests/standard_world_physics.h)
        EXPECT(table.environment.gravityMicrometersPerSecond2 ==
               sim::StandardWorldPhysics().gravityMicrometersPerSecond2);
        EXPECT(table.environment.soundSpeedMillimetersPerSecond ==
               sim::StandardWorldPhysics().soundSpeedMillimetersPerSecond);

        // 基準の大気: 4 つの成分・足して 1・15 ℃・1 気圧
        EXPECT(table.atmosphere.fractions.size() == 4);
        uint64_t total = 0;
        for (const sim::AtmosphereFraction& fraction : table.atmosphere.fractions)
            total += fraction.partsPerMillion;

        EXPECT(total == sim::ATMOSPHERE_PARTS_PER_WHOLE);
        EXPECT(table.atmosphere.temperatureMilliKelvin == 288150);
        EXPECT(table.atmosphere.surfacePressurePascals == 101325);

        // 空気で満たすセル: 格子の全部・4 つの成分・理想気体で 1 気圧(0.125 m³ に 1 万分の 1 の精度で)
        EXPECT(table.worldLayout.cellRegions.size() == 1);
        if (table.worldLayout.cellRegions.size() == 1) {
            const sim::WorldCellRegion& air = table.worldLayout.cellRegions.front();
            EXPECT(air.wholeWorld);

            uint64_t micromoles = 0;
            uint32_t count = 0;
            for (uint32_t slot = 0; slot < reaction::RX_MAX_CELL_SPECIES; ++slot) {
                if (air.cell.amounts[slot] == 0)
                    continue;

                micromoles += air.cell.amounts[slot];
                ++count;
            }

            EXPECT(count == 4);
            const double pascals = (double)micromoles * 1e-6 * 8.314462618 * 288.15 / 0.125;
            EXPECT(pascals > 101315.0 && pascals < 101335.0);
        }

        // 足す順に依らず同じ表(版)
        const Names reversed(ALL_PACKAGES.rbegin(), ALL_PACKAGES.rend());
        const auto again = LoadMix(*catalog, reversed);
        EXPECT(again && again->tableVersion == loaded->tableVersion);

        // 再生の作り直し(表の中身だけ)でも同じ大気
        const auto rebuilt = RebuildReactionTable(loaded->tableBytes, loaded->tableVersion);
        EXPECT(rebuilt && rebuilt->table.atmosphere.fractions.size() == 4 &&
               rebuilt->table.atmosphere.surfacePressurePascals == 101325);
    }

    // 何も足さない読み方 = ブランクの表(D-455)
    void TestEmptyMixIsBlank() {
        const auto empty = LoadReactionTable({.basePackage = {}, .readPackageRoot = false});
        const auto blank = LoadBlankReactionTable();
        EXPECT(empty && blank && empty->tableVersion == blank->tableVersion);
        EXPECT(empty && empty->table.atmosphere.Empty() && empty->table.environment.gravityMicrometersPerSecond2 == 0);
    }

    // サンプルの世界(combustion_test)は標準パッケージの上に作る(D-490 ①・T-0498): 依存の標準パッケージは棚から読み、
    // サンプルは現実の物質の熱伝導率だけを書き直す(印なし)。同じ標準パッケージを足しても同じ表(版が同じ・読まないものも無い)
    void TestSampleWithMix() {
        const auto catalog = Catalog();
        if (!catalog)
            return;

        const auto sample = LoadReactionTable({.packageRoot = fs::path(BICAMERAL_DATA_PACKAGES_DIR),
                                               .standardRoot = fs::path(BICAMERAL_DATA_STANDARD_DIR)});
        const auto folders = MixFolders(*catalog, Names{"real_elements", "real_matter"});
        if (!sample || !folders) {
            std::printf("FAILED: サンプルか足すフォルダを読めない: %s\n", sample ? "" : sample.error().c_str());
            ++failureCount;

            return;
        }

        EXPECT((sample->loadOrder ==
                Names{"real_elements", "real_matter", "earth_environment", "real_reactions", "combustion_test"}));
        EXPECT(sample->rejected.empty() && !sample->modifiedWorld);
        EXPECT(sample->standardRoot == fs::path(BICAMERAL_DATA_STANDARD_DIR));
        EXPECT(!sample->overrides.empty() && std::ranges::all_of(sample->overrides, [](const TableOverride& entry) {
            return entry.category == "species" && entry.previousPackage == "real_matter" &&
                   entry.package == "combustion_test";
        }));

        // 熱伝導率は現実の値に試験の倍率(固体 20000・気体 5000。units.luau)。ほかの欄は現実の値のまま
        const auto standardOnly = LoadMix(*catalog, Names{"real_elements", "real_matter"});
        if (standardOnly) {
            const auto conductivity = [](const sim::BakedReactionTable& table, std::string_view name) -> int64_t {
                for (size_t index = 0; index < table.speciesNames.size(); ++index) {
                    if (table.speciesNames[index] == name)
                        return static_cast<int64_t>(table.species[index].conductivity);
                }

                return -1;
            };
            EXPECT(conductivity(sample->table, "cellulose") == conductivity(standardOnly->table, "cellulose") * 20000);
            EXPECT(conductivity(sample->table, "oxygen") == conductivity(standardOnly->table, "oxygen") * 5000);
            EXPECT(conductivity(sample->table, "argon") == conductivity(standardOnly->table, "argon") * 5000);
        }

        const auto mixed = LoadReactionTable({.packageRoot = fs::path(BICAMERAL_DATA_PACKAGES_DIR),
                                              .addedPackages = *folders,
                                              .standardRoot = fs::path(BICAMERAL_DATA_STANDARD_DIR)});
        EXPECT(mixed && mixed->tableVersion == sample->tableVersion);
        EXPECT(mixed && mixed->rejected.empty());
    }

    // --- 基準の大気の検査 ---

    void TestAtmosphereChecks() {
        const auto catalog = Catalog();
        if (!catalog)
            return;

        const auto loaded = LoadMix(*catalog, Names{"real_elements", "real_matter"});
        if (!loaded)
            return;

        const sim::BakedReactionTable& table = loaded->table;
        const auto bake = [&](sim::WorldAtmosphereDefinition definition) {
            return sim::BakeWorldAtmosphere(definition, table);
        };

        const sim::WorldAtmosphereDefinition good{.fractions = {{"nitrogen", 790000}, {"oxygen", 210000}},
                                                  .temperatureMilliKelvin = 300000,
                                                  .surfacePressurePascals = 101325};
        const auto baked = bake(good);
        EXPECT(baked && baked->fractions.size() == 2 && baked->fractions[0].species < baked->fractions[1].species);

        auto shortSum = good;
        shortSum.fractions[0].partsPerMillion = 780000;
        const auto shortResult = bake(shortSum);
        EXPECT(!shortResult && Contains(shortResult.error(), "990000"));

        auto unknown = good;
        unknown.fractions[0].species = "helium";
        const auto unknownResult = bake(unknown);
        EXPECT(!unknownResult && Contains(unknownResult.error(), "helium"));

        auto noTemperature = good;
        noTemperature.temperatureMilliKelvin = 0;
        const auto noTemperatureResult = bake(noTemperature);
        EXPECT(!noTemperatureResult && Contains(noTemperatureResult.error(), "atmosphere_temperature_mk"));

        const auto onlyPressure = bake({.surfacePressurePascals = 101325});
        EXPECT(!onlyPressure && Contains(onlyPressure.error(), "成分"));

        const auto none = bake({});
        EXPECT(none && none->Empty());
    }

    // --- マニフェストの title ---

    void TestManifestTitle() {
        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        if (!sandbox)
            return;

        (*sandbox)->Seal();
        PackageSource titled{.name = "titled", .files = {}};
        titled.files.emplace("package.luau", "return { format = 1, title = \"見せる名前\", depends = { \"core\" } }");
        const auto manifest = ReadPackageManifest(**sandbox, titled);
        EXPECT(manifest && manifest->title == "見せる名前" && manifest->depends == Names{"core"});

        PackageSource broken{.name = "broken", .files = {}};
        broken.files.emplace("package.luau", "return { format = 1, title = 3 }");
        const auto brokenResult = ReadPackageManifest(**sandbox, broken);
        EXPECT(!brokenResult && Contains(brokenResult.error(), "title"));
    }

    // --- ホットリロードの読む元の差し替え(環境ミキサーのボタン)---

    void TestHotReloadReplace() {
        const auto catalog = Catalog();
        const auto blank = LoadBlankReactionTable();
        if (!catalog || !blank)
            return;

        ReactionTableSource source{.basePackage = {}, .readPackageRoot = false};
        ReactionTableHotReload reload(source, std::make_shared<const LoadedReactionTable>(*blank),
                                      SpeciesChangePolicy::Remap);

        // 足す: 物質が増える表を名前で付け替えて当てる
        const auto folders = MixFolders(*catalog, Names{"real_elements", "real_matter"});
        source.addedPackages = folders.value_or(std::vector<fs::path>{});
        const HotReloadResult added = reload.Replace(source);
        EXPECT(added.state == HotReloadState::Reloaded && added.table &&
               added.table->table.speciesNames.size() == 1 + 8);
        EXPECT(reload.Source().addedPackages.size() == 2);

        // 読めない読む元は落とし、読む元も表も元のまま
        ReactionTableSource broken = source;
        broken.addedPackages.push_back(fs::path(BICAMERAL_DATA_STANDARD_DIR) / "no_such_package");
        const HotReloadResult failed = reload.Replace(broken);
        EXPECT(failed.state == HotReloadState::Failed);
        EXPECT(reload.Source().addedPackages.size() == 2);
        EXPECT(reload.Current() == added.table);

        // 外す: 全部外すとブランクの表に戻る(物質を消す表も名前で付け替えて当てる)
        source.addedPackages.clear();
        const HotReloadResult removed = reload.Replace(source);
        EXPECT(removed.state == HotReloadState::Reloaded && removed.table &&
               removed.table->tableVersion == blank->tableVersion);
    }

}  // namespace

int main() {
    TestCatalog();
    TestMixAddRemove();
    TestEachPackageLoadsWithDependencies();
    TestFullMix();
    TestEmptyMixIsBlank();
    TestSampleWithMix();
    TestAtmosphereChecks();
    TestManifestTitle();
    TestHotReloadReplace();
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("standard_packages: すべて通過\n");

    return 0;
}
