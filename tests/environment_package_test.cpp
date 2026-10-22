// environment_package_test.cpp — 世界のパッケージの環境の値(分類 environment → sim::WorldPhysics。T-0400・ADR-0176)を CPU だけで確かめる。
// 見るもの: サンプルの世界のパッケージ(data/packages/combustion_test)の環境が、標準の値(StandardWorldPhysics。試験の場面が使う)と同じ /
//           分類が無い世界はブランク(全部 0)/ 環境だけのパッケージで別の重力・音速の基準を渡せる /
//           知らない鍵・整数でない数・範囲の外(重力の上限・音速の基準の安定の上限)を、鍵と書いたパッケージの名前付きで落とす /
//           ランタイムの入り口(LoadReactionTable)のベイクした表が環境を持ち、ブランクの表は 0、再生の作り直しでも同じ /
//           気体の圧力の伝わる速さ gas_pressure_speed("sound" / "instant"。T-0804)の読み込みと誤り /
//           ホットリロードの検査(CheckHotReloadCompatible)が、器の範囲の外の環境(コードで作った表)を理由付きで当てない(T-0444)。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "script/environment_package.h"

#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "reaction_test_table.h"
#include "script/luau_sandbox.h"
#include "script/reaction_hot_reload.h"
#include "script/reaction_table_loader.h"
#include "sim/gas_projection.h"
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

    // --- 道具 ---

    bool Contains(std::string_view text, std::string_view part) {
        if (text.find(part) != std::string_view::npos)
            return true;

        std::printf("  '%.*s' に '%.*s' が無い\n", static_cast<int>(text.size()), text.data(),
                    static_cast<int>(part.size()), part.data());

        return false;
    }

    // init だけのパッケージ(依存なし)
    PackageSource InitPackage(std::string name, std::string init) {
        PackageSource package{.name = std::move(name), .files = {}};
        package.files.emplace("package.luau", "return { format = 1 }");
        package.files.emplace("init.luau", std::move(init));

        return package;
    }

    // パッケージ群 → 読み込み → 環境の値。読まなかったパッケージがあれば誤りにする
    std::expected<sim::WorldPhysics, std::string> ReadEnvironment(const std::vector<PackageSource>& packages,
                                                                  std::string basePackage = {}) {
        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        if (!sandbox)
            return std::unexpected(sandbox.error());

        (*sandbox)->Seal();
        const auto loaded = LoadPackages(**sandbox, packages,
                                         PackageLoadOptions{.basePackage = std::move(basePackage)});
        if (!loaded)
            return std::unexpected(loaded.error());

        if (!loaded->rejected.empty())
            return std::unexpected(loaded->rejected.front().package + ": " + loaded->rejected.front().reason);

        return ReadWorldEnvironment(*loaded);
    }

    std::expected<sim::WorldPhysics, std::string> ReadEnvironmentInit(std::string init) {
        return ReadEnvironment({InitPackage("world", std::move(init))});
    }

    bool SameWorld(const sim::WorldPhysics& left, const sim::WorldPhysics& right) {
        return left == right;  // 重力・音速の基準・世界の端(T-0710)
    }

    // earth_environment の値 = 標準の値 + 気体の乱れの粘りの仮の値(T-0775。気体の試験の場面は粘りを書かない)
    sim::WorldPhysics EarthWorldPhysics() {
        sim::WorldPhysics world = sim::StandardWorldPhysics();
        world.gasEddyViscosityMm2PerS = sim::STANDARD_GAS_EDDY_VISCOSITY_MM2_PER_S;
        return world;
    }

    bool HasWarning(const sim::BakedReactionTable& table, std::string_view part) {
        for (const std::string& warning : table.warnings) {
            if (warning.find(part) != std::string::npos)
                return true;
        }

        return false;
    }

    // --- 試験 ---

    // サンプルの世界のパッケージの環境 = 標準の値(試験の場面が StandardWorldPhysics で書いた結果がサンプルの世界と同じになる)。
    // 気体の乱れの粘りだけは違う: earth_environment の仮の値(T-0775)を、気体の試験の場面は書かない(粘りの無い基準で比べる)
    void TestSamplePackageIsStandard() {
        // 試験のパッケージは環境の値を書かず、依存の標準パッケージ earth_environment が書く(T-0498)
        const auto world = ReadEnvironment(sim::ReadCombustionTestPackages(), "combustion_test");
        if (!world) {
            std::printf("FAILED: 環境を読めない: %s\n", world.error().c_str());
            ++failureCount;

            return;
        }

        std::printf("sample environment: gravity %u um/s^2, sound speed %u mm/s\n", world->gravityMicrometersPerSecond2,
                    world->soundSpeedMillimetersPerSecond);
        EXPECT(SameWorld(*world, EarthWorldPhysics()));
    }

    // 分類が無い世界 = ブランク(重力 0・音速の基準なし。D-455・D-480)
    void TestBlankWithoutCategory() {
        const auto world = ReadEnvironmentInit("return { elements = {} }");
        EXPECT(world && SameWorld(*world, sim::WorldPhysics{}));

        const auto empty = ReadWorldEnvironment(PackageSetResult{});
        EXPECT(empty && SameWorld(*empty, sim::WorldPhysics{}));
    }

    // 環境だけのパッケージ: 月の重力・書かない音速の基準は 0
    void TestCustomEnvironment() {
        const auto moon = ReadEnvironmentInit("return { environment = { gravity_um_per_s2 = 1620000 } }");
        EXPECT(moon && moon->gravityMicrometersPerSecond2 == 1620000 && moon->soundSpeedMillimetersPerSecond == 0);

        // 上限ちょうどは通る(重力 100 m/s²・音速の基準は既定の小刻み 24 で 432 m/s)
        const auto limits = ReadEnvironmentInit(std::format(
            "return {{ environment = {{ gravity_um_per_s2 = {}, sound_speed_mm_per_s = {} }} }}",
            fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2, sim::GasSoundSpeedLimitMmPerS(sim::GasConfig{}.substeps)));
        EXPECT(limits && limits->gravityMicrometersPerSecond2 == fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2 &&
               limits->soundSpeedMillimetersPerSecond == 432000);
    }

    void ExpectRejected(std::string init, std::string_view part) {
        const auto world = ReadEnvironmentInit(std::move(init));
        EXPECT(!world);
        if (world)
            return;

        std::printf("rejected: %s\n", world.error().c_str());
        EXPECT(Contains(world.error(), part));
        EXPECT(Contains(world.error(), "パッケージ world"));
    }

    // 形の誤り: 知らない鍵(綴りの誤り)・整数でない数・範囲の外・数でない値
    void TestRejectsBrokenEnvironment() {
        ExpectRejected("return { environment = { gravity = 9806650 } }", "environment.gravity");
        ExpectRejected("return { environment = { gravity_um_per_s2 = 9806650.5 } }", "整数");
        ExpectRejected("return { environment = { gravity_um_per_s2 = -1 } }", "範囲");
        ExpectRejected("return { environment = { gravity_um_per_s2 = 100000001 } }", "範囲");
        ExpectRejected("return { environment = { sound_speed_mm_per_s = 432001 } }", "sound_speed_mm_per_s");
        ExpectRejected("return { environment = { sound_speed_mm_per_s = \"340000\" } }", "整数");
    }

    // 世界の端(T-0710・土台の決定 5): 書かない面は wall・値は "open" か "wall"・知らない面や表でない値は誤り
    void TestWorldEdges() {
        const auto blank = ReadEnvironmentInit("return { elements = {} }");
        EXPECT(blank && blank->edges.AllWall());

        const auto field = ReadEnvironmentInit(
            "return { environment = { world_edges = { top = \"open\", sides = \"open\" } } }");
        EXPECT(field && field->edges.top == sim::WorldEdge::Open && field->edges.bottom == sim::WorldEdge::Wall &&
               field->edges.sides == sim::WorldEdge::Open);

        ExpectRejected("return { environment = { world_edges = { top = \"sky\" } } }", "\"open\" か \"wall\"");
        ExpectRejected("return { environment = { world_edges = { up = \"open\" } } }", "world_edges");
        ExpectRejected("return { environment = { world_edges = \"open\" } }", "world_edges");
    }

    // 釣り合う家族の V の回数(T-0955): 書かない世界・書かない法則は 2。1〜16 の整数。外・知らない法則・小数は名前付きで誤り。
    // 決まった回数の道の陰の圧力の solver.cycles へ入る(許容差で止める道は変えない)
    void TestBalanceCycles() {
        const auto blank = ReadEnvironmentInit("return { elements = {} }");
        EXPECT(blank && blank->balanceCycles == sim::BalanceCycles{});
        EXPECT(blank && blank->balanceCycles.gasPressure == 2 && blank->balanceCycles.support == 2);

        const auto water = ReadEnvironmentInit("return { environment = { balance_cycles = { liquid_pressure = 4 } } }");
        EXPECT(water && water->balanceCycles.liquidPressure == 4 && water->balanceCycles.gasPressure == 2 &&
               water->balanceCycles.support == 2);

        const auto all = ReadEnvironmentInit(
            "return { environment = { balance_cycles = { gas_pressure = 1, liquid_pressure = 16, support = 3 } } }");
        EXPECT(all && all->balanceCycles.gasPressure == 1 && all->balanceCycles.liquidPressure == 16 &&
               all->balanceCycles.support == 3);

        ExpectRejected("return { environment = { balance_cycles = { gas_pressure = 17 } } }", "範囲");
        ExpectRejected("return { environment = { balance_cycles = { gas_pressure = 0 } } }", "範囲");
        ExpectRejected("return { environment = { balance_cycles = { support = 2.5 } } }", "整数");
        ExpectRejected("return { environment = { balance_cycles = { water = 4 } } }", "balance_cycles");
        ExpectRejected("return { environment = { balance_cycles = 4 } }", "balance_cycles");

        if (all) {
            sim::GasProjectionOptions fixed;
            fixed.fixedCycles = true;
            sim::ApplyBalanceCycles(*all, fixed);
            EXPECT(fixed.solver.cycles == 1);

            sim::GasProjectionOptions tolerance;  // 許容差で止める道(上限 8)は変えない
            sim::ApplyBalanceCycles(*all, tolerance);
            EXPECT(tolerance.solver.cycles == 8);
        }

        sim::WorldPhysics tooMany;
        tooMany.balanceCycles.support = sim::BALANCE_CYCLES_MAX + 1;
        const auto checked = sim::CheckWorldPhysics(tooMany);
        EXPECT(!checked);
        if (!checked)
            EXPECT(Contains(checked.error(), "V の回数"));
    }

    // 細かい所の半径と移り目(T-0954): 書かない世界・書かない欄は既定。長さは 0〜1000 km の整数(遠くの葉は 1 以上)・段の数は 1〜8。
    // 外・知らない欄・小数は名前付きで誤り
    void TestFineRegion() {
        const auto blank = ReadEnvironmentInit("return { elements = {} }");
        EXPECT(blank && blank->fineRegion == sim::FineRegion{});
        EXPECT(blank && blank->fineRegion.radiusMm == sim::FINE_REGION_RADIUS_DEFAULT_MM &&
               blank->fineRegion.farLeafMm == sim::FINE_REGION_FAR_LEAF_DEFAULT_MM &&
               blank->fineRegion.levelsPerDoubling == sim::FINE_REGION_LEVELS_PER_DOUBLING_DEFAULT);

        const auto wide = ReadEnvironmentInit("return { environment = { fine_region = { radius_mm = 256000 } } }");
        EXPECT(wide && wide->fineRegion.radiusMm == 256000 &&
               wide->fineRegion.farLeafMm == sim::FINE_REGION_FAR_LEAF_DEFAULT_MM);

        const auto all = ReadEnvironmentInit(
            "return { environment = { fine_region = { radius_mm = 64000, "
            "far_leaf_mm = 16000, levels_per_doubling = 2 } } }");
        EXPECT(all && all->fineRegion.radiusMm == 64000 && all->fineRegion.farLeafMm == 16000 &&
               all->fineRegion.levelsPerDoubling == 2);

        ExpectRejected("return { environment = { fine_region = { levels_per_doubling = 0 } } }", "範囲");
        ExpectRejected("return { environment = { fine_region = { levels_per_doubling = 9 } } }", "範囲");
        ExpectRejected("return { environment = { fine_region = { far_leaf_mm = 0 } } }", "範囲");
        ExpectRejected("return { environment = { fine_region = { radius_mm = -1 } } }", "範囲");
        ExpectRejected("return { environment = { fine_region = { radius_mm = 1.5 } } }", "整数");
        ExpectRejected("return { environment = { fine_region = { radius = 4 } } }", "fine_region");
        ExpectRejected("return { environment = { fine_region = 4 } }", "fine_region");

        sim::WorldPhysics zero;
        zero.fineRegion.levelsPerDoubling = 0;
        const auto checked = sim::CheckWorldPhysics(zero);
        EXPECT(!checked);
        if (!checked)
            EXPECT(Contains(checked.error(), "移り目"));
    }

    // 気体の圧力の伝わる速さ(T-0804・D-526 ②): 書かない世界は本物の音速(D-519)・"sound" / "instant"・ほかの文字や数は名前付きで誤り。
    // 最速がまだ効かない置き場の知らせは最速の世界だけ
    void TestGasPressureSpeed() {
        const auto blank = ReadEnvironmentInit("return { elements = {} }");
        EXPECT(blank && blank->gasPressureSpeed == sim::GasPressureSpeed::Sound);

        const auto sound = ReadEnvironmentInit("return { environment = { gas_pressure_speed = \"sound\" } }");
        EXPECT(sound && sound->gasPressureSpeed == sim::GasPressureSpeed::Sound);
        EXPECT(sound && *sound == sim::WorldPhysics{});  // 書かないのと同じ(表の値は全部同じ)

        const auto instant = ReadEnvironmentInit("return { environment = { gas_pressure_speed = \"instant\" } }");
        EXPECT(instant && instant->gasPressureSpeed == sim::GasPressureSpeed::Instant);

        ExpectRejected("return { environment = { gas_pressure_speed = \"fast\" } }", "environment.gas_pressure_speed");
        ExpectRejected("return { environment = { gas_pressure_speed = 0 } }", "\"sound\"(本物の音速)か \"instant\"");
        ExpectRejected("return { environment = { gas_pressure_speed = { \"instant\" } } }", "gas_pressure_speed");

        EXPECT(sim::GasInstantNotYetNotice(sim::WorldPhysics{}, "多重解像度の世界の気体").empty());
        if (instant) {
            const std::string notice = sim::GasInstantNotYetNotice(*instant, "多重解像度の世界の気体");
            std::printf("notice: %s\n", notice.c_str());
            EXPECT(notice.starts_with(sim::GAS_INSTANT_NOT_YET_PREFIX));
            EXPECT(Contains(notice, "多重解像度の世界の気体"));
        }
    }

    // ベイクの知らせ(T-0710・土台の設計 §1.4): 液体を置いて重力 0 は警告・端が全部 wall で基準の大気が無いのは注意。
    // open があるのに基準の大気が無い(外は真空)のも注意(T-0772・ADR-0338)。セルを置かない世界(ブランク)は知らせない
    void TestWorldNotices() {
        sim::ReactionTableDefinition definition = sim::MakeCombustionTestTable();
        definition.environment = {};
        definition.atmosphere = {};
        definition.worldLayout = {};
        definition.playMetrics = {};
        definition.worldLayout.cellRegions.push_back({.name = "pond",
                                                      .contents = {{.species = "water", .micromoles = 1000000}},
                                                      .temperatureMilliKelvin = 293150});

        const auto still = sim::BakeReactionTable(definition);
        EXPECT(still && HasWarning(*still, "重力") && HasWarning(*still, "注意: 世界の端"));
        if (!still)
            std::printf("FAILED: ベイクできない: %s\n", still.error().c_str());

        definition.environment.gravityMicrometersPerSecond2 = 9806650;
        definition.environment.edges.top = sim::WorldEdge::Open;
        const auto open = sim::BakeReactionTable(definition);
        EXPECT(open && !HasWarning(*open, "重力") && !HasWarning(*open, "全部 wall") &&
               HasWarning(*open, "基準の大気が無い"));

        definition.environment = {};
        definition.worldLayout = {};
        const auto empty = sim::BakeReactionTable(definition);
        EXPECT(empty && !HasWarning(*empty, "重力") && !HasWarning(*empty, "世界の端"));
    }

    // ランタイムの入り口: ベイクした表の environment(窓の物理の重力はここから。frame_loop.cpp)
    void TestBakedTableCarriesEnvironment() {
        const auto loaded = LoadReactionTable({.packageRoot = fs::path(BICAMERAL_DATA_PACKAGES_DIR)});
        if (!loaded) {
            std::printf("FAILED: 表を読めない: %s\n", loaded.error().c_str());
            ++failureCount;

            return;
        }

        EXPECT(SameWorld(loaded->table.environment, EarthWorldPhysics()));

        const auto rebuilt = RebuildReactionTable(loaded->tableBytes, loaded->tableVersion);
        EXPECT(rebuilt && SameWorld(rebuilt->table.environment, EarthWorldPhysics()));

        const auto blank = LoadBlankReactionTable();
        EXPECT(blank && SameWorld(blank->table.environment, sim::WorldPhysics{}));
    }

    // ホットリロードの検査(T-0444・ADR-0194): 環境の値は差し替えの刻みから物理と気体に当たるので、器の範囲の外は理由付きで当てない
    void TestHotReloadChecksEnvironment() {
        const auto loaded = LoadReactionTable({.packageRoot = fs::path(BICAMERAL_DATA_PACKAGES_DIR)});
        if (!loaded) {
            std::printf("FAILED: 表を読めない: %s\n", loaded.error().c_str());
            ++failureCount;

            return;
        }

        const sim::BakedReactionTable& current = loaded->table;
        sim::BakedReactionTable moon = current;
        moon.environment.gravityMicrometersPerSecond2 = 1620000;
        moon.environment.soundSpeedMillimetersPerSecond = 300000;
        EXPECT(CheckHotReloadCompatible(current, moon).has_value());
        EXPECT(CheckHotReloadCompatible(current, moon, SpeciesChangePolicy::Remap).has_value());

        sim::BakedReactionTable tooFast = current;
        tooFast.environment.soundSpeedMillimetersPerSecond = static_cast<uint32_t>(sim::GasSoundSpeedLimitMmPerS(
                                                                 sim::GasConfig{}.substeps)) +
                                                             1;
        const auto fastResult = CheckHotReloadCompatible(current, tooFast, SpeciesChangePolicy::Remap);
        EXPECT(!fastResult);
        if (!fastResult) {
            std::printf("rejected: %s\n", fastResult.error().c_str());
            EXPECT(Contains(fastResult.error(), "音速の基準"));
        }

        sim::BakedReactionTable tooHeavy = current;
        tooHeavy.environment.gravityMicrometersPerSecond2 = fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2 + 1;
        const auto heavyResult = CheckHotReloadCompatible(current, tooHeavy);
        EXPECT(!heavyResult);
        if (!heavyResult)
            EXPECT(Contains(heavyResult.error(), "重力"));
    }

}  // namespace

int main() {
    TestSamplePackageIsStandard();
    TestBlankWithoutCategory();
    TestCustomEnvironment();
    TestRejectsBrokenEnvironment();
    TestWorldEdges();
    TestGasPressureSpeed();
    TestBalanceCycles();
    TestFineRegion();
    TestWorldNotices();
    TestBakedTableCarriesEnvironment();
    TestHotReloadChecksEnvironment();
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("environment_package: すべて通過\n");

    return 0;
}
