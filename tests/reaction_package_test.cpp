// reaction_package_test.cpp — Luau の反応表 → 定義 → ベイク(T-0021、script/reaction_package・sim/reaction_table)を CPU だけで確かめる。
// 見るもの: 試験のパッケージ(data/packages/combustion_test)をベイクすると、C++ の試験の表(MakeCombustionTestTable)と
//           GPU に載せる配列がビットで同じ / 定義の並び・A の書き方に依らない / 形の誤り(欄の綴り・小数・範囲)と
//           中身の誤り(元素の釣り合い・知らない物質)を落とす / 文献の反応熱の食い違いは警告 / Mod が物質と規則を足せる /
//           10 進の数の読み方。
//           (T-0157)ランタイムの入り口 LoadReactionTable: exe の横の data/packages(ビルドが写したもの)から C++ の表とビットで同じ表を作る /
//           フォルダが無い・ゲーム本体が無い・ゲーム本体が読めない・Mod が形や検査で落ちる → 失敗(起動を止める)/
//           Luau として読めない Mod だけは除いて続け、rejected に残す。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "script/reaction_package.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "script/luau_sandbox.h"
#include "script/reaction_table_loader.h"
#include "sim/reaction_test_table.h"

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

    std::vector<PackageSource> BasePackages() {
        const fs::path folder = fs::path(BICAMERAL_DATA_PACKAGES_DIR) / BASE_PACKAGE;
        auto package = ReadPackageFolder(folder);
        if (!package) {
            std::printf("試験のパッケージを読めない: %s\n", package.error().c_str());
            ++failureCount;

            return {};
        }

        return {std::move(*package)};
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

    void PrintWarnings(const BakedReactionTable& table) {
        for (const std::string& warning : table.warnings)
            std::printf("  警告: %s\n", warning.c_str());
    }

    // --- 試験の表と同じ ---

    void TestSameAsCppTable() {
        const auto luau = BakePackages(BasePackages());
        const auto cpp = BakeReactionTable(MakeCombustionTestTable());
        if (!luau || !cpp) {
            std::printf("FAILED: ベイクできない: %s / %s\n", luau ? "" : luau.error().c_str(),
                        cpp ? "" : cpp.error().c_str());
            ++failureCount;

            return;
        }

        EXPECT(SameTable(*luau, *cpp));
        EXPECT(luau->species.size() == 8 && luau->rules.size() == 5);
        EXPECT(luau->rates.size() == luau->rules.size() * reaction::RX_RATE_TABLE_KELVINS);
        PrintWarnings(*luau);
        EXPECT(luau->warnings.empty() && cpp->warnings.empty());  // 文献の反応熱は生成エンタルピーの差と 1 kJ/mol 以内
        std::printf("試験のパッケージ: 物質 %zu・規則 %zu・速度の表 %zu バイト(C++ の表とビットで同じ)\n",
                    luau->species.size() - 1, luau->rules.size(), luau->rates.size() * sizeof(uint64_t));
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

        // 中身の誤り(ベイクが落とす)
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
        EXPECT(baked->species.size() == 9 && baked->rules.size() == 6 && baked->warnings.empty());
        EXPECT(baked->SpeciesId("hydrogen") != 0);
        EXPECT(std::ranges::find(baked->ruleNames, "water_gas") != baked->ruleNames.end());
    }

    void TestDeclaredEnthalpyWarning() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        const auto burn = std::ranges::find(definition.rules, "carbon_combustion", &RuleDefinition::name);
        burn->declaredReactionEnthalpy = -393000;  // 差 509 J/mol: 警告しない
        const auto close = BakeReactionTable(definition);
        EXPECT(close && close->warnings.empty());

        burn->declaredReactionEnthalpy = -350000;  // 差 43.5 kJ/mol: 警告(ベイクは通す。表は生成エンタルピーの差)
        const auto far = BakeReactionTable(definition);
        EXPECT(far && far->warnings.size() == 1 && Contains(far->warnings.front(), "carbon_combustion"));
        EXPECT(far && close && SameTable(*far, *close));
        if (far)
            PrintWarnings(*far);
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
        const auto cpp = BakeReactionTable(MakeCombustionTestTable());
        if (!loaded || !cpp) {
            std::printf("FAILED: data/packages を読めない: %s\n", loaded ? "" : loaded.error().c_str());
            ++failureCount;

            return;
        }

        EXPECT(SameTable(loaded->table, *cpp));
        EXPECT(loaded->packageRoot == DefaultPackageRoot());
        EXPECT(loaded->loadOrder == std::vector<std::string>{std::string(BASE_PACKAGE)});
        EXPECT(loaded->rejected.empty() && loaded->overrides.empty() && !loaded->modifiedWorld);
        EXPECT(loaded->tableVersion != 0);
        std::printf("ランタイムの表: %s(版 %016llx。C++ の表とビットで同じ)\n",
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
                     {{"package.luau", "return { format = 1 }"}, {"init.luau", "return {"}});
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
        const auto cpp = BakeReactionTable(MakeCombustionTestTable());
        EXPECT(skipped && cpp && SameTable(skipped->table, *cpp));
        EXPECT(skipped && skipped->rejected.size() == 1 && skipped->rejected.front().package == "broken_syntax");

        fs::remove_all(root, ignored);
    }

}  // namespace

int main() {
    TestSameAsCppTable();
    TestOrderIndependent();
    TestRejectsBrokenTables();
    TestModAddsSpeciesAndRule();
    TestDeclaredEnthalpyWarning();
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
