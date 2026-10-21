// reaction_package_test.cpp — Luau の反応表 → 定義 → ベイク(T-0021、script/reaction_package・sim/reaction_table)を CPU だけで確かめる。
// 見るもの: 試験のパッケージ(data/packages/combustion_test。テストの試験の表 MakeCombustionTestTable もこれを読む。T-0169)が
//           ベイクできて物質と規則の数が合う / 定義の並び・A の書き方に依らない / 形の誤り(欄の綴り・小数・範囲)と
//           中身の誤り(元素の釣り合い・知らない物質)を落とす / 文献の反応熱の食い違いは警告 / Mod が物質と規則を足せる /
//           10 進の数の読み方。
//           (T-0157)ランタイムの入り口 LoadReactionTable: exe の横の data/packages(ビルドが写したもの)からリポジトリのパッケージとビットで同じ表を作る /
//           フォルダが無い・ゲーム本体が無い・ゲーム本体が読めない・Mod が形や検査で落ちる → 失敗(起動を止める)/
//           Luau として読めない Mod だけは除いて続け、rejected に残す。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "script/reaction_package.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "reaction_test_table.h"
#include "script/luau_sandbox.h"
#include "script/reaction_table_loader.h"
#include "sim/cell_view.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::script;
    using namespace bicameral::sim;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 道具 ---

    constexpr std::string_view BASE_PACKAGE = "combustion_test";

    // 試験のパッケージと、それが依存する標準パッケージ(T-0498)
    std::vector<PackageSource> BasePackages() {
        return sim::ReadCombustionTestPackages();
    }

    // 試験のパッケージに依存する Mod(init だけ)
    PackageSource ModPackage(std::string name, std::string init) {
        PackageSource package{.name = std::move(name), .files = {}};
        package.files.emplace("package.luau", "return { format = 1, depends = { \"combustion_test\" } }");
        package.files.emplace("init.luau", std::move(init));

        return package;
    }

    // パッケージ群 → 読み込み → 定義 → ベイク。読まなかったパッケージがあれば誤りにする
    std::expected<BakedReactionTable, std::string> BakePackages(const std::vector<PackageSource>& packages) {
        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        if (!sandbox)
            return std::unexpected(sandbox.error());

        (*sandbox)->Seal();
        const auto loaded = LoadPackages(**sandbox, packages,
                                         PackageLoadOptions{.basePackage = std::string(BASE_PACKAGE)});
        if (!loaded)
            return std::unexpected(loaded.error());

        if (!loaded->rejected.empty())
            return std::unexpected(loaded->rejected.front().package + ": " + loaded->rejected.front().reason);

        const auto definition = ReadReactionTableDefinition(*loaded);
        if (!definition)
            return std::unexpected(definition.error());

        return BakeReactionTable(*definition);
    }

    template <typename T>
    bool SameBytes(const std::vector<T>& left, const std::vector<T>& right) {
        return left.size() == right.size() &&
               (left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
    }

    // GPU に載せる配列(バイト)と、CPU だけの表(名前・モル質量・組成)が同じか
    bool SameTable(const BakedReactionTable& left, const BakedReactionTable& right) {
        const bool gpu = SameBytes(left.species, right.species) && SameBytes(left.rules, right.rules) &&
                         SameBytes(left.ruleIndex, right.ruleIndex) && SameBytes(left.rates, right.rates);
        const bool cpu = left.elementNames == right.elementNames && left.speciesNames == right.speciesNames &&
                         left.ruleNames == right.ruleNames && left.molarMasses == right.molarMasses &&
                         left.speciesElements == right.speciesElements;

        return gpu && cpu;
    }

    bool Contains(std::string_view text, std::string_view part) {
        if (text.find(part) != std::string_view::npos)
            return true;

        std::printf("  '%.*s' に '%.*s' が無い\n", static_cast<int>(text.size()), text.data(),
                    static_cast<int>(part.size()), part.data());

        return false;
    }

    // 坂の警告(T-0492。頭が SLOPE_WARNING_PREFIX)と、それ以外の警告(文献の反応熱との食い違いなど)の数
    size_t SlopeWarningCount(const BakedReactionTable& table) {
        return static_cast<size_t>(std::ranges::count_if(
            table.warnings, [](const std::string& warning) { return warning.starts_with(SLOPE_WARNING_PREFIX); }));
    }

    // 光の値の書き忘れの警告(T-0942。頭が LIGHT_WARNING_PREFIX)。標準パッケージの凝縮相は光の値を書いてある(出ない)
    size_t LightWarningCount(const BakedReactionTable& table) {
        return static_cast<size_t>(std::ranges::count_if(
            table.warnings, [](const std::string& warning) { return warning.starts_with(LIGHT_WARNING_PREFIX); }));
    }

    bool HasLightWarning(const BakedReactionTable& table, std::string_view part) {
        return std::ranges::any_of(table.warnings, [&](const std::string& warning) {
            return warning.starts_with(LIGHT_WARNING_PREFIX) && warning.find(part) != std::string::npos;
        });
    }

    // 坂以外の警告(文献の反応熱との食い違い・密度や光の値の書き忘れなど)
    std::vector<std::string> OtherWarnings(const BakedReactionTable& table) {
        std::vector<std::string> others;
        for (const std::string& warning : table.warnings) {
            if (!warning.starts_with(SLOPE_WARNING_PREFIX))
                others.push_back(warning);
        }

        return others;
    }

    size_t OtherWarningCount(const BakedReactionTable& table) {
        return OtherWarnings(table).size();
    }

    bool HasSlopeWarning(const BakedReactionTable& table, std::string_view part) {
        return std::ranges::any_of(table.warnings, [&](const std::string& warning) {
            return warning.starts_with(SLOPE_WARNING_PREFIX) && warning.find(part) != std::string::npos;
        });
    }

    uint32_t RuleId(const BakedReactionTable& table, std::string_view name) {
        return static_cast<uint32_t>(std::ranges::find(table.ruleNames, name) - table.ruleNames.begin());
    }

    void PrintWarnings(const BakedReactionTable& table) {
        for (const std::string& warning : table.warnings)
            std::printf("  警告: %s\n", warning.c_str());
    }

    // --- 試験のパッケージ(C++ の写しは T-0169 で消した。中身はパッケージだけ)---

    void TestShippedPackage() {
        const auto luau = BakePackages(BasePackages());
        if (!luau) {
            std::printf("FAILED: ベイクできない: %s\n", luau.error().c_str());
            ++failureCount;

            return;
        }

        // 使わない 0 番 + 標準パッケージの現実の物質 10(argon・液体の水・氷を含む。T-0498・T-0611)+ 仮の魔素の物質 1。
        // 規則は現実 9(水の相変化 4。T-0611)+ 仮の魔素 2(T-0225)
        EXPECT(luau->species.size() == 12 && luau->rules.size() == 11);
        EXPECT(luau->rates.size() == luau->rules.size() * reaction::RX_RATE_TABLE_KELVINS);
        PrintWarnings(*luau);
        EXPECT(OtherWarningCount(*luau) == 0);  // 文献の反応熱は生成エンタルピーの差と 1 kJ/mol 以内
        // 坂を上る規則は boudouard の組だけ(949 K 以下で ΔG° > 0。T-0491 の静的な一覧と同じ。T-0492)
        EXPECT(SlopeWarningCount(*luau) == 2 && HasSlopeWarning(*luau, "規則 boudouard ") &&
               HasSlopeWarning(*luau, "mana_test_catalyzed_boudouard"));
        std::printf("試験のパッケージ: 物質 %zu・規則 %zu・速度の表 %zu バイト\n", luau->species.size() - 1,
                    luau->rules.size(), luau->rates.size() * sizeof(uint64_t));
    }

    // --- 並びと書き方に依らない ---

    void TestOrderIndependent() {
        const ReactionTableDefinition original = MakeCombustionTestTable();
        ReactionTableDefinition shuffled = original;
        std::ranges::reverse(shuffled.elements);
        std::ranges::reverse(shuffled.species);
        std::ranges::reverse(shuffled.rules);
        for (RuleDefinition& rule : shuffled.rules) {
            std::ranges::reverse(rule.reactants);
            std::ranges::reverse(rule.products);
            // A = 3e8 → 30e7、1.5e9 → 150e7(同じ値の別の書き方)
            rule.rate.preExponentialMantissa *= 10;
            rule.rate.preExponentialExponent10 -= 1;
        }

        for (SpeciesDefinition& species : shuffled.species)
            std::ranges::reverse(species.composition);

        const auto left = BakeReactionTable(original);
        const auto right = BakeReactionTable(shuffled);
        EXPECT(left && right && SameTable(*left, *right));
    }

    // --- 誤りを落とす ---

    // 試験の表に Mod で物質 species と規則 reactions(Luau の表の中身)を足してベイクし、誤りの文に part を含むか
    void ExpectModFails(std::string_view label, std::string_view species, std::string_view reactions,
                        std::string_view part) {
        const std::string init = "return { species = { " + std::string(species) + " }, reactions = { " +
                                 std::string(reactions) + " } }";
        std::vector<PackageSource> packages = BasePackages();
        packages.push_back(ModPackage("broken", init));
        const auto baked = BakePackages(packages);
        if (baked) {
            std::printf("FAILED: %.*s を落とさない\n", static_cast<int>(label.size()), label.data());
            ++failureCount;

            return;
        }

        std::printf("  %.*s → %s\n", static_cast<int>(label.size()), label.data(), baked.error().c_str());
        EXPECT(Contains(baked.error(), part));
    }

    constexpr std::string_view HYDROGEN =
        "hydrogen = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = 28836, "
        "thermal_conductivity_mw_per_m_k = 900000 }";
    constexpr std::string_view RATE = "rate = { a = \"1e8\", activation_energy_j_per_mol = 200000 }";

    std::string WaterGas(std::string_view products, std::string_view extra) {
        return "water_gas = { reactants = { carbon = 1, water_vapor = 1 }, products = { " + std::string(products) +
               " }, " + std::string(RATE) + std::string(extra) + " }";
    }

    void TestRejectsBrokenTables() {
        const std::string good = WaterGas("carbon_monoxide = 1, hydrogen = 1", "");

        // 形の誤り(読むときに落とす)
        ExpectModFails("小数",
                       "hydrogen = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, "
                       "heat_capacity_mj_per_mol_k = 28.836, thermal_conductivity_mw_per_m_k = 900000 }",
                       good, "整数");
        ExpectModFails("欄の綴り",
                       "hydrogen = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, "
                       "heat_capacity = 28836, thermal_conductivity_mw_per_m_k = 900000 }",
                       good, "知らない欄");
        ExpectModFails("組成の 0",
                       "hydrogen = { composition = { H = 0 }, formation_enthalpy_j_per_mol = 0, "
                       "heat_capacity_mj_per_mol_k = 28836, thermal_conductivity_mw_per_m_k = 900000 }",
                       good, "範囲");
        ExpectModFails("速度が無い", HYDROGEN,
                       "water_gas = { reactants = { carbon = 1, water_vapor = 1 }, "
                       "products = { carbon_monoxide = 1, hydrogen = 1 } }",
                       "欄 rate がない");
        ExpectModFails("A の書き方", HYDROGEN,
                       "water_gas = { reactants = { carbon = 1 }, products = { carbon_monoxide = 1 }, "
                       "rate = { a = \"1e8x\", activation_energy_j_per_mol = 1 } }",
                       "10 進");
        ExpectModFails("反応物でない次数", HYDROGEN,
                       WaterGas("carbon_monoxide = 1, hydrogen = 1", ", orders = { hydrogen = 1 }"), "反応物ではない");
        ExpectModFails("次数 2", HYDROGEN, WaterGas("carbon_monoxide = 1, hydrogen = 1", ", orders = { carbon = 2 }"),
                       "範囲");
        ExpectModFails(
            "相の綴り(T-0609)",
            "hydrogen = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, "
            "heat_capacity_mj_per_mol_k = 28836, thermal_conductivity_mw_per_m_k = 900000, phase = \"plasma\" }",
            good, "phase");
        ExpectModFails("相と condensed の食い違い(T-0609)",
                       "hydrogen = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, "
                       "heat_capacity_mj_per_mol_k = 28836, thermal_conductivity_mw_per_m_k = 900000, phase = "
                       "\"liquid\", condensed = false }",
                       good, "食い違う");

        // 中身の誤り(ベイクが落とす)
        ExpectModFails(
            "密度の無い液体(T-0609)",
            "hydrogen = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, "
            "heat_capacity_mj_per_mol_k = 28836, thermal_conductivity_mw_per_m_k = 900000, phase = \"liquid\" }",
            good, "密度");
        ExpectModFails("元素の釣り合い", HYDROGEN, WaterGas("carbon_monoxide = 1, hydrogen = 2", ""), "釣り合わない");
        ExpectModFails("知らない物質", HYDROGEN, WaterGas("carbon_monoxide = 1, helium = 1", ""), "知らない物質");
        ExpectModFails("知らない元素",
                       "hydrogen = { composition = { X = 2 }, formation_enthalpy_j_per_mol = 0, "
                       "heat_capacity_mj_per_mol_k = 28836, thermal_conductivity_mw_per_m_k = 900000 }",
                       good, "知らない元素");
    }

    // --- Mod が足す・文献の反応熱 ---

    void TestModAddsSpeciesAndRule() {
        // 水性ガス反応 C + H2O → CO + H2(ΔH = +131.3 kJ/mol。速度は仮の値)
        const std::string init = "return { species = { " + std::string(HYDROGEN) + " }, reactions = { " +
                                 WaterGas("carbon_monoxide = 1, hydrogen = 1",
                                          ", reaction_enthalpy_j_per_mol = 131300") +
                                 " } }";
        std::vector<PackageSource> packages = BasePackages();
        packages.push_back(ModPackage("water_gas", init));
        const auto baked = BakePackages(packages);
        if (!baked) {
            std::printf("FAILED: Mod つきでベイクできない: %s\n", baked.error().c_str());
            ++failureCount;

            return;
        }

        PrintWarnings(*baked);
        EXPECT(baked->species.size() == 13 && baked->rules.size() == 12 && OtherWarningCount(*baked) == 0);
        // hydrogen に標準エントロピーを書いていないので water_gas の坂は調べられない(まとめて 1 行。表は当たる。T-0492)
        EXPECT(HasSlopeWarning(*baked, "坂を調べられない規則 1 本: water_gas") && HasSlopeWarning(*baked, "hydrogen"));
        EXPECT(baked->SpeciesId("hydrogen") != 0);
        EXPECT(std::ranges::find(baked->ruleNames, "water_gas") != baked->ruleNames.end());
    }

    void TestDeclaredEnthalpyWarning() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        const auto burn = std::ranges::find(definition.rules, "carbon_combustion", &RuleDefinition::name);
        burn->declaredReactionEnthalpy = -393000;  // 差 509 J/mol: 警告しない
        const auto close = BakeReactionTable(definition);
        EXPECT(close && OtherWarningCount(*close) == 0);

        burn->declaredReactionEnthalpy = -350000;  // 差 43.5 kJ/mol: 警告(ベイクは通す。表は生成エンタルピーの差)
        const auto far = BakeReactionTable(definition);
        EXPECT(far && OtherWarningCount(*far) == 1 && Contains(OtherWarnings(*far).front(), "carbon_combustion"));
        EXPECT(far && close && SameTable(*far, *close));
        if (far)
            PrintWarnings(*far);
    }

    // 凝縮相に密度を書き忘れるとベイクが警告する(表は当たる。T-0616)。標準パッケージの凝縮相は全部書いてある
    void TestMissingDensityWarning() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        EXPECT(std::ranges::all_of(definition.species, [](const SpeciesDefinition& species) {
            return species.phase != SpeciesPhase::Solid || species.density != 0;
        }));

        const auto cellulose = std::ranges::find(definition.species, "cellulose", &SpeciesDefinition::name);
        cellulose->density = 0;
        const auto missing = BakeReactionTable(definition);
        EXPECT(missing && OtherWarningCount(*missing) == 1 && Contains(OtherWarnings(*missing).front(), "cellulose") &&
               Contains(OtherWarnings(*missing).front(), "density_kg_per_m3"));
        if (missing)
            PrintWarnings(*missing);

        // 気体には書かなくてよい(警告しない)
        cellulose->density = 1500;
        const auto gas = std::ranges::find(definition.species, "oxygen", &SpeciesDefinition::name);
        EXPECT(gas->density == 0);
        const auto written = BakeReactionTable(definition);
        EXPECT(written && OtherWarningCount(*written) == 0);
    }

    // 光の値の書き忘れはベイクが警告する(表は当たる。T-0942・D-610・ADR-0395): 固体・液体で 1 つも書かない・片方の帯だけ書いた。
    // 気体は書かなくても透明が自然なので知らせない。標準パッケージの凝縮相(炭・セルロース・液体の水・氷)は光の値を書いてある(出ない)
    void TestMissingLightWarning() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        const auto standard = BakeReactionTable(definition);
        EXPECT(standard && LightWarningCount(*standard) == 0);

        // セルロースの光の値を全部消す: 1 行(灰色で通さないとして動かす)
        const auto cellulose = std::ranges::find(definition.species, "cellulose", &SpeciesDefinition::name);
        cellulose->lightAbsorptionShort.reset();
        cellulose->lightAbsorptionLong.reset();
        cellulose->lightReflectanceShortPermille.reset();
        cellulose->lightReflectanceLongPermille.reset();
        const auto unwritten = BakeReactionTable(definition);
        EXPECT(unwritten && LightWarningCount(*unwritten) == 1);
        EXPECT(unwritten && HasLightWarning(*unwritten, "物質 cellulose: 固体") &&
               HasLightWarning(*unwritten, "灰色で光を通さない") && !HasLightWarning(*unwritten, "oxygen") &&
               !HasLightWarning(*unwritten, "mana_test"));
        if (unwritten)
            PrintWarnings(*unwritten);

        // 両方の帯を書けば知らせない(照り返しは書かなくてよい: 吸い込みだけで光の値を書いた物質)
        cellulose->lightAbsorptionShort = 50000;
        cellulose->lightAbsorptionLong = 80000;
        const auto written = BakeReactionTable(definition);
        EXPECT(written && LightWarningCount(*written) == 0 && !HasLightWarning(*written, "cellulose"));

        // 片方の帯だけ: その欄の名前で 1 つ(もう片方も同じ値として動かす)
        cellulose->lightAbsorptionLong.reset();
        cellulose->lightReflectanceLongPermille = 300;
        const auto alone = BakeReactionTable(definition);
        EXPECT(alone && LightWarningCount(*alone) == 2 &&
               HasLightWarning(*alone, "cellulose: light_absorption_short_mm2_per_mol を片方の帯だけ") &&
               HasLightWarning(*alone, "cellulose: light_reflectance_long_permille を片方の帯だけ"));

        // 前の欄(radiation_absorption_mm2_per_mol)だけ: 光の値は無いので警告し、その値で動かすことを書く
        cellulose->lightAbsorptionShort.reset();
        cellulose->lightReflectanceLongPermille.reset();
        cellulose->radiationAbsorption = 900;
        const auto legacy = BakeReactionTable(definition);
        EXPECT(legacy && LightWarningCount(*legacy) == 1 &&
               HasLightWarning(*legacy, "radiation_absorption_mm2_per_mol = 900"));

        // 気体: 1 つも書かないのは知らせない(透明)が、片方の帯だけ書いたら知らせる(もう片方も同じ値になるので)
        const auto oxygen = std::ranges::find(definition.species, "oxygen", &SpeciesDefinition::name);
        oxygen->lightAbsorptionLong = 1;
        const auto gas = BakeReactionTable(definition);
        EXPECT(gas && HasLightWarning(*gas, "oxygen: light_absorption_long_mm2_per_mol を片方の帯だけ") &&
               LightWarningCount(*gas) == 2);
    }

    // --- 10 進の数 ---

    bool IsDecimal(std::string_view text, uint64_t mantissa, int32_t exponent10) {
        const auto parsed = ParseDecimal(text);
        if (parsed && parsed->mantissa == mantissa && parsed->exponent10 == exponent10)
            return true;

        std::printf(
            "  \"%.*s\" → %s\n", static_cast<int>(text.size()), text.data(),
            parsed ? std::to_string(parsed->mantissa).append("e").append(std::to_string(parsed->exponent10)).c_str()
                   : parsed.error().c_str());

        return false;
    }

    // 坂を上る規則はベイクが警告し、表はそのまま当てる。GPU の表は物質のエントロピーに依らない(T-0492・D-487 ②)
    // --- 相と動き方の欄(T-0710・土台の決定 3)---

    // 試験のパッケージに物質を足してベイクする
    std::expected<BakedReactionTable, std::string> BakeWithSpecies(std::string_view species) {
        std::vector<PackageSource> packages = BasePackages();
        packages.push_back(ModPackage("phase_mod", "return { species = { " + std::string(species) + " } }"));

        return BakePackages(packages);
    }

    // 光の値の書き忘れ(T-0942)は除いて探す(この節の試験の物質は光の値を書かないので、名前で探すと必ず当たる)
    bool HasWarning(const BakedReactionTable& table, std::string_view part) {
        return std::ranges::any_of(table.warnings, [&](const std::string& warning) {
            return !warning.starts_with(LIGHT_WARNING_PREFIX) && warning.find(part) != std::string::npos;
        });
    }

    // 密度を書いた試験の物質(相と動き方の欄は fields で足す)
    std::string Stone(std::string_view fields) {
        return "test_stone = { composition = { C = 1 }, formation_enthalpy_j_per_mol = 0, "
               "heat_capacity_mj_per_mol_k = 30000, thermal_conductivity_mw_per_m_k = 2000, density_kg_per_m3 = "
               "2000, " +
               std::string(fields) + " }";
    }

    // condensed(廃止)で書いた表と phase で書いた表は、ベイクの結果(GPU の表・相の表)が同じ。condensed だけが警告される
    void TestLegacyCondensed() {
        const auto legacy = BakeWithSpecies(Stone("condensed = true"));
        const auto solid = BakeWithSpecies(Stone("phase = \"solid\""));
        EXPECT(legacy && solid);
        if (!legacy || !solid)
            return;

        PrintWarnings(*legacy);
        EXPECT(SameTable(*legacy, *solid));
        EXPECT(legacy->speciesPhase == solid->speciesPhase && legacy->speciesCondensed == solid->speciesCondensed &&
               legacy->speciesLiquid == solid->speciesLiquid &&
               legacy->speciesMolarVolume == solid->speciesMolarVolume);
        EXPECT(solid->speciesPhase[solid->SpeciesId("test_stone")] == SpeciesPhase::Solid);
        EXPECT(HasWarning(*legacy, "condensed は廃止") && !HasWarning(*solid, "condensed"));

        // condensed = false は気体(phase を書かないのと同じ)
        const auto gas = BakeWithSpecies(Stone("condensed = false"));
        const auto unwritten = BakeWithSpecies(Stone(""));
        EXPECT(gas && unwritten && SameTable(*gas, *unwritten) && gas->speciesPhase == unwritten->speciesPhase);
        EXPECT(gas && gas->speciesPhase[gas->SpeciesId("test_stone")] == SpeciesPhase::Gas);
    }

    // 標準パッケージの相の表は condensed を phase に直す前と同じ(凝縮相 = 炭・セルロース・氷・水、液体 = 水)と、見本の動き方の値
    void TestShippedPhases() {
        const auto baked = BakePackages(BasePackages());
        if (!baked) {
            std::printf("FAILED: ベイクできない: %s\n", baked.error().c_str());
            ++failureCount;

            return;
        }

        EXPECT(baked->speciesPhase.size() == baked->species.size() &&
               baked->speciesTransport.size() == baked->species.size());
        EXPECT(baked->speciesPhase[0] == SpeciesPhase::Gas);
        for (uint32_t id = 1; id < baked->speciesPhase.size(); ++id) {
            const std::string& name = baked->speciesNames[id];
            const bool solid = name == "carbon" || name == "cellulose" || name == "ice";
            const bool liquid = name == "water";
            const SpeciesPhase expected = solid    ? SpeciesPhase::Solid
                                          : liquid ? SpeciesPhase::Liquid
                                                   : SpeciesPhase::Gas;
            EXPECT(baked->speciesPhase[id] == expected);
            EXPECT(baked->speciesCondensed[id] == (solid || liquid ? 1 : 0));
            EXPECT(baked->speciesLiquid[id] == (liquid ? 1 : 0));
        }

        EXPECT(!HasWarning(*baked, "condensed") && !HasWarning(*baked, "粘り"));
        const SpeciesTransport& water = baked->speciesTransport[baked->SpeciesId("water")];
        const SpeciesTransport& vapor = baked->speciesTransport[baked->SpeciesId("water_vapor")];
        EXPECT(water.viscosityMicroPascalSeconds == 890 && vapor.diffusivityMm2PerS == 25);
    }

    // 動き方の 4 つの欄: 整数で持つ形(孔は nm²)・相に合わない欄と範囲の外は誤り・液体の粘りの書き忘れは警告
    void TestTransportFields() {
        const auto read = BakeWithSpecies(
            Stone("phase = \"solid\", permeability_um2 = \"1e-6\", suction_pa = 5000") +
            ", test_clay = { composition = { C = 1 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = "
            "30000, thermal_conductivity_mw_per_m_k = 2000, density_kg_per_m3 = 2000, phase = \"solid\", "
            "permeability_um2 = \"0.001\" }, test_gravel = { composition = { C = 1 }, formation_enthalpy_j_per_mol = "
            "0, "
            "heat_capacity_mj_per_mol_k = 30000, thermal_conductivity_mw_per_m_k = 2000, density_kg_per_m3 = 2000, "
            "phase = \"solid\", permeability_um2 = 10 }, test_oil = { composition = { C = 1 }, "
            "formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = 30000, thermal_conductivity_mw_per_m_k = "
            "200, phase = \"liquid\", density_kg_per_m3 = 900, viscosity_upa_s = 50000 }, test_mist = { composition = "
            "{ C = 1 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = 30000, "
            "thermal_conductivity_mw_per_m_k = 20, diffusivity_mm2_per_s = 5 }");
        EXPECT(read.has_value());
        if (!read) {
            std::printf("  %s\n", read.error().c_str());

            return;
        }

        const auto transport = [&](std::string_view name) {
            return read->speciesTransport[read->SpeciesId(name)];
        };
        EXPECT(transport("test_stone").permeabilityNm2 == 1 && transport("test_stone").suctionPascals == 5000);
        EXPECT(transport("test_clay").permeabilityNm2 == 1000 && !transport("test_clay").suctionPascals);
        EXPECT(transport("test_gravel").permeabilityNm2 == 10000000);
        EXPECT(transport("test_oil").viscosityMicroPascalSeconds == 50000);
        EXPECT(transport("test_mist").diffusivityMm2PerS == 5 && !transport("test_mist").permeabilityNm2);
        EXPECT(!HasWarning(*read, "test_oil"));

        // 液体の粘りの書き忘れは警告(表は当たる)
        const auto thin = BakeWithSpecies(
            "test_oil = { composition = { C = 1 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = "
            "30000, "
            "thermal_conductivity_mw_per_m_k = 200, phase = \"liquid\", density_kg_per_m3 = 900 }");
        EXPECT(thin && HasWarning(*thin, "test_oil") && HasWarning(*thin, "viscosity_upa_s"));

        // 相に合わない欄・範囲の外は誤り
        ExpectModFails("気体に孔", Stone("permeability_um2 = 10"), "", "permeability_um2");
        ExpectModFails("気体に吸い上げ", Stone("suction_pa = 10"), "", "suction_pa");
        ExpectModFails("固体に粘り", Stone("phase = \"solid\", viscosity_upa_s = 1000"), "", "viscosity_upa_s");
        ExpectModFails("固体に混ざる速さ", Stone("phase = \"solid\", diffusivity_mm2_per_s = 5"), "",
                       "diffusivity_mm2_per_s");
        ExpectModFails("細かすぎる孔", Stone("phase = \"solid\", permeability_um2 = \"1e-7\""), "", "1e-6 µm² 刻み");
        ExpectModFails("大きすぎる孔", Stone("phase = \"solid\", permeability_um2 = \"1e8\""), "", "1e-6 µm² 刻み");
        ExpectModFails("孔の小数", Stone("phase = \"solid\", permeability_um2 = 0.5"), "", "整数");
        ExpectModFails("粘り 0", Stone("phase = \"liquid\", viscosity_upa_s = 0"), "", "範囲");
    }

    // --- 固体の動き方(T-0819・ADR-0359 §1・§2)---

    // 試験の固体(密度 density。欄は fields で足す。density が 0 なら密度を書かない)
    std::string Grain(std::string_view name, uint32_t density, std::string_view fields) {
        const std::string densityField = density == 0 ? "" : std::format("density_kg_per_m3 = {}, ", density);

        return std::string(name) +
               " = { composition = { C = 1 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = 30000, "
               "thermal_conductivity_mw_per_m_k = 2000, phase = \"solid\", " +
               densityField + std::string(fields) + " }";
    }

    bool SamePhases(const BakedReactionTable& left, const BakedReactionTable& right) {
        return SameBytes(MakeSpeciesPhases(left), MakeSpeciesPhases(right));
    }

    // 読んだ形と相の表(動く 1 bit・安息角の tan θ の Q16)。"fixed" と書いた表・書かない表は同じ表と同じ相の表
    void TestSolidMotionRead() {
        const auto read = BakeWithSpecies(Grain("test_sand", 1600, "solid_motion = \"loose\", repose_angle_deg = 34") +
                                          ", " +
                                          Grain("test_ice", 917, "solid_motion = \"loose\", repose_angle_deg = 90") +
                                          ", " + Grain("test_rock", 2600, "solid_motion = \"fixed\""));
        EXPECT(read.has_value());
        if (!read) {
            std::printf("  %s\n", read.error().c_str());

            return;
        }

        const auto transport = [&](std::string_view name) {
            return read->speciesTransport[read->SpeciesId(name)];
        };
        EXPECT(transport("test_sand").solidMotion == SolidMotion::Loose &&
               transport("test_sand").reposeAngleDegrees == 34);
        EXPECT(transport("test_ice").reposeAngleDegrees == 90);
        EXPECT(transport("test_rock").solidMotion == SolidMotion::Fixed && !transport("test_rock").reposeAngleDegrees);
        EXPECT(!HasWarning(*read, "test_sand") && !HasWarning(*read, "test_ice") && !HasWarning(*read, "test_rock"));

        // 相の表: 相は固体のまま・ばらけるは 1 bit・tan 34° = 0.6745 → 44205(Q16)・90° は横に崩れない
        const std::vector<multires::CvSpeciesPhase> phases = MakeSpeciesPhases(*read);
        const multires::CvSpeciesPhase sand = phases[read->SpeciesId("test_sand")];
        const multires::CvSpeciesPhase ice = phases[read->SpeciesId("test_ice")];
        const multires::CvSpeciesPhase rock = phases[read->SpeciesId("test_rock")];
        EXPECT(sand.phase == multires::CV_PHASE_SOLID && sand.motion == multires::CV_MOTION_LOOSE &&
               sand.reposeSlopeQ16 == 44205);
        EXPECT(ice.motion == multires::CV_MOTION_LOOSE && ice.reposeSlopeQ16 == multires::CV_REPOSE_VERTICAL);
        EXPECT(rock.phase == multires::CV_PHASE_SOLID && rock.motion == multires::CV_MOTION_FIXED &&
               rock.reposeSlopeQ16 == 0);

        // "fixed" と書く = 書かない(反応の表・相の表がバイト単位で同じ。警告も無い)
        const auto fixedWritten = BakeWithSpecies(Grain("test_rock", 2600, "solid_motion = \"fixed\""));
        const auto unwritten = BakeWithSpecies(Grain("test_rock", 2600, ""));
        EXPECT(fixedWritten && unwritten && SameTable(*fixedWritten, *unwritten) &&
               SamePhases(*fixedWritten, *unwritten) && fixedWritten->warnings == unwritten->warnings);

        // 標準パッケージ(固体の欄を書かない)の相の表は全部据わる・傾き 0
        const auto shipped = BakePackages(BasePackages());
        EXPECT(shipped.has_value());
        if (shipped) {
            for (const multires::CvSpeciesPhase& phase : MakeSpeciesPhases(*shipped))
                EXPECT(phase.motion == multires::CV_MOTION_FIXED && phase.reposeSlopeQ16 == 0);
        }
    }

    // 警告 2 つ(表は当たる)と誤り 2 つ + 書き方の誤り(ADR-0359 §1。D-487 ②)
    void TestSolidMotionChecks() {
        // ばらける固体の安息角の書き忘れ → 35 度として警告
        const auto defaulted = BakeWithSpecies(Grain("test_sand", 1600, "solid_motion = \"loose\""));
        EXPECT(defaulted && HasWarning(*defaulted, "test_sand") && HasWarning(*defaulted, "repose_angle_deg") &&
               HasWarning(*defaulted, "35 度"));
        if (defaulted)
            EXPECT(MakeSpeciesPhases(*defaulted)[defaulted->SpeciesId("test_sand")].reposeSlopeQ16 == 45889);

        // 据わる固体の安息角 → 効かないと警告(相の表は据わる・傾き 0)
        const auto ignored = BakeWithSpecies(Grain("test_rock", 2600, "repose_angle_deg = 30"));
        EXPECT(ignored && HasWarning(*ignored, "test_rock") && HasWarning(*ignored, "だけに効く"));
        if (ignored) {
            const multires::CvSpeciesPhase rock = MakeSpeciesPhases(*ignored)[ignored->SpeciesId("test_rock")];
            EXPECT(rock.motion == multires::CV_MOTION_FIXED && rock.reposeSlopeQ16 == 0);
        }

        // 誤り: 固体の外の固体の欄・密度の無いばらける固体・知らない値・範囲の外
        ExpectModFails("気体に固体の動き方", Stone("solid_motion = \"loose\""), "", "solid_motion");
        ExpectModFails("液体に安息角", Stone("phase = \"liquid\", viscosity_upa_s = 1000, repose_angle_deg = 30"), "",
                       "repose_angle_deg");
        ExpectModFails("密度の無いばらける固体",
                       Grain("test_dust", 0, "solid_motion = \"loose\", repose_angle_deg = 30"), "",
                       "density_kg_per_m3");
        ExpectModFails("知らない動き方", Grain("test_sand", 1600, "solid_motion = \"granular\""), "", "solid_motion");
        ExpectModFails("安息角 91", Grain("test_sand", 1600, "solid_motion = \"loose\", repose_angle_deg = 91"), "",
                       "範囲");
        ExpectModFails("安息角の小数", Grain("test_sand", 1600, "solid_motion = \"loose\", repose_angle_deg = 34.5"),
                       "", "整数");
    }

    void TestSlopeWarnings() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        const auto with = BakeReactionTable(definition);
        EXPECT(with && with->ruleSlopes.size() == with->rules.size());
        if (!with)
            return;

        const RuleSlope& boudouard = with->ruleSlopes[RuleId(*with, "boudouard")];
        EXPECT(boudouard.state == RuleSlopeState::Uphill);
        EXPECT(boudouard.balanceKelvin > 900 &&
               boudouard.balanceKelvin < 1000);  // 比熱一定の近似で 949 K(文献 約 980 K)
        EXPECT(boudouard.uphill.highKelvin + 1 == boudouard.balanceKelvin);
        EXPECT(boudouard.gibbs300MilliJoules > 0 && boudouard.gibbs1000MilliJoules < 0);
        EXPECT(with->ruleSlopes[RuleId(*with, "cellulose_combustion")].state == RuleSlopeState::Downhill);
        // 触媒(mana_test)は両辺で消えるので、エントロピーが無くても調べられる
        EXPECT(with->ruleSlopes[RuleId(*with, "mana_test_catalyzed_boudouard")].state == RuleSlopeState::Uphill);

        // 釣り合いの規則(相変化。T-0623・ADR-0290): ベイクが作る逆向き(~逆)と対になり、坂の欄は 1 本ずつ上る温度があるが
        // 一方向ではないので警告しない。書いた釣り合いは物質の値の釣り合いと 10 倍以内(BalanceWarning が出ない)
        const RuleSlope& evaporation = with->ruleSlopes[RuleId(*with, "water_evaporation")];
        EXPECT(evaporation.state == RuleSlopeState::Uphill && evaporation.balanceKelvin == 374);
        EXPECT(evaporation.reverseRule == RuleId(*with, "water_evaporation~逆"));
        EXPECT(with->ruleSlopes[RuleId(*with, "water_evaporation~逆")].reverseRule ==
               RuleId(*with, "water_evaporation"));
        EXPECT(with->ruleSlopes[RuleId(*with, "ice_melting")].reverseRule == RuleId(*with, "ice_melting~逆"));
        EXPECT(boudouard.reverseRule == NO_REVERSE_RULE);
        EXPECT(!HasSlopeWarning(*with, "water_") && !HasSlopeWarning(*with, "ice_"));

        // 一酸化炭素のエントロピーを消す: それを含む規則だけ未評価になり、まとめの 1 行に出る
        auto monoxide = std::ranges::find(definition.species, "carbon_monoxide", &SpeciesDefinition::name);
        monoxide->standardEntropy.reset();
        const auto partial = BakeReactionTable(definition);
        EXPECT(partial && SameTable(*with, *partial));
        if (partial) {
            EXPECT(partial->ruleSlopes[RuleId(*partial, "boudouard")].state == RuleSlopeState::Unevaluated);
            EXPECT(partial->ruleSlopes[RuleId(*partial, "carbon_partial_combustion")].missingEntropy ==
                   std::vector<std::string>{"carbon_monoxide"});
            EXPECT(SlopeWarningCount(*partial) == 1 && HasSlopeWarning(*partial, "3 本") &&
                   HasSlopeWarning(*partial, "carbon_monoxide"));
            PrintWarnings(*partial);
        }

        // 全部消す: 表は同じで、坂の警告はまとめの 1 行だけ(読み込みは断らない)
        for (SpeciesDefinition& species : definition.species)
            species.standardEntropy.reset();

        const auto without = BakeReactionTable(definition);
        EXPECT(without && SameTable(*with, *without) && SlopeWarningCount(*without) == 1);
    }

    void TestParseDecimal() {
        EXPECT(IsDecimal("2.8e19", 28, 18));
        EXPECT(IsDecimal("280e17", 28, 18));
        EXPECT(IsDecimal("28E+18", 28, 18));
        EXPECT(IsDecimal("0.5", 5, -1));
        EXPECT(IsDecimal("3", 3, 0));
        EXPECT(IsDecimal("1e-3", 1, -3));
        EXPECT(IsDecimal("007.0100", 701, -2));
        EXPECT(IsDecimal("1234567890123456789", 1234567890123456789ull, 0));
        for (const std::string_view broken :
             {"", "e5", "1.2.3", "-1", "0", "0.000", "1e", "1e99999", "1e1000", "abc", "12345678901234567891"})
            EXPECT(!ParseDecimal(broken).has_value());
    }

    // --- ランタイムの入り口(T-0157)---

    void TestLoaderReadsShippedTable() {
        const auto loaded = LoadReactionTable({});
        const auto source = BakePackages(BasePackages());
        if (!loaded || !source) {
            std::printf("FAILED: data/packages を読めない: %s\n", loaded ? "" : loaded.error().c_str());
            ++failureCount;

            return;
        }

        EXPECT(SameTable(loaded->table, *source));
        EXPECT(loaded->packageRoot == DefaultPackageRoot());
        // 依存の標準パッケージを棚から読み、最後にゲーム本体(T-0498)。ゲーム本体が現実の物質を書き直しても印は付かない
        EXPECT((loaded->loadOrder == std::vector<std::string>{"real_elements", "real_matter", "earth_environment",
                                                              "real_reactions", std::string(BASE_PACKAGE)}));
        EXPECT(loaded->rejected.empty() && !loaded->overrides.empty() && !loaded->modifiedWorld);
        EXPECT(loaded->tableVersion != 0);
        std::printf("ランタイムの表: %s(版 %016llx。リポジトリのパッケージとビットで同じ)\n",
                    reinterpret_cast<const char*>(loaded->packageRoot.generic_u8string().c_str()),
                    static_cast<unsigned long long>(loaded->tableVersion));
    }

    // ディスクにパッケージを書く(files: 相対パス → ソース)
    void WritePackage(const fs::path& folder, const std::map<std::string, std::string>& files) {
        fs::create_directories(folder);
        for (const auto& [path, source] : files)
            std::ofstream(folder / path, std::ios::binary) << source;
    }

    // ゲーム本体(data/packages/combustion_test の写し)+ Mod を root に置く
    void WriteBaseAndMod(const fs::path& root, std::string_view mod, std::string init) {
        fs::create_directories(root);
        fs::copy(fs::path(BICAMERAL_DATA_PACKAGES_DIR) / BASE_PACKAGE, root / BASE_PACKAGE,
                 fs::copy_options::recursive);
        WritePackage(root / mod, {{"package.luau", "return { format = 1, depends = { \"combustion_test\" } }"},
                                  {"init.luau", std::move(init)}});
    }

    void ExpectLoadFails(std::string_view label, const fs::path& root, std::string_view part) {
        const auto loaded = LoadReactionTable({.packageRoot = root});
        if (loaded) {
            std::printf("FAILED: %.*s で止まらない\n", static_cast<int>(label.size()), label.data());
            ++failureCount;

            return;
        }

        std::printf("  %.*s → %s\n", static_cast<int>(label.size()), label.data(), loaded.error().c_str());
        EXPECT(Contains(loaded.error(), part));
        EXPECT(Contains(loaded.error(), "反応表のパッケージ"));
    }

    void TestLoaderFailures() {
        const fs::path root = fs::temp_directory_path() / "bicameral_reaction_table_loader_test";
        std::error_code ignored;
        fs::remove_all(root, ignored);

        ExpectLoadFails("フォルダが無い", root / "missing", "読めない");

        fs::create_directories(root / "empty");
        ExpectLoadFails("ゲーム本体が無い", root / "empty", "見つからない");

        WritePackage(root / "syntax" / BASE_PACKAGE,
                     {{"package.luau", "return { format = 1, game = true }"}, {"init.luau", "return {"}});
        ExpectLoadFails("ゲーム本体が Luau として読めない", root / "syntax",
                        "ゲーム本体のパッケージ combustion_test を読めない");

        const std::string shapeError =
            "return { species = { hydrogen = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, "
            "heat_capacity_mj_per_mol_k = 28.836, thermal_conductivity_mw_per_m_k = 900000 } } }";
        WriteBaseAndMod(root / "shape", "broken_shape", shapeError);
        ExpectLoadFails("Mod の形の誤り", root / "shape", "整数");

        const std::string unbalanced = "return { species = { " + std::string(HYDROGEN) + " }, reactions = { " +
                                       WaterGas("carbon_monoxide = 1, hydrogen = 2", "") + " } }";
        WriteBaseAndMod(root / "balance", "broken_balance", unbalanced);
        ExpectLoadFails("Mod が元素の釣り合いの検査で落ちる", root / "balance", "ベイクの検査で落ちた");

        // Luau として読めない Mod は除いて続ける(ADR-0031 の 3)。表はゲーム本体だけのもの
        WriteBaseAndMod(root / "skip", "broken_syntax", "return {");
        const auto skipped = LoadReactionTable({.packageRoot = root / "skip"});
        const auto base = BakePackages(BasePackages());
        EXPECT(skipped && base && SameTable(skipped->table, *base));
        EXPECT(skipped && skipped->rejected.size() == 1 && skipped->rejected.front().package == "broken_syntax");

        fs::remove_all(root, ignored);
    }

}  // namespace

int main() {
    TestShippedPackage();
    TestOrderIndependent();
    TestRejectsBrokenTables();
    TestModAddsSpeciesAndRule();
    TestDeclaredEnthalpyWarning();
    TestMissingDensityWarning();
    TestMissingLightWarning();
    TestLegacyCondensed();
    TestShippedPhases();
    TestTransportFields();
    TestSolidMotionRead();
    TestSolidMotionChecks();
    TestSlopeWarnings();
    TestParseDecimal();
    TestLoaderReadsShippedTable();
    TestLoaderFailures();
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("reaction_package: すべて通過\n");

    return 0;
}
