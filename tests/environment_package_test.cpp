// environment_package_test.cpp — 世界のパッケージの環境の値(分類 environment → sim::WorldPhysics。T-0400・ADR-0176)を CPU だけで確かめる。
// 見るもの: サンプルの世界のパッケージ(data/packages/combustion_test)の環境が、標準の値(StandardWorldPhysics。試験の場面が使う)と同じ /
//           分類が無い世界はブランク(全部 0)/ 環境だけのパッケージで別の重力・音速の基準を渡せる /
//           知らない鍵・整数でない数・範囲の外(重力の上限・音速の基準の安定の上限)を、鍵と書いたパッケージの名前付きで落とす /
//           ランタイムの入り口(LoadReactionTable)のベイクした表が環境を持ち、ブランクの表は 0、再生の作り直しでも同じ。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "script/environment_package.h"

#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "script/luau_sandbox.h"
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
    std::expected<sim::WorldPhysics, std::string> ReadEnvironment(const std::vector<PackageSource>& packages) {
        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        if (!sandbox)
            return std::unexpected(sandbox.error());

        (*sandbox)->Seal();
        const auto loaded = LoadPackages(**sandbox, packages, PackageLoadOptions{});
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
        return left.gravityMicrometersPerSecond2 == right.gravityMicrometersPerSecond2 &&
               left.soundSpeedMillimetersPerSecond == right.soundSpeedMillimetersPerSecond;
    }

    // --- 試験 ---

    // サンプルの世界のパッケージの環境 = 標準の値(試験の場面が StandardWorldPhysics で書いた結果がサンプルの世界と同じになる)
    void TestSamplePackageIsStandard() {
        auto package = ReadPackageFolder(fs::path(BICAMERAL_DATA_PACKAGES_DIR) / "combustion_test");
        if (!package) {
            std::printf("FAILED: 試験のパッケージを読めない: %s\n", package.error().c_str());
            ++failureCount;

            return;
        }

        const auto world = ReadEnvironment({std::move(*package)});
        if (!world) {
            std::printf("FAILED: 環境を読めない: %s\n", world.error().c_str());
            ++failureCount;

            return;
        }

        std::printf("sample environment: gravity %u um/s^2, sound speed %u mm/s\n", world->gravityMicrometersPerSecond2,
                    world->soundSpeedMillimetersPerSecond);
        EXPECT(SameWorld(*world, sim::StandardWorldPhysics()));
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

    // ランタイムの入り口: ベイクした表の environment(窓の物理の重力はここから。frame_loop.cpp)
    void TestBakedTableCarriesEnvironment() {
        const auto loaded = LoadReactionTable({.packageRoot = fs::path(BICAMERAL_DATA_PACKAGES_DIR)});
        if (!loaded) {
            std::printf("FAILED: 表を読めない: %s\n", loaded.error().c_str());
            ++failureCount;

            return;
        }

        EXPECT(SameWorld(loaded->table.environment, sim::StandardWorldPhysics()));

        const auto rebuilt = RebuildReactionTable(loaded->tableBytes, loaded->tableVersion);
        EXPECT(rebuilt && SameWorld(rebuilt->table.environment, sim::StandardWorldPhysics()));

        const auto blank = LoadBlankReactionTable();
        EXPECT(blank && SameWorld(blank->table.environment, sim::WorldPhysics{}));
    }

}  // namespace

int main() {
    TestSamplePackageIsStandard();
    TestBlankWithoutCategory();
    TestCustomEnvironment();
    TestRejectsBrokenEnvironment();
    TestBakedTableCarriesEnvironment();
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("environment_package: すべて通過\n");

    return 0;
}
