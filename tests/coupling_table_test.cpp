// coupling_table_test.cpp — 流れどうしの結合(分類 drags・machines。T-0536・ADR-0238)の読み手・ベイク・ホットリロードを CPU だけで確かめる。
// 見るもの: 2 つの分類が同じ型・同じ検査を通る(同じ q は同じ効く q・同じ効率の上限)/ 書いた |q| > 1 はベイクが ±1 に押さえて警告する
//           (押す構造の模型 tests/push_model_test・引きずりの模型 tests/drag_model_test が同じ関数で押さえる)/ 費用と速さが模型と同じ刻みの単位 /
//           q の書き方(10 進の文字列・整数。0 の向きに切り捨て)/ 形の誤り(知らない欄・小数の数・流れの書き方・|q| > 1000)と
//           ベイクの誤り(知らない物質・X = Y・分類で使えない流れ)/ ホットリロードで書き換えた q が読み直されて押さえられる・再生ファイルの中身から同じ表。
// 失敗すると条件と行を表示して 1 を返す。
#include <cmath>
#include <cstdio>
#include <expected>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "script/coupling_package.h"
#include "script/luau_sandbox.h"
#include "script/reaction_hot_reload.h"
#include "script/reaction_package.h"
#include "script/reaction_table_loader.h"
#include "sim/coupling_table.h"
#include "sim/reaction_table.h"

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

    bool Contains(std::string_view text, std::string_view part) {
        if (text.find(part) != std::string_view::npos)
            return true;

        std::printf("  '%.*s' に '%.*s' が無い\n", static_cast<int>(text.size()), text.data(),
                    static_cast<int>(part.size()), part.data());

        return false;
    }

    // 試験の元素と物質(salt・water・air)。結合だけを足して読む
    constexpr std::string_view BASE_TABLE = R"(
        elements = { A = { atomic_mass_mg_per_mol = 10000 }, B = { atomic_mass_mg_per_mol = 20000 } },
        species = {
            salt = { composition = { A = 1 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = 30000,
                     thermal_conductivity_mw_per_m_k = 1000 },
            water = { composition = { B = 1 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = 75000,
                      thermal_conductivity_mw_per_m_k = 600000 },
            air = { composition = { A = 1, B = 1 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = 29000,
                    thermal_conductivity_mw_per_m_k = 26000 },
        },
    )";

    std::string InitWith(std::string_view couplings) {
        return std::format("return {{ {} {} }}", BASE_TABLE, couplings);
    }

    std::expected<PackageSetResult, std::string> LoadInit(std::string init) {
        PackageSource package{.name = "world", .files = {}};
        package.files.emplace("package.luau", "return { format = 1 }");
        package.files.emplace("init.luau", std::move(init));

        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        if (!sandbox)
            return std::unexpected(sandbox.error());

        (*sandbox)->Seal();
        const std::vector<PackageSource> packages{std::move(package)};
        auto loaded = LoadPackages(**sandbox, packages, PackageLoadOptions{});
        if (!loaded)
            return std::unexpected(loaded.error());

        if (!loaded->rejected.empty())
            return std::unexpected(loaded->rejected.front().reason);

        return std::move(*loaded);
    }

    // 読んでベイクした表(形の誤り・ベイクの誤りは error)
    std::expected<BakedReactionTable, std::string> Bake(std::string_view couplings) {
        const auto loaded = LoadInit(InitWith(couplings));
        if (!loaded)
            return std::unexpected(loaded.error());

        const auto definition = ReadReactionTableDefinition(*loaded);
        if (!definition)
            return std::unexpected(definition.error());

        return BakeReactionTable(*definition);
    }

    const Coupling* Find(const BakedReactionTable& table, CouplingForm form, std::string_view key) {
        for (const Coupling& coupling : table.couplings) {
            if (coupling.form == form && coupling.key == key)
                return &coupling;
        }

        return nullptr;
    }

    bool AnyWarning(const BakedReactionTable& table, std::string_view part) {
        for (const std::string& warning : table.warnings) {
            if (warning.find(part) != std::string::npos)
                return true;
        }

        return false;
    }

    double PeakEfficiency(double coupling) {
        const double slip = std::sqrt(1.0 - (coupling * coupling));

        return (1.0 - slip) / (1.0 + slip);
    }

    // --- 1 つの形: 2 つの分類が同じ型・同じ検査 ---

    void TestOneForm() {
        const auto table = Bake(R"(
            drags = {
                salt_thermal = { flow = "heat", drags = "species:salt", coupling = "0.3", within = "water" },
                over = { flow = "species:salt", drags = "heat", coupling = "-1.5" },
                same = { flow = "heat", drags = "species:air", coupling = "0.9" },
            },
            machines = {
                pump = { store = "work", push = "gas", coupling = "0.9", cost_j_per_mol = 3000, speed_w = 3000 },
                over = { store = "work", push = "species:salt", coupling = "1.2", cost_j_per_mol = 3000, speed_w = 3000 },
                broken = { store = "heat", push = "gas", coupling = 1, cost_j_per_mol = 0, speed_w = 3000 },
            },
        )");
        EXPECT(table.has_value());
        if (!table) {
            std::printf("  %s\n", table.error().c_str());
            return;
        }

        EXPECT(table->couplings.size() == 6);
        EXPECT(table->couplings.front().form == CouplingForm::Drag);  // drags → machines・鍵の順
        EXPECT(table->couplings.back().form == CouplingForm::Machine);

        const Coupling* thermal = Find(*table, CouplingForm::Drag, "salt_thermal");
        EXPECT(thermal != nullptr);
        if (thermal != nullptr) {
            EXPECT(thermal->couplingQ16 == 19660);  // 0.3 × 65536 = 19660.8 を 0 の向きに
            EXPECT(thermal->writtenCouplingQ16 == thermal->couplingQ16);
            EXPECT(thermal->driver.kind == CouplingQuantityKind::Heat);
            EXPECT(thermal->driven ==
                   (CouplingQuantity{.kind = CouplingQuantityKind::Species, .species = table->SpeciesId("salt")}));
            EXPECT(thermal->withinSpecies == table->SpeciesId("water"));
        }

        // 書いた |q| > 1 は、どちらの分類でも警告して ±1 に押さえる(同じ 1 つの検査)
        const Coupling* dragOver = Find(*table, CouplingForm::Drag, "over");
        const Coupling* machineOver = Find(*table, CouplingForm::Machine, "over");
        EXPECT(dragOver != nullptr && machineOver != nullptr);
        if (dragOver != nullptr && machineOver != nullptr) {
            EXPECT(dragOver->writtenCouplingQ16 == -98304);
            EXPECT(dragOver->couplingQ16 == -COUPLING_ONE_Q16);
            EXPECT(machineOver->writtenCouplingQ16 == 78643);  // 1.2 × 65536 = 78643.2
            EXPECT(machineOver->couplingQ16 == COUPLING_ONE_Q16);
            EXPECT(dragOver->peakEfficiencyQ16 == COUPLING_ONE_Q16);
            EXPECT(machineOver->peakEfficiencyQ16 == COUPLING_ONE_Q16);
        }

        EXPECT(AnyWarning(*table, "drags.over: 結合の強さ |q| > 1"));
        EXPECT(AnyWarning(*table, "machines.over: 結合の強さ |q| > 1"));
        EXPECT(!AnyWarning(*table, "drags.salt_thermal"));
        EXPECT(!AnyWarning(*table, "machines.pump"));

        // 同じ q は分類によらず同じ効く q・同じ効率の上限
        const Coupling* dragSame = Find(*table, CouplingForm::Drag, "same");
        const Coupling* pump = Find(*table, CouplingForm::Machine, "pump");
        EXPECT(dragSame != nullptr && pump != nullptr);
        if (dragSame != nullptr && pump != nullptr) {
            EXPECT(dragSame->couplingQ16 == pump->couplingQ16);
            EXPECT(dragSame->peakEfficiencyQ16 == pump->peakEfficiencyQ16);
        }

        // 押す構造の数は模型(tests/push_model_test の PumpEntry: w = 3 mJ/µmol・P = 50 J/刻み)と同じ刻みの単位
        if (pump != nullptr) {
            EXPECT(pump->couplingQ16 == 58982);
            EXPECT(pump->costQ16 == 3 * COUPLING_ONE_Q16);
            EXPECT(pump->speedPerTick == 50000);
            EXPECT(!pump->disabled);
            const double peak = static_cast<double>(pump->peakEfficiencyQ16) / 65536.0;
            EXPECT(std::abs(peak - PeakEfficiency(58982.0 / 65536.0)) < 1.0e-3);
        }

        const Coupling* broken = Find(*table, CouplingForm::Machine, "broken");
        EXPECT(broken != nullptr && broken->disabled && broken->speedPerTick == 0);
        EXPECT(AnyWarning(*table, "machines.broken: 費用"));
    }

    // --- q の書き方 ---

    void TestCouplingText() {
        const auto read = [](ScriptValue value) {
            return ReadCouplingQ16(value, "q");
        };
        EXPECT(read(ScriptValue::MakeString("1")) == COUPLING_ONE_Q16);
        EXPECT(read(ScriptValue::MakeNumber(1.0)) == COUPLING_ONE_Q16);
        EXPECT(read(ScriptValue::MakeNumber(-1.0)) == -COUPLING_ONE_Q16);
        EXPECT(read(ScriptValue::MakeString("-0.25")) == -16384);
        EXPECT(read(ScriptValue::MakeString("0.3")) == 19660);
        EXPECT(read(ScriptValue::MakeString("1e-30")) == 0);
        EXPECT(read(ScriptValue::MakeString("1000")) == 1000 * COUPLING_ONE_Q16);
        EXPECT(!read(ScriptValue::MakeString("1001")).has_value());
        EXPECT(!read(ScriptValue::MakeString("2e5")).has_value());
        EXPECT(
            !read(ScriptValue::MakeNumber(0.9)).has_value());  // 小数は文字列で(機械ごとに丸めが変わる余地を作らない)
        EXPECT(!read(ScriptValue::MakeString("abc")).has_value());

        // 押さえる関数(引きずりと押す構造の模型も使う)
        EXPECT(!ClampCouplingQ16(COUPLING_ONE_Q16).clamped);
        EXPECT(ClampCouplingQ16(COUPLING_ONE_Q16 + 1).clamped);
        EXPECT(ClampCouplingQ16(-COUPLING_ONE_Q16 - 1).couplingQ16 == -COUPLING_ONE_Q16);
        EXPECT(PeakEfficiencyQ16(0) == 0);
    }

    // --- 誤り ---

    void ExpectError(std::string_view couplings, std::string_view part) {
        const auto table = Bake(couplings);
        EXPECT(!table.has_value());
        if (!table)
            EXPECT(Contains(table.error(), part));
    }

    void TestErrors() {
        // 形の誤り(読み手)
        ExpectError(R"(drags = { d = { flow = "heat", drags = "species:salt", coupling = "0.3", strength = 1 } })",
                    "drags.d");
        ExpectError(R"(drags = { d = { flow = "heat", drags = "species:salt", coupling = 0.3 } })", "drags.d.coupling");
        ExpectError(R"(drags = { d = { flow = "steam", drags = "species:salt", coupling = "0.3" } })", "drags.d.flow");
        ExpectError(R"(drags = { d = { flow = "heat", drags = "species:salt" } })", "coupling");
        ExpectError(
            R"(machines = { m = { store = "work", push = "gas", coupling = "5000", cost_j_per_mol = 1, speed_w = 1 } })",
            "machines.m.coupling");
        ExpectError(R"(machines = { m = { store = "work", push = "gas", coupling = "1", speed_w = 1 } })",
                    "cost_j_per_mol");

        // ベイクの誤り
        ExpectError(R"(drags = { d = { flow = "heat", drags = "species:mud", coupling = "0.3" } })", "物質 mud");
        ExpectError(R"(drags = { d = { flow = "heat", drags = "species:salt", coupling = "0.3", within = "mud" } })",
                    "物質 mud");
        ExpectError(R"(drags = { d = { flow = "heat", drags = "heat", coupling = "0.3" } })", "同じ");
        ExpectError(R"(drags = { d = { flow = "work", drags = "heat", coupling = "0.3" } })", "使えない");
        ExpectError(R"(drags = { d = { flow = "heat", drags = "gas", coupling = "0.3" } })", "使えない");
        ExpectError(
            R"(machines = { m = { store = "gas", push = "heat", coupling = "1", cost_j_per_mol = 1, speed_w = 1 } })",
            "使えない");
        ExpectError(
            R"(machines = { m = { store = "work", push = "work", coupling = "1", cost_j_per_mol = 1, speed_w = 1 } })",
            "使えない");

        // 分類が無ければ空(ブランクの世界。D-455)
        const auto empty = Bake("");
        EXPECT(empty.has_value() && empty->couplings.empty());
        const auto blank = LoadBlankReactionTable();
        EXPECT(blank.has_value() && blank->table.couplings.empty());
    }

    // --- ホットリロード ---

    void WriteText(const fs::path& path, std::string_view text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    std::string MachineInit(std::string_view coupling) {
        return InitWith(std::format(
            R"(machines = {{ pump = {{ store = "work", push = "gas", coupling = "{}", cost_j_per_mol = 3000, speed_w = 3000 }} }},)",
            coupling));
    }

    void TestHotReload(const fs::path& root) {
        const fs::path package = root / "coupling_world";
        WriteText(package / "package.luau", "return { format = 1, game = true }");
        WriteText(package / "init.luau", MachineInit("0.5"));

        const ReactionTableSource source{.packageRoot = root};
        auto initial = LoadReactionTable(source);
        EXPECT(initial.has_value());
        if (!initial) {
            std::printf("  読めない: %s\n", initial.error().c_str());
            return;
        }

        EXPECT(initial->table.couplings.size() == 1 && initial->table.couplings[0].couplingQ16 == 32768);
        const uint64_t firstVersion = initial->tableVersion;
        ReactionTableHotReload reload(source, std::make_shared<const LoadedReactionTable>(std::move(*initial)));

        // 書きすぎた値を書く → 読み直してベイクが押さえる(表は当たる。D-487 ②)
        WriteText(package / "init.luau", MachineInit("1.5"));
        EXPECT(reload.Poll().state == HotReloadState::Waiting);
        const HotReloadResult result = reload.Poll();
        EXPECT(result.state == HotReloadState::Reloaded);
        if (result.state != HotReloadState::Reloaded) {
            std::printf("  読み直せない: %s\n", result.message.c_str());
            return;
        }

        const BakedReactionTable& table = result.table->table;
        EXPECT(result.table->tableVersion != firstVersion);
        EXPECT(table.couplings.size() == 1);
        EXPECT(table.couplings[0].writtenCouplingQ16 == 98304);
        EXPECT(table.couplings[0].couplingQ16 == COUPLING_ONE_Q16);
        EXPECT(AnyWarning(table, "machines.pump: 結合の強さ |q| > 1"));

        // 再生ファイルの中身から作り直しても同じ結合
        const auto rebuilt = RebuildReactionTable(result.table->tableBytes, result.table->tableVersion);
        EXPECT(rebuilt.has_value() && rebuilt->table.couplings == table.couplings);
    }

}  // namespace

int main() {
    TestOneForm();
    TestCouplingText();
    TestErrors();

    const fs::path root = fs::temp_directory_path() / "bicameral_coupling_table_test";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    TestHotReload(root);
    fs::remove_all(root, ignored);

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("coupling_table: すべて通過\n");

    return 0;
}
