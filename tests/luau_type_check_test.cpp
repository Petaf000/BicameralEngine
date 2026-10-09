// luau_type_check_test.cpp — script/luau_type_check(読み込む前の型検査。T-0140、13 §2・ADR-0046)を CPU だけで確かめる。
// 見るもの: 型の定義(data/types/bicameral.d.luau)が読めて、試験の表のパッケージ(data/packages/combustion_test)が通る /
//           誤りが「どのファイルの何行目・何が違うか」で出る(戻り値の形・型を付けた表の欄・知らない名前・殻で見えないもの・
//           require の先・文法・マニフェスト)/ LoadPackages が誤りのあるパッケージ(と依存するもの)を読まない /
//           決定性(同じ検査器で 2 回・作り直した検査器・間にほかのパッケージを挟んでも、同じ診断の列)/ 型の定義の誤り。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。診断は全部表示する(誤りの文の確認用)。
#include "script/luau_type_check.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "script/luau_package.h"
#include "script/luau_sandbox.h"

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

    // --- 道具 ---

    const fs::path DATA_DIR = BICAMERAL_DATA_DIR;

    PackageSource MakePackage(std::string name, std::initializer_list<std::pair<std::string, std::string>> files) {
        PackageSource package{.name = std::move(name), .files = {}};
        for (const auto& [path, source] : files)
            package.files.emplace(path, source);

        return package;
    }

    // マニフェストと init だけのパッケージ
    PackageSource InitPackage(std::string name, std::string init) {
        return MakePackage(std::move(name),
                           {{"package.luau", "return { format = 1 }"}, {"init.luau", std::move(init)}});
    }

    std::unique_ptr<LuauTypeChecker> MakeChecker() {
        auto checker = CreateTypeCheckerFromFile(DATA_DIR / "types" / "bicameral.d.luau");
        if (!checker) {
            std::printf("型の定義を読めない: %s\n", checker.error().c_str());
            ++failureCount;

            return nullptr;
        }

        return std::move(*checker);
    }

    void Print(const char* title, const std::vector<TypeDiagnostic>& diagnostics) {
        std::printf("[%s] %zu 件\n", title, diagnostics.size());
        for (const TypeDiagnostic& diagnostic : diagnostics)
            std::printf("  %s\n", FormatDiagnostic(diagnostic).c_str());
    }

    // file・line・kind(の一部)が合う診断があるか
    bool Has(const std::vector<TypeDiagnostic>& diagnostics, std::string_view file, uint32_t line,
             std::string_view kindPart) {
        return std::ranges::any_of(diagnostics, [&](const TypeDiagnostic& diagnostic) {
            return diagnostic.file == file && diagnostic.line == line &&
                   diagnostic.kind.find(kindPart) != std::string::npos;
        });
    }

    // --- 試験の表のパッケージが通る ---

    void TestRealPackagePasses(LuauTypeChecker& checker) {
        const auto package = ReadPackageFolder(DATA_DIR / "packages" / "combustion_test");
        EXPECT(package.has_value());
        if (!package)
            return;

        const auto manifest = checker.CheckManifest(*package);
        const auto all = checker.CheckPackage(*package, "init");
        Print("combustion_test の manifest", manifest);
        Print("combustion_test", all);
        EXPECT(manifest.empty());
        EXPECT(all.empty());
    }

    // --- 誤りの場所と種類 ---

    void TestReturnShape(LuauTypeChecker& checker) {
        // species の欄が文字列(返す表の形の誤り → init.luau の return の行)
        const auto package = InitPackage("bad",
                                         "-- 1 行目\n"
                                         "local x = 1\n"
                                         "return {\n"
                                         "    species = { water = { composition = { H = 2, O = 1 },\n"
                                         "        formation_enthalpy_j_per_mol = \"-241826\",\n"
                                         "        heat_capacity_mj_per_mol_k = 33580,\n"
                                         "        thermal_conductivity_mw_per_m_k = 1 } },\n"
                                         "}\n");
        const auto diagnostics = checker.CheckPackage(package, "init");
        Print("返す表の形", diagnostics);
        EXPECT(diagnostics.size() == 1);
        EXPECT(Has(diagnostics, "bad/init.luau", 5, "型が合わない"));
        EXPECT(std::ranges::any_of(diagnostics, [](const TypeDiagnostic& diagnostic) {
            return diagnostic.message.starts_with("species.water.formation_enthalpy_j_per_mol: ");
        }));

        // require した先の表: 綴りの誤り(知らない欄)・省けない欄が無い・欄の型の誤りが、そのファイルのその行に出る
        const auto required = MakePackage("fields",
                                          {{"package.luau", "return { format = 1 }"},
                                           {"init.luau", "return { reactions = require(\"reactions\") }\n"},
                                           {"reactions.luau",
                                            "return {\n"
                                            "    burn = {\n"
                                            "        reactants = { carbon = 1 },\n"
                                            "        product = { ash = 1 },\n"
                                            "        rate = { a = 3e8, activation_energy_j_per_mol = \"x\" },\n"
                                            "    },\n"
                                            "}\n"}});
        const auto fields = checker.CheckPackage(required, "init");
        Print("require した先の欄", fields);
        EXPECT(fields.size() == 3);
        EXPECT(Has(fields, "fields/reactions.luau", 2, "欄が足りない"));
        EXPECT(Has(fields, "fields/reactions.luau", 4, "知らない欄"));
        EXPECT(Has(fields, "fields/reactions.luau", 5, "型が合わない"));
    }

    void TestAnnotatedTableLine(LuauTypeChecker& checker) {
        // 自分で型を付けた表は、欄の行に誤りが出る(欄が足りない・型が合わない)
        const auto package = MakePackage("annotated",
                                         {{"package.luau", "return { format = 1 }"},
                                          {"init.luau", "return { reactions = require(\"reactions\") }\n"},
                                          {"reactions.luau",
                                           "local reactions: { [string]: Reaction } = {\n"
                                           "    burn = {\n"
                                           "        reactants = { carbon = 1, oxygen = 1 },\n"
                                           "        products = { carbon_dioxide = true },\n"
                                           "        rate = { a = \"3e8\", activation_energy_j_per_mol = 160000 },\n"
                                           "    },\n"
                                           "}\n"
                                           "return reactions\n"}});
        const auto diagnostics = checker.CheckPackage(package, "init");
        Print("型を付けた表", diagnostics);
        EXPECT(!diagnostics.empty());
        EXPECT(std::ranges::all_of(diagnostics, [](const TypeDiagnostic& diagnostic) {
            return diagnostic.file == "annotated/reactions.luau";
        }));
        EXPECT(std::ranges::any_of(diagnostics, [](const TypeDiagnostic& diagnostic) {
            return diagnostic.line >= 1 && diagnostic.line <= 4;
        }));
    }

    void TestNamesAndSandbox(LuauTypeChecker& checker) {
        const auto package = InitPackage("names",
                                         "local a = undefined_helper(1)\n"
                                         "local b = os.time()\n"
                                         "local c = debug.traceback()\n"
                                         "local d = string.upper(\"ok\")\n"
                                         "return {}\n");
        const auto diagnostics = checker.CheckPackage(package, "init");
        Print("知らない名前・殻で見えないもの", diagnostics);
        EXPECT(Has(diagnostics, "names/init.luau", 1, "知らない名前"));
        EXPECT(Has(diagnostics, "names/init.luau", 2, "知らない名前"));
        EXPECT(Has(diagnostics, "names/init.luau", 3, "知らない名前"));
        EXPECT(
            !std::ranges::any_of(diagnostics, [](const TypeDiagnostic& diagnostic) { return diagnostic.line == 4; }));
    }

    void TestRequireAndSyntax(LuauTypeChecker& checker) {
        const auto missing = InitPackage("req", "local util = require(\"lib/util\")\nreturn {}\n");
        const auto outside = InitPackage("outside", "local other = require(\"../core/init\")\nreturn {}\n");
        const auto syntax = InitPackage("syntax", "local x = \nreturn {}\n");
        const auto fine = MakePackage("fine", {{"package.luau", "return { format = 1 }"},
                                               {"init.luau",
                                                "local util = require(\"lib/util\")\n"
                                                "return { data = { value = util.twice(2) } }\n"},
                                               {"lib/util.luau",
                                                "return { twice = function(x: number): number "
                                                "return x * 2 end }\n"}});
        const auto badCall = MakePackage("badcall", {{"package.luau", "return { format = 1 }"},
                                                     {"init.luau",
                                                      "local util = require(\"lib/util\")\n"
                                                      "return { data = { value = util.twice(\"2\") } }\n"},
                                                     {"lib/util.luau",
                                                      "return { twice = function(x: number): number "
                                                      "return x * 2 end }\n"}});

        const auto missingResult = checker.CheckPackage(missing, "init");
        const auto outsideResult = checker.CheckPackage(outside, "init");
        const auto syntaxResult = checker.CheckPackage(syntax, "init");
        const auto fineResult = checker.CheckPackage(fine, "init");
        const auto badCallResult = checker.CheckPackage(badCall, "init");
        Print("require の先が無い", missingResult);
        Print("パッケージの外", outsideResult);
        Print("文法", syntaxResult);
        Print("require した関数(正しい)", fineResult);
        Print("require した関数の引数の型", badCallResult);
        EXPECT(Has(missingResult, "req/init.luau", 1, "require"));
        EXPECT(!outsideResult.empty());
        EXPECT(Has(syntaxResult, "syntax/init.luau", 2, "文法"));
        EXPECT(fineResult.empty());
        EXPECT(Has(badCallResult, "badcall/init.luau", 2, "型が合わない"));
    }

    void TestManifest(LuauTypeChecker& checker) {
        const auto badFormat = MakePackage(
            "manifest", {{"package.luau", "-- マニフェスト\nreturn { format = \"1\" }\n"}, {"init.luau", "return {}"}});
        const auto badDepends = MakePackage(
            "depends", {{"package.luau", "return { format = 1, depends = \"core\" }\n"}, {"init.luau", "return {}"}});
        const auto good = MakePackage(
            "good", {{"package.luau", "return { format = 1, depends = { \"core\" } }\n"}, {"init.luau", "return {}"}});
        const auto formatResult = checker.CheckManifest(badFormat);
        const auto dependsResult = checker.CheckManifest(badDepends);
        Print("マニフェストの format", formatResult);
        Print("マニフェストの depends", dependsResult);
        EXPECT(Has(formatResult, "manifest/package.luau", 2, "型が合わない"));
        EXPECT(Has(dependsResult, "depends/package.luau", 1, "型が合わない"));
        EXPECT(std::ranges::none_of(dependsResult, [](const TypeDiagnostic& diagnostic) {
            return diagnostic.message.find('\n') != std::string::npos;  // ログの 1 行に収まる
        }));
        EXPECT(checker.CheckManifest(good).empty());
    }

    // --- LoadPackages が型の誤りのあるパッケージを読まない ---

    void TestLoadPackagesRejects(LuauTypeChecker& checker) {
        const std::vector<PackageSource> packages = {
            InitPackage("core", "return { data = { a = 1 } }"),
            InitPackage("broken", "return { data = { b = missing_value } }"),
            MakePackage("child", {{"package.luau", "return { format = 1, depends = { \"broken\" } }"},
                                  {"init.luau", "return { data = { c = 3 } }"}}),
            MakePackage("badmanifest",
                        {{"package.luau", "return { format = 1, entry = 5 }"}, {"init.luau", "return {}"}}),
        };

        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        EXPECT(sandbox.has_value());
        if (!sandbox)
            return;

        (*sandbox)->Seal();
        const auto result = LoadPackages(**sandbox, packages,
                                         PackageLoadOptions{.basePackage = "core", .typeChecker = &checker});
        EXPECT(result.has_value());
        if (!result)
            return;

        for (const PackageRejection& rejection : result->rejected)
            std::printf("  読まない: %s: %s\n", rejection.package.c_str(), rejection.reason.c_str());

        EXPECT(result->loadOrder == std::vector<std::string>{"core"});
        const auto reasonOf = [&](std::string_view name) {
            const auto found = std::ranges::find(result->rejected, name, &PackageRejection::package);
            return found == result->rejected.end() ? std::string() : found->reason;
        };
        EXPECT(reasonOf("broken").starts_with("型検査: broken/init.luau:1:"));
        EXPECT(reasonOf("badmanifest").starts_with("型検査: badmanifest/package.luau:1:"));
        EXPECT(reasonOf("child").find("broken") != std::string::npos);
        EXPECT(result->typeDiagnostics.size() >= 2);
    }

    // --- 決定性 ---

    void TestDeterminism(LuauTypeChecker& checker) {
        const auto package = MakePackage(
            "many", {{"package.luau", "return { format = 1 }"},
                     {"init.luau",
                      "local a = nope()\nlocal s = require(\"species\")\n"
                      "return { species = s, elements = { C = { atomic_mass_mg_per_mol = \"x\" } } }\n"},
                     {"species.luau",
                      "local t: { [string]: Species } = { w = { composition = 1 } }\n"
                      "local u: number = \"text\"\nreturn t\n"}});
        const auto first = checker.CheckPackage(package, "init");
        const auto second = checker.CheckPackage(package, "init");
        (void)checker.CheckPackage(InitPackage("other", "return { data = { z = zz } }"), "init");
        const auto third = checker.CheckPackage(package, "init");

        auto fresh = MakeChecker();
        const auto fourth = fresh ? fresh->CheckPackage(package, "init") : std::vector<TypeDiagnostic>{};

        Print("決定性", first);
        EXPECT(first.size() >= 3);
        EXPECT(first == second);
        EXPECT(first == third);
        EXPECT(first == fourth);
        EXPECT(std::ranges::is_sorted(first));
    }

    // --- 型の定義の誤り ---

    void TestDefinitionErrors() {
        const auto noEntry = LuauTypeChecker::Create("export type PackageManifest = { format: number }\n");
        const auto broken = LuauTypeChecker::Create("export type PackageManifest = {\n");
        EXPECT(!noEntry.has_value() && noEntry.error().find("PackageEntry") != std::string::npos);
        EXPECT(!broken.has_value());
        EXPECT(!CreateTypeCheckerFromFile(DATA_DIR / "types" / "no_such_file.d.luau").has_value());
        if (!broken)
            std::printf("  定義の誤り: %s\n", broken.error().c_str());
    }

}  // namespace

int main() {
    auto checker = MakeChecker();
    if (!checker) {
        std::printf("luau_type_check_test: 型の定義を読めない\n");
        return 1;
    }

    TestRealPackagePasses(*checker);
    TestReturnShape(*checker);
    TestAnnotatedTableLine(*checker);
    TestNamesAndSandbox(*checker);
    TestRequireAndSyntax(*checker);
    TestManifest(*checker);
    TestLoadPackagesRejects(*checker);
    TestDeterminism(*checker);
    TestDefinitionErrors();

    if (failureCount != 0) {
        std::printf("luau_type_check_test: %d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("luau_type_check_test: OK\n");
    return 0;
}
