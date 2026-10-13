// world_layout_test.cpp — 世界の組み立て(パッケージの world_cells・world_bodies。T-0398・ADR-0172)を CPU で確かめる。
//
// 確かめること:
//   - 試験のパッケージ(data/packages/combustion_test)の組み立てから作った初めのセル(sim::MakeProbeInitialWorld)が、
//     前の C++ の組み立て(このファイルに写した OldProbeInitialWorld。空気の中の木箱)とビット一致
//   - 物(sim::MakeLayoutPhysicsScene)が前の C++ の積み木の場面(OldProbeStackScene)と同じ(物の数・番号の順・形・質量・位置)
//   - ランタイムの入り口(型検査あり。script::LoadReactionTable)で読んでも同じで、再生ファイルの中身(TableBytes)から作り直した表
//     (RebuildReactionTable)でも同じ初めの世界と物になる
//   - ブランクの表は組み立てが空(セルは全部空・物なし)
//   - Mod が領域と物を鍵で足せる(層の順に重ね書き・番号は order の順)。綴りの誤り・知らない物質・半端な場所・小数は誤りになる
// GPU の刻みは今まで通り gpu_probe_* が同じ初めの世界で確かめる(初めの世界の作り方だけが変わった)。
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "common/fixed.hlsli"
#include "common/units.hlsli"
#include "core/aliases.h"
#include "reaction_test_table.h"
#include "script/luau_sandbox.h"
#include "script/reaction_package.h"
#include "script/reaction_table_loader.h"
#include "sim/physics_scene.h"
#include "sim/probe_sim.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    bool SameCells(std::span<const reaction::RxCell> left, std::span<const reaction::RxCell> right) {
        return left.size() == right.size() && std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
    }

    // --- 前の C++ の組み立て(T-0089・T-0098 の値。比べるためだけに試験へ写した)---

    std::vector<reaction::RxCell> OldProbeInitialWorld(const BakedReactionTable& table) {
        constexpr uint32_t CRATE_SIZE = 8;
        constexpr uint32_t CRATE_BEGIN = (PROBE_GRID_SIZE - CRATE_SIZE) / 2;
        constexpr uint32_t CRATE_END = CRATE_BEGIN + CRATE_SIZE;
        const auto inCrate = [](uint32_t value) {
            return value >= CRATE_BEGIN && value < CRATE_END;
        };
        const auto onWall = [](uint32_t value) {
            return value == CRATE_BEGIN || value == CRATE_END - 1;
        };

        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t oxygen = table.SpeciesId("oxygen");
        const uint32_t nitrogen = table.SpeciesId("nitrogen");
        const std::vector<SpeciesAmount> air = {{.species = oxygen, .amount = 1063800},
                                                {.species = nitrogen, .amount = 4014200}};
        const std::vector<SpeciesAmount> wall = {{.species = cellulose, .amount = 38600000},
                                                 {.species = oxygen, .amount = 983000},
                                                 {.species = nitrogen, .amount = 3697000}};
        const reaction::RxCell airCell = MakeReactionCell(table, air, 300000);
        const reaction::RxCell wallCell = MakeReactionCell(table, wall, 300000);

        std::vector<reaction::RxCell> cells(PROBE_CELL_COUNT, airCell);
        for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
            const uint32_t x = index % PROBE_GRID_SIZE;
            const uint32_t y = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
            const uint32_t z = index / PROBE_SLICE_CELL_COUNT;
            const bool inside = inCrate(x) && inCrate(y) && inCrate(z);
            if (inside && (onWall(x) || onWall(y) || onWall(z)))
                cells[index] = wallCell;
        }

        return cells;
    }

    std::vector<PhysicsSceneBody> OldProbeStackBodies() {
        constexpr int64_t METER = int64_t{1} << fx::POSITION_FRACTION_BITS;
        std::vector<PhysicsSceneBody> bodies;
        bodies.push_back(
            {.halfExtent = {50 * METER, METER / 2, 50 * METER}, .position = {16 * METER, -METER / 2, 16 * METER}});
        for (int64_t level = 0; level < 10; ++level) {
            bodies.push_back({.halfExtent = {METER / 2, METER / 2, METER / 2},
                              .massMilligrams = uint64_t{500} * 1000000,
                              .position = {10 * METER, (level * METER) + (METER / 2), (16 * METER) + (METER / 4)}});
        }

        return bodies;
    }

    bool SameBody(const PhysicsSceneBody& left, const PhysicsSceneBody& right) {
        return left.halfExtent == right.halfExtent && left.massMilligrams == right.massMilligrams &&
               left.position == right.position && left.rotation == right.rotation &&
               left.spawnTick == right.spawnTick && left.removeTick == right.removeTick;
    }

    bool SameAsOldScene(const PhysicsScene& scene) {
        const std::vector<PhysicsSceneBody> old = OldProbeStackBodies();
        if (scene.bodies.size() != old.size() || scene.frictionQ16 != (1u << 15))
            return false;

        for (size_t index = 0; index < old.size(); ++index) {
            if (!SameBody(scene.bodies[index], old[index]))
                return false;
        }

        return true;
    }

    // --- 試験のパッケージの組み立て = 前の C++ の組み立て ---

    void TestSampleMatchesOld() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        EXPECT(table.has_value());
        if (!table)
            return;

        EXPECT(table->worldLayout.cellRegions.size() == 2);
        EXPECT(SameCells(MakeProbeInitialWorld(*table), OldProbeInitialWorld(*table)));
        EXPECT(SameAsOldScene(MakeLayoutPhysicsScene(table->worldLayout)));
    }

    // --- ランタイムの入り口(型検査あり)と、再生ファイルの中身からの作り直し ---

    void TestRuntimeAndReplay() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        const auto loaded = script::LoadReactionTable({.packageRoot = fs::path(BICAMERAL_DATA_PACKAGES_DIR)});
        if (!loaded)
            std::printf("読めない: %s\n", loaded.error().c_str());

        EXPECT(table.has_value() && loaded.has_value());
        if (!table || !loaded)
            return;

        const std::vector<reaction::RxCell> expected = OldProbeInitialWorld(*table);
        EXPECT(SameCells(MakeProbeInitialWorld(loaded->table), expected));
        EXPECT(SameAsOldScene(MakeLayoutPhysicsScene(loaded->table.worldLayout)));

        const auto rebuilt = script::RebuildReactionTable(loaded->tableBytes, loaded->tableVersion);
        EXPECT(rebuilt.has_value());
        if (!rebuilt)
            return;

        EXPECT(SameCells(MakeProbeInitialWorld(rebuilt->table), expected));
        EXPECT(SameAsOldScene(MakeLayoutPhysicsScene(rebuilt->table.worldLayout)));
    }

    // --- ブランク ---

    void TestBlank() {
        const auto blank = script::LoadBlankReactionTable();
        EXPECT(blank.has_value());
        if (!blank)
            return;

        EXPECT(blank->table.worldLayout.cellRegions.empty() && blank->table.worldLayout.bodies.empty());
        EXPECT(SameCells(MakeProbeInitialWorld(blank->table), MakeBlankProbeWorld()));
        EXPECT(MakeLayoutPhysicsScene(blank->table.worldLayout).bodies.empty());
    }

    // --- Mod で足す・誤り ---

    // 試験のパッケージ + init.luau が modEntry を返す Mod を読んでベイクする
    std::expected<BakedReactionTable, std::string> BakeWithMod(const std::string& modEntry) {
        auto base = script::ReadPackageFolder(fs::path(BICAMERAL_DATA_PACKAGES_DIR) / "combustion_test");
        auto sandbox = script::LuauSandbox::Create(script::SandboxLimits{});
        if (!base || !sandbox)
            return std::unexpected(std::string("試験のパッケージか殻を作れない"));

        (*sandbox)->Seal();
        const script::PackageSource mod{.name = "layout_mod",
                                        .files = {{"package.luau", "return { format = 1 }"}, {"init.luau", modEntry}}};
        const std::vector<script::PackageSource> packages{std::move(*base), mod};
        const auto loaded = script::LoadPackages(**sandbox, packages, {.basePackage = "combustion_test"});
        if (!loaded)
            return std::unexpected(loaded.error());

        if (!loaded->rejected.empty())
            return std::unexpected(loaded->rejected.front().reason);

        const auto definition = script::ReadReactionTableDefinition(*loaded);
        if (!definition)
            return std::unexpected(definition.error());

        return BakeReactionTable(*definition);
    }

    bool FailsWith(const std::string& modEntry, std::string_view message) {
        const auto baked = BakeWithMod(modEntry);
        if (baked)
            return false;

        std::printf("  期待した誤り: %s\n", baked.error().c_str());
        return baked.error().find(message) != std::string::npos;
    }

    void TestModAddsAndErrors() {
        const auto baked = BakeWithMod(R"(return {
            world_cells = { hot_corner = { layer = 2, min_cell = { 0, 0, 0 }, size_cells = { 2, 1, 1 },
                                           amounts_umol = { nitrogen = 5000000 }, temperature_mk = 900000 } },
            world_bodies = { extra = { order = 2, size_mm = { 500, 500, 500 }, position_mm = { 1000, 250, 1000 }, mass_g = 1000 } },
        })");
        if (!baked)
            std::printf("読めない: %s\n", baked.error().c_str());

        EXPECT(baked.has_value());
        if (baked) {
            const std::vector<reaction::RxCell> world = MakeProbeInitialWorld(*baked);
            const std::vector<SpeciesAmount> hot = {{.species = baked->SpeciesId("nitrogen"), .amount = 5000000}};
            const reaction::RxCell hotCell = MakeReactionCell(*baked, hot, 900000);
            EXPECT(std::memcmp(&world[0], &hotCell, sizeof(hotCell)) == 0);
            EXPECT(std::memcmp(&world[1], &hotCell, sizeof(hotCell)) == 0);
            EXPECT(std::memcmp(&world[2], &hotCell, sizeof(hotCell)) != 0);  // 箱の外は空気のまま

            const PhysicsScene scene = MakeLayoutPhysicsScene(baked->worldLayout);
            EXPECT(scene.bodies.size() == 12 && scene.bodies.back().massMilligrams == 1000000);
        }

        const std::string cell = "amounts_umol = { oxygen = 1 }, temperature_mk = 300000";
        EXPECT(FailsWith("return { world_cells = { a = { " + cell + ", colour = 1 } } }", "知らない欄"));
        EXPECT(FailsWith("return { world_cells = { a = { amounts_umol = { unobtainium = 1 }, temperature_mk = 1 } } }",
                         "知らない物質 unobtainium"));
        EXPECT(FailsWith("return { world_cells = { a = { min_cell = { 0, 0, 0 }, " + cell + " } } }", "両方"));
        EXPECT(FailsWith("return { world_cells = { a = { shell_cells = 1, " + cell + " } } }", "箱"));
        EXPECT(FailsWith("return { world_cells = { a = { amounts_umol = { oxygen = 1.5 }, temperature_mk = 1 } } }",
                         "整数"));
        EXPECT(
            FailsWith("return { world_bodies = { b = { size_mm = { 1, 1 }, position_mm = { 0, 0, 0 }, mass_g = 1 } } }",
                      "x, y, z"));
    }

}  // namespace

int main() {
    TestSampleMatchesOld();
    TestRuntimeAndReplay();
    TestBlank();
    TestModAddsAndErrors();

    std::printf("world_layout_test: %s\n", failureCount == 0 ? "OK" : "FAILED");

    return failureCount == 0 ? 0 : 1;
}
