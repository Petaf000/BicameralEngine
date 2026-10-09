// luau_package_test.cpp — script/luau_package(Luau のパッケージ。T-0138、13 §2・ADR-0031)を CPU だけで確かめる。
// 見るもの: パッケージの中だけの require(キャッシュ・循環・外に出られない)/ 読む順番(依存 → 名前)/ 依存の欠け・循環 /
//           表の合わせ方 3 種(追加だけ・印つきの上書き・印なしの上書き)と「改造された世界」の印 / マニフェストと戻り値の誤り /
//           ディスクのフォルダから読む / 決定性(入れる順番・殻を変えても、同じパッケージ群なら同じバイト列)。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "script/luau_package.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

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

    constexpr const char* MANIFEST = "return { format = 1 }";

    PackageSource MakePackage(std::string name, std::initializer_list<std::pair<std::string, std::string>> files) {
        PackageSource package{.name = std::move(name), .files = {}};
        for (const auto& [path, source] : files)
            package.files.emplace(path, source);

        return package;
    }

    // マニフェスト(depends つき)と init だけのパッケージ
    PackageSource SimplePackage(std::string name, const std::string& depends, std::string init) {
        const std::string manifest = "return { format = 1, depends = { " + depends + " } }";

        return MakePackage(std::move(name), {{"package.luau", manifest}, {"init.luau", std::move(init)}});
    }

    std::unique_ptr<LuauSandbox> MakeSealed(int32_t randomSeed = 0) {
        auto created = LuauSandbox::Create(SandboxLimits{.randomSeed = randomSeed});
        if (!created) {
            std::printf("Create に失敗: %s\n", created.error().c_str());
            ++failureCount;

            return nullptr;
        }

        (*created)->Seal();

        return std::move(*created);
    }

    PackageSetResult Load(const std::vector<PackageSource>& packages, const PackageLoadOptions& options = {}) {
        auto sandbox = MakeSealed();
        if (!sandbox)
            return {};

        auto result = LoadPackages(*sandbox, packages, options);
        if (!result) {
            std::printf("LoadPackages に失敗: %s\n", result.error().c_str());
            ++failureCount;

            return {};
        }

        return std::move(*result);
    }

    // 読まなかった理由(読んだなら空。理由に part を含むかは HasRejection で)
    std::string RejectionOf(const PackageSetResult& result, std::string_view package) {
        for (const PackageRejection& rejection : result.rejected) {
            if (rejection.package == package)
                return rejection.reason;
        }

        return {};
    }

    bool HasRejection(const PackageSetResult& result, std::string_view package, std::string_view part) {
        const std::string reason = RejectionOf(result, package);
        if (!reason.empty() && reason.find(part) != std::string::npos)
            return true;

        std::printf("  '%s' の理由に '%.*s' が無い: '%s'\n", std::string(package).c_str(),
                    static_cast<int>(part.size()), part.data(), reason.c_str());

        return false;
    }

    const ScriptValue* EntryValue(const PackageSetResult& result, const std::string& category, const std::string& key) {
        const auto table = result.tables.find(category);
        if (table == result.tables.end())
            return nullptr;

        const auto entry = table->second.find(key);

        return entry == table->second.end() ? nullptr : &entry->second.value;
    }

    // 表の値 value の欄 field の数(無ければ -1)
    double FieldNumber(const ScriptValue* value, std::string_view field) {
        const ScriptValue* found = value ? value->Find(field) : nullptr;

        return found && found->IsNumber() ? found->number : -1.0;
    }

    bool FieldTrue(const ScriptValue* value, std::string_view field) {
        const ScriptValue* found = value ? value->Find(field) : nullptr;

        return found && found->kind == ScriptValue::Kind::Boolean && found->boolean;
    }

    // --- require ---

    void TestRequireInsidePackage() {
        const char* init = R"(
            counter = { n = 0 }
            local util = require("lib/util")
            local again = require("lib/util")
            return { checks = { require = {
                heat = util.double(21), same = util == again, runs = counter.n,
                noLeak = moduleGlobal == nil, nested = require("lib/deep/leaf").name == "leaf",
            } } }
        )";
        const char* util = R"(
            counter.n += 1
            moduleGlobal = 1  -- モジュールの環境に入り、entry からは見えない
            return { double = function(x) return x * 2 end }
        )";

        const PackageSetResult result = Load(
            {MakePackage("core", {{"package.luau", MANIFEST},
                                  {"init.luau", init},
                                  {"lib/util.luau", util},
                                  {"lib/deep/leaf.luau", "return { name = 'leaf' }"}})});
        const ScriptValue* checks = EntryValue(result, "checks", "require");
        EXPECT(result.loadOrder == std::vector<std::string>{"core"});
        EXPECT(FieldNumber(checks, "heat") == 42.0);
        EXPECT(FieldTrue(checks, "same"));
        EXPECT(FieldNumber(checks, "runs") == 1.0);  // 2 回 require しても 1 回だけ走る
        EXPECT(FieldTrue(checks, "noLeak"));
        EXPECT(FieldTrue(checks, "nested"));
    }

    void TestRequireErrors() {
        const char* retry = R"(
            local first = pcall(require, "bad")
            local second, message = pcall(require, "bad")
            return { checks = { retry = { first = first, second = second, notCycle = string.find(message, "boom") ~= nil } } }
        )";

        const PackageSetResult result = Load({
            SimplePackage("missing", "", "require('nope')"),
            SimplePackage("escape", "", "require('../missing/init')"),
            SimplePackage("absolute", "", "require('/init')"),
            SimplePackage("extension", "", "require('init.luau')"),
            SimplePackage(
                "cross", "",
                "require('missing/init')"),  // ほかのパッケージには届かない(自分の中の missing/init.luau を探す)
            MakePackage("cycle", {{"package.luau", MANIFEST},
                                  {"init.luau", "require('x')"},
                                  {"x.luau", "return require('y')"},
                                  {"y.luau", "return require('x')"}}),
            MakePackage("nilmodule",
                        {{"package.luau", MANIFEST}, {"init.luau", "require('empty')"}, {"empty.luau", ""}}),
            MakePackage(
                "inmanifest",
                {{"package.luau", "require('x') return { format = 1 }"}, {"init.luau", ""}, {"x.luau", "return 1"}}),
            MakePackage("retry", {{"package.luau", MANIFEST}, {"init.luau", retry}, {"bad.luau", "error('boom')"}}),
        });

        EXPECT(HasRejection(result, "missing", "nope.luau が無い"));
        EXPECT(HasRejection(result, "escape", "名前は"));
        EXPECT(HasRejection(result, "absolute", "名前は"));
        EXPECT(HasRejection(result, "extension", "名前は"));
        EXPECT(HasRejection(result, "cross", "missing/init.luau が無い"));
        EXPECT(HasRejection(result, "cycle", "require の循環: cycle/x.luau → cycle/y.luau → cycle/x.luau"));
        EXPECT(HasRejection(result, "nilmodule", "nil 以外"));
        EXPECT(HasRejection(result, "inmanifest", "package.luau"));  // マニフェストでは require が見えない

        // 失敗したモジュールは「読み込み中」のまま残らない(2 回目も同じ誤りで、循環にならない)
        const ScriptValue* retried = EntryValue(result, "checks", "retry");
        EXPECT(result.loadOrder == std::vector<std::string>{"retry"});
        EXPECT(retried && !FieldTrue(retried, "first") && !FieldTrue(retried, "second"));
        EXPECT(FieldTrue(retried, "notCycle"));
    }

    // --- 読む順番 ---

    std::vector<PackageSource> OrderPackages() {
        return {
            SimplePackage("zeta", "'alpha'", ""),
            SimplePackage("mid", "", ""),
            SimplePackage("core", "", ""),
            SimplePackage("alpha", "", ""),
            SimplePackage("ghost", "'nothere'", ""),
            SimplePackage("haunt", "'ghost'", ""),
            SimplePackage("loop-a", "'loop-b'", ""),
            SimplePackage("loop-b", "'loop-a'", ""),
            SimplePackage("on-loop", "'loop-a'", ""),
        };
    }

    void TestOrder() {
        std::vector<PackageSource> packages = OrderPackages();
        const PackageSetResult free = Load(packages);
        EXPECT((free.loadOrder == std::vector<std::string>{"alpha", "core", "mid", "zeta"}));

        const PackageSetResult based = Load(packages, PackageLoadOptions{.basePackage = "core"});
        EXPECT((based.loadOrder == std::vector<std::string>{"core", "alpha", "mid", "zeta"}));
        EXPECT(HasRejection(based, "ghost", "'nothere' が無い"));
        EXPECT(HasRejection(based, "haunt", "'ghost' を読まなかった"));
        EXPECT(HasRejection(based, "loop-a", "循環"));
        EXPECT(HasRejection(based, "loop-b", "循環"));
        EXPECT(HasRejection(based, "on-loop", "循環"));

        // 入れる順番を変えても同じ
        std::ranges::reverse(packages);
        const PackageSetResult reversed = Load(packages, PackageLoadOptions{.basePackage = "core"});
        EXPECT(reversed.loadOrder == based.loadOrder);
        EXPECT(reversed.rejected.size() == based.rejected.size());

        // ゲーム本体が無ければ、ほかも読まない
        const PackageSetResult noBase = Load({SimplePackage("mod", "", "")}, PackageLoadOptions{.basePackage = "core"});
        EXPECT(noBase.loadOrder.empty());
        EXPECT(HasRejection(noBase, "mod", "'core' が無い"));
    }

    // --- 表の合わせ方 ---

    std::vector<PackageSource> MergePackages() {
        return {
            SimplePackage(
                "core", "",
                "return { reactions = { burn = { rate = 1 }, rust = { rate = 2 } }, elements = { fire = {} } }"),
            SimplePackage("addmod", "", "return { reactions = { freeze = { rate = 3 } } }"),
            SimplePackage("overmod", "", "return { reactions = { burn = { rate = 99 } }, spells = { ignite = true } }"),
            SimplePackage("overdep", "'overmod'", "return { spells = { blaze = true } }"),
        };
    }

    void TestMergeAddOnly() {
        const PackageSetResult result = Load(
            MergePackages(), PackageLoadOptions{.mergeMode = MergeMode::AddOnly, .basePackage = "core"});

        EXPECT((result.loadOrder == std::vector<std::string>{"core", "addmod"}));
        EXPECT(HasRejection(result, "overmod", "reactions.burn('core' が書いた)"));
        EXPECT(HasRejection(result, "overdep", "'overmod' を読まなかった"));
        EXPECT(FieldNumber(EntryValue(result, "reactions", "burn"), "rate") == 1.0);
        EXPECT(FieldNumber(EntryValue(result, "reactions", "freeze"), "rate") == 3.0);
        EXPECT(!result.tables.contains("spells"));  // 読まなかったパッケージの分は何も残らない(全部か無しか)
        EXPECT(result.overrides.empty());
        EXPECT(!result.modifiedWorld);
    }

    void TestMergeOverride() {
        const PackageLoadOptions marked{.mergeMode = MergeMode::OverrideMarked, .basePackage = "core"};
        const PackageSetResult result = Load(MergePackages(), marked);

        EXPECT((result.loadOrder == std::vector<std::string>{"core", "addmod", "overmod", "overdep"}));
        EXPECT(FieldNumber(EntryValue(result, "reactions", "burn"), "rate") == 99.0);
        EXPECT(result.tables.at("reactions").at("burn").package == "overmod");
        EXPECT(result.overrides.size() == 1);
        EXPECT(!result.overrides.empty() && result.overrides[0].category == "reactions" &&
               result.overrides[0].key == "burn" && result.overrides[0].previousPackage == "core" &&
               result.overrides[0].package == "overmod");
        EXPECT(result.modifiedWorld);  // 「改造された世界」の印

        // 印なしの上書き(案 A): 中身は同じで、印だけ付かない
        const PackageSetResult plain = Load(
            MergePackages(), PackageLoadOptions{.mergeMode = MergeMode::Override, .basePackage = "core"});
        EXPECT(!plain.modifiedWorld);
        EXPECT(plain.overrides.size() == 1);
        EXPECT(TableBytes(plain) == TableBytes(result));

        // 表の版: 上書きした世界と、追加だけの世界は違う版
        const PackageSetResult addOnly = Load(MergePackages(), PackageLoadOptions{.basePackage = "core"});
        EXPECT(TableVersion(addOnly) != TableVersion(result));
    }

    // --- マニフェストと戻り値の誤り ---

    void TestManifestAndValueErrors() {
        const PackageSetResult result = Load({
            MakePackage("nomanifest", {{"init.luau", ""}}),
            MakePackage("noformat", {{"package.luau", "return {}"}, {"init.luau", ""}}),
            MakePackage("newformat", {{"package.luau", "return { format = 2 }"}, {"init.luau", ""}}),
            MakePackage("unknown", {{"package.luau", "return { format = 1, priority = 3 }"}, {"init.luau", ""}}),
            MakePackage("baddeps", {{"package.luau", "return { format = 1, depends = 'core' }"}, {"init.luau", ""}}),
            MakePackage("Upper", {{"package.luau", MANIFEST}, {"init.luau", ""}}),
            MakePackage("noentry", {{"package.luau", "return { format = 1, entry = 'main' }"}}),
            MakePackage("custom", {{"package.luau", "return { format = 1, entry = 'src/main' }"},
                                   {"src/main.luau", "return { things = { a = 1 } }"}}),
            SimplePackage("function", "", "return { things = { f = function() end } }"),
            SimplePackage("cyclic", "", "local t = {} t.self = t return { things = { c = t } }"),
            SimplePackage("nan", "", "return { things = { n = 0 / 0 } }"),
            SimplePackage("vector", "", "return { things = { v = vector.create(1, 2, 3) } }"),
            SimplePackage("notable", "", "return 3"),
            SimplePackage("badcategory", "", "return { ['Bad Cat'] = {} }"),
            SimplePackage("nothing", "", "return nil"),
        });

        EXPECT(HasRejection(result, "nomanifest", "package.luau が無い"));
        EXPECT(HasRejection(result, "noformat", "format = 1 が無い"));
        EXPECT(HasRejection(result, "newformat", "format は 1"));
        EXPECT(HasRejection(result, "unknown", "知らない欄 \"priority\""));
        EXPECT(HasRejection(result, "baddeps", "depends は名前の配列"));
        EXPECT(HasRejection(result, "Upper", "英小文字"));
        EXPECT(HasRejection(result, "noentry", "main.luau が無い"));
        EXPECT(HasRejection(result, "function", "function は表にできない"));
        EXPECT(HasRejection(result, "cyclic", "循環"));
        EXPECT(HasRejection(result, "nan", "NaN"));
        EXPECT(HasRejection(result, "vector", "vector は表にできない"));
        EXPECT(HasRejection(result, "notable", "表を 1 つ返す"));
        EXPECT(HasRejection(result, "badcategory", "分類"));
        EXPECT((result.loadOrder == std::vector<std::string>{"custom", "nothing"}));
        EXPECT(FieldNumber(EntryValue(result, "things", "a"), "") == -1.0);  // 値は数(表ではない)
        EXPECT(EntryValue(result, "things", "a") && EntryValue(result, "things", "a")->number == 1.0);
    }

    void TestUsageErrors() {
        auto created = LuauSandbox::Create({});
        EXPECT(created.has_value());
        if (!created)
            return;

        const std::vector<PackageSource> one = {SimplePackage("core", "", "")};
        EXPECT(!LoadPackages(**created, one, {}).has_value());  // Seal の前

        (*created)->Seal();
        const std::vector<PackageSource> twice = {SimplePackage("core", "", ""), SimplePackage("core", "", "")};
        EXPECT(!LoadPackages(**created, twice, {}).has_value());  // 同じ名前が 2 つ
        EXPECT(LoadPackages(**created, one, {}).has_value());
    }

    // --- ディスクから ---

    void WriteFile(const fs::path& path, std::string_view text) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    void TestReadFromDisk() {
        const fs::path root = fs::temp_directory_path() / "bicameral_luau_package_test";
        std::error_code error;
        fs::remove_all(root, error);

        WriteFile(root / "core" / "package.luau", MANIFEST);
        WriteFile(root / "core" / "init.luau", "return { reactions = { burn = require('lib/rate') } }");
        WriteFile(root / "core" / "lib" / "rate.luau", "return { rate = 5 }");
        WriteFile(root / "mod" / "package.luau", "return { format = 1, depends = { 'core' } }");
        WriteFile(root / "mod" / "init.luau", "return { reactions = { freeze = { rate = 6 } } }");
        WriteFile(root / "mod" / "readme.txt", "Luau ではないので読まない");

        const auto packages = ReadPackageRoot(root);
        EXPECT(packages.has_value());
        if (packages) {
            EXPECT(packages->size() == 2);
            EXPECT(packages->size() == 2 && (*packages)[0].files.contains("lib/rate.luau"));
            EXPECT(packages->size() == 2 && (*packages)[1].files.size() == 2);

            const PackageSetResult result = Load(*packages);
            EXPECT((result.loadOrder == std::vector<std::string>{"core", "mod"}));
            EXPECT(FieldNumber(EntryValue(result, "reactions", "burn"), "rate") == 5.0);
        }

        EXPECT(!ReadPackageFolder(root / "nothere").has_value());
        fs::remove_all(root, error);
    }

    // --- 決定性 ---

    std::vector<PackageSource> DeterminismPackages() {
        // 鍵を入れる順番が違う表・数と文字列の鍵・入れ子・math.random(実行ごとに決まった種)
        const char* core = R"(
            local forward, backward = {}, {}
            for i = 1, 40 do forward["k" .. i] = i end
            for i = 40, 1, -1 do backward["k" .. i] = i end
            local mixed = { 10, 20, 30, name = "mixed", [2.5] = "half", nested = { deep = { 1, { "x" } } } }
            return { data = { forward = forward, backward = backward, mixed = mixed, random = { math.random(1, 1000000), math.random() } } }
        )";

        return {
            MakePackage("core", {{"package.luau", MANIFEST}, {"init.luau", core}}),
            SimplePackage("addon-b", "", "return { data = { b = { roll = math.random(1, 1000000) } } }"),
            SimplePackage("addon-a", "", "return { data = { a = { roll = math.random(1, 1000000) } } }"),
            SimplePackage("over", "'addon-a'", "return { data = { b = { roll = -1 } } }"),
        };
    }

    std::string LoadBytes(std::vector<PackageSource> packages, int32_t randomSeed, MergeMode mode) {
        auto sandbox = MakeSealed(randomSeed);
        if (!sandbox)
            return {};

        const auto result = LoadPackages(*sandbox, packages,
                                         PackageLoadOptions{.mergeMode = mode, .basePackage = "core"});
        if (!result)
            return "ERROR";

        // 中身・読んだ順・読まなかったもの・上書き・印を全部つなげる
        std::string bytes = TableBytes(*result);
        for (const std::string& name : result->loadOrder)
            bytes += "|" + name;
        for (const PackageRejection& rejection : result->rejected)
            bytes += "|x:" + rejection.package + ":" + rejection.reason;
        for (const TableOverride& overridden : result->overrides)
            bytes += "|o:" + overridden.category + "." + overridden.key + ":" + overridden.previousPackage + ">" +
                     overridden.package;

        return bytes + (result->modifiedWorld ? "|modified" : "|clean");
    }

    void TestDeterminism() {
        const std::vector<PackageSource> packages = DeterminismPackages();
        std::vector<PackageSource> reversed = packages;
        std::ranges::reverse(reversed);

        for (const MergeMode mode : {MergeMode::AddOnly, MergeMode::OverrideMarked}) {
            const std::string first = LoadBytes(packages, 7, mode);
            EXPECT(first.size() > 100);
            EXPECT(LoadBytes(packages, 7, mode) == first);  // 別の殻で 2 回目
            EXPECT(LoadBytes(reversed, 7, mode) == first);  // 入れる順番を変えた
            EXPECT(LoadBytes(packages, 8, mode) != first);  // 種を変えると変わる(種で決まっている)
        }

        // 同じ殻で 2 回読んでも同じ
        auto sandbox = MakeSealed(7);
        if (!sandbox)
            return;

        const auto once = LoadPackages(*sandbox, packages, PackageLoadOptions{.basePackage = "core"});
        const auto twice = LoadPackages(*sandbox, packages, PackageLoadOptions{.basePackage = "core"});
        EXPECT(once && twice && TableBytes(*once) == TableBytes(*twice));

        // 入れた順番が逆の表は、同じ木(同じバイト列)になる
        if (once) {
            std::string forward, backward;
            AppendCanonicalBytes(once->tables.at("data").at("forward").value, forward);
            AppendCanonicalBytes(once->tables.at("data").at("backward").value, backward);
            EXPECT(forward == backward);
        }
    }

}  // namespace

int main() {
    TestRequireInsidePackage();
    TestRequireErrors();
    TestOrder();
    TestMergeAddOnly();
    TestMergeOverride();
    TestManifestAndValueErrors();
    TestUsageErrors();
    TestReadFromDisk();
    TestDeterminism();

    if (failureCount != 0) {
        std::printf("luau_package_test: %d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("luau_package_test: OK\n");
    return 0;
}
