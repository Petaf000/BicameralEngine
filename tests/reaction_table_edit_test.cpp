// reaction_table_edit_test.cpp — 反応表のパネルの中身(T-0219・D-449): Luau のリテラルの探し方と、書き戻し → 読み直し。
//
//   - 字句: コメント・長い括弧の文字列・関数の本体の中の「鍵 = 値」を拾わない。鍵の並びの後ろで探す。2 か所・式は書き戻せない
//   - 試験の表(data/packages/combustion_test の写し)の一覧: 書き戻せる値・式の値(thermal_conductivity は solid(120))・
//     保存則の検査(元素の釣り合い・生成エンタルピーの差)
//   - 同じ値を書き戻す → ファイルは 1 バイトも変わらない・同じ版の表
//   - 値を 1 つずつ変える → そのリテラルだけ変わる → 読み直すと、その値だけ変わった表 → 元に戻す → 元のファイル・元の版
//   - 係数を変えて元素が釣り合わない → 読み直しが落ちる(ベイクの検査)
//   - 再生ファイルの表(フォルダ無し)は全部書き戻せない
// GPU は使わない(世界に当てるのはホットリロード。窓の確認は --auto-table-edit)。
#include "script/reaction_table_edit.h"

#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "script/luau_literal_edit.h"
#include "script/reaction_package.h"

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

    constexpr std::string_view BASE_PACKAGE = "combustion_test";

    std::string ReadText(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);

        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    void WriteText(const fs::path& path, std::string_view text) {
        std::ofstream(path, std::ios::binary) << text;
    }

    // パッケージの全部のファイルの中身(相対パス → 中身)
    std::map<std::string, std::string> Snapshot(const fs::path& folder) {
        std::map<std::string, std::string> files;
        for (const auto& entry : fs::recursive_directory_iterator(folder)) {
            if (entry.is_regular_file())
                files[fs::relative(entry.path(), folder).generic_string()] = ReadText(entry.path());
        }

        return files;
    }

    std::string Literal(std::string_view source, std::vector<std::string> keyPath) {
        const auto span = FindLuauLiteral(source, keyPath);
        if (!span) {
            std::printf("  %s\n", span.error().c_str());
            return {};
        }

        return std::string(span->Text(source));
    }

    // --- 字句と探し方 ---

    void TestFinder() {
        constexpr std::string_view SOURCE = R"(-- a = 1 (コメントの中の鍵)
--[[ rate = { a = 5 } ]]
local helper = function(x) local a = 3 return x end
return {
    burn = {
        rate = { a = "2.8e19", activation_energy_j_per_mol = 160000 }, -- } {
        note = [==[ a = 9 }]==],
        ["quoted-key"] = -42,
        computed = helper(120),
        list = { 1, 2, { inner = 7 } },
    },
    other = { rate = { a = 1e5 }; b = 0x10 },
}
)";
        EXPECT(Literal(SOURCE, {"burn", "rate", "a"}) == "\"2.8e19\"");
        EXPECT(Literal(SOURCE, {"burn", "rate", "activation_energy_j_per_mol"}) == "160000");
        EXPECT(Literal(SOURCE, {"burn", "quoted-key"}) == "-42");
        EXPECT(Literal(SOURCE, {"other", "rate", "a"}) == "1e5");
        EXPECT(Literal(SOURCE, {"other", "b"}) == "0x10");
        EXPECT(Literal(SOURCE, {"inner"}) == "7");
        EXPECT(Literal(SOURCE, {"note"}) == "[==[ a = 9 }]==]");

        // 2 か所(関数の本体の local a は数えない)・式・位置で並べた表を飛び越えない
        const std::vector<std::string> bare = {"a"};
        const auto both = FindLuauLiterals(SOURCE, bare);
        EXPECT(both && both->literals.size() == 2 && both->expressions.empty());
        EXPECT(!FindLuauLiteral(SOURCE, bare));
        EXPECT(!FindLuauLiteral(SOURCE, std::vector<std::string>{"burn", "computed"}));
        EXPECT(!FindLuauLiteral(SOURCE, std::vector<std::string>{"burn", "list", "inner"}));
        EXPECT(!FindLuauLiteral(SOURCE, std::vector<std::string>{"missing"}));

        // 差し替えはその字面だけ
        const std::vector<std::string> path = {"burn", "rate", "a"};
        const auto span = FindLuauLiteral(SOURCE, path);
        EXPECT(span.has_value());
        if (span) {
            std::string expected(SOURCE);
            expected.replace(expected.find("\"2.8e19\""), 8, "\"4e10\"");
            EXPECT(ReplaceLuauLiteral(SOURCE, *span, "\"4e10\"") == expected);
            EXPECT(span->isString && span->line == 6);
        }

        EXPECT(LuauLiteralValue("\"a\\\"b\"").value_or("") == "a\"b");
        EXPECT(LuauLiteralValue("1_000").value_or("") == "1000");
        EXPECT(FormatLuauString("a\"b") == "\"a\\\"b\"");
        EXPECT(!FindLuauLiterals("return { a = \"x", bare));
        EXPECT(!FindLuauLiterals("return { a = 1 } }", bare));
    }

    // --- 試験の表の一覧と保存則の検査 ---

    std::expected<ReactionTableDocument, std::string> Load(const fs::path& root) {
        const auto loaded = LoadReactionTable({.packageRoot = root});
        if (!loaded)
            return std::unexpected(loaded.error());

        return BuildReactionTableDocument(*loaded);
    }

    // 書き戻せる値の全部
    std::vector<const ReactionValue*> WritableValues(const ReactionTableDocument& document) {
        std::vector<const ReactionValue*> values;
        const auto add = [&](const ReactionValue& value) {
            if (!value.file.empty())
                values.push_back(&value);
        };

        for (const ElementRow& row : document.elements)
            add(row.atomicMass);

        for (const SpeciesRow& row : document.species) {
            add(row.formationEnthalpy);
            add(row.heatCapacity);
            add(row.thermalConductivity);
        }

        for (const RuleRow& row : document.rules) {
            add(row.preExponential);
            add(row.activationEnergy);
            if (row.declaredEnthalpy)
                add(*row.declaredEnthalpy);

            for (const ReactionValue& coefficient : row.coefficients)
                add(coefficient);
        }

        return values;
    }

    void TestDocument(const ReactionTableDocument& document) {
        // 現実の 4 元素・7 物質・5 規則 + 触るための仮の魔素(元素 1・物質 1・規則 2。T-0225)
        EXPECT(document.elements.size() == 5);
        EXPECT(document.species.size() == 8);
        EXPECT(document.rules.size() == 7);

        for (const SpeciesRow& row : document.species) {
            EXPECT(!row.formationEnthalpy.file.empty());
            EXPECT(!row.heatCapacity.file.empty());
            // solid(120)・gas(27) は式なので書き戻せない
            EXPECT(row.thermalConductivity.file.empty() && !row.thermalConductivity.readOnly.empty());
        }

        bool manaRuleFound = false;
        int64_t celluloseEnthalpy = 0;
        int64_t manaEnthalpy = 1;
        for (const RuleRow& row : document.rules) {
            EXPECT(row.elementImbalance.empty());
            EXPECT(!row.preExponential.file.empty() && !row.activationEnergy.file.empty());
            if (row.name == "carbon_combustion") {
                EXPECT(row.enthalpyJoulesPerMol == -393509);
                EXPECT(row.declaredEnthalpy && row.declaredEnthalpy->text == "-393500");
                EXPECT(row.equation == "carbon + oxygen → carbon_dioxide");
            }

            if (row.name == "cellulose_pyrolysis")
                EXPECT(row.preExponential.text == "2.8e19");

            // 仮の魔素は触媒(両辺に 1 ずつ): 反応熱は魔素の無い木の燃焼と同じで、速度の値は書き戻せる(T-0225)
            if (row.name == "mana_test_catalyzed_cellulose_combustion") {
                const size_t arrow = row.equation.find("→");
                EXPECT(arrow != std::string::npos && row.equation.find("mana_test") < arrow &&
                       row.equation.find("mana_test", arrow) != std::string::npos);
                EXPECT(row.activationEnergy.text == "100000");
                manaRuleFound = true;
            }

            if (row.name == "cellulose_combustion")
                celluloseEnthalpy = row.enthalpyJoulesPerMol;

            if (row.name == "mana_test_catalyzed_cellulose_combustion")
                manaEnthalpy = row.enthalpyJoulesPerMol;
        }

        EXPECT(manaRuleFound && celluloseEnthalpy == manaEnthalpy);

        std::printf("  thermal_conductivity → %s\n", document.species.front().thermalConductivity.readOnly.c_str());
    }

    // 1 つの値を変えた時に期待する字面(整数は +7、係数は +1〔元素が釣り合わなくなる〕、A は仮数を 2 倍)
    std::string ChangedText(const ReactionValue& value) {
        if (value.kind == ReactionValueKind::Decimal) {
            const auto decimal = ParseDecimal(value.text);

            return std::format("{}e{}", decimal ? decimal->mantissa * 2 : 0, decimal ? decimal->exponent10 : 0);
        }

        const int64_t current = std::stoll(value.text);

        return std::format(
            "{}", value.keyPath[2] == "reactants" || value.keyPath[2] == "products" ? current + 1 : current + 7);
    }

    bool IsCoefficient(const ReactionValue& value) {
        return value.keyPath.size() == 4 && (value.keyPath[2] == "reactants" || value.keyPath[2] == "products");
    }

    // 書き戻す → 読み直す → 同じ表(変えた値だけ違う)→ 戻す → 元のファイル・元の版
    void TestRoundTrip(const fs::path& root) {
        const fs::path package = root / BASE_PACKAGE;
        const auto original = Snapshot(package);
        const auto initial = LoadReactionTable({.packageRoot = root});
        EXPECT(initial.has_value());
        if (!initial)
            return;

        const auto document = BuildReactionTableDocument(*initial);
        EXPECT(document.has_value());
        if (!document) {
            std::printf("  一覧を作れない: %s\n", document.error().c_str());
            return;
        }

        TestDocument(*document);
        const auto values = WritableValues(*document);
        EXPECT(values.size() >= 30);

        // --- 同じ値 → 何も書かない("2.8e19" と書いてある所に "28e18" と書いても同じ値)---
        for (const ReactionValue* value : values)
            EXPECT(WriteReactionValue(*value, value->text).has_value());

        EXPECT(Snapshot(package) == original);
        if (const auto* pyrolysis = FindReactionValue(
                *document, std::vector<std::string>{"reactions", "cellulose_pyrolysis", "rate", "a"})) {
            EXPECT(WriteReactionValue(*pyrolysis, "28e18").has_value());
            EXPECT(Snapshot(package) == original);
        }

        // --- 1 つずつ変える ---
        size_t changed = 0;
        for (const ReactionValue* value : values) {
            const std::string text = ChangedText(*value);
            const std::string before = ReadText(value->file);
            const auto span = FindLuauLiteral(before, value->sourceKeyPath);
            const auto literal = ReactionValueLiteral(*value, text);
            EXPECT(span && literal);
            if (!span || !literal)
                continue;

            const auto written = WriteReactionValue(*value, text);
            EXPECT(written.has_value());
            EXPECT(ReadText(value->file) == ReplaceLuauLiteral(before, *span, *literal));

            const auto next = Load(root);
            if (IsCoefficient(*value)) {
                // 元素が釣り合わない → ベイクの検査で落ちる(保存則)
                EXPECT(!next.has_value());
            } else {
                EXPECT(next.has_value());
                if (next) {
                    const ReactionValue* reread = FindReactionValue(*next, value->keyPath);
                    EXPECT(reread != nullptr);
                    if (reread != nullptr) {
                        const auto expected = ParseDecimal(text);
                        const auto actual = ParseDecimal(reread->text);
                        EXPECT(value->kind == ReactionValueKind::Integer
                                   ? reread->text == text
                                   : (expected && actual && expected->mantissa == actual->mantissa &&
                                      expected->exponent10 == actual->exponent10));
                    }

                    EXPECT(next->version != document->version);
                    ++changed;
                }
            }

            // --- 戻す ---
            EXPECT(WriteReactionValue(*value, value->text).has_value());
            EXPECT(ReadText(value->file) == before);
        }

        EXPECT(Snapshot(package) == original);
        const auto restored = LoadReactionTable({.packageRoot = root});
        EXPECT(restored && restored->tableVersion == initial->tableVersion);
        std::printf("  書き戻した値 %zu 個(係数を除いて読み直せた %zu 個)\n", values.size(), changed);

        // --- 書けない値・範囲の外 ---
        const ReactionValue& conductivity = document->species.front().thermalConductivity;
        EXPECT(!WriteReactionValue(conductivity, "1").has_value());
        EXPECT(!WriteReactionValue(document->species.front().heatCapacity, "0").has_value());
        EXPECT(!WriteReactionValue(document->rules.front().preExponential, "abc").has_value());
        EXPECT(!WriteReactionValue(document->rules.front().activationEnergy, "1.5").has_value());
        EXPECT(Snapshot(package) == original);

        // --- 再生ファイルの表(フォルダ無し)は書き戻せない ---
        const auto replayed = RebuildReactionTable(initial->tableBytes, initial->tableVersion);
        EXPECT(replayed.has_value());
        if (replayed) {
            const auto replayDocument = BuildReactionTableDocument(*replayed);
            EXPECT(replayDocument && WritableValues(*replayDocument).empty());
        }
    }

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "bicameral_reaction_table_edit_test";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    const fs::path packages = root / "packages";
    fs::create_directories(packages);
    fs::copy(fs::path(BICAMERAL_DATA_PACKAGES_DIR) / BASE_PACKAGE, packages / BASE_PACKAGE,
             fs::copy_options::recursive);

    TestFinder();
    TestRoundTrip(packages);

    fs::remove_all(root, ignored);
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("reaction_table_edit: すべて通過\n");

    return 0;
}
