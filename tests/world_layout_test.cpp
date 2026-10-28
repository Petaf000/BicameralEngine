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
//   - 流れる所(D-510・T-0724): 区画の箱はやめ、基準の大気があれば気体・重力があれば液体が世界いっぱいで流れる(SetLayoutRegions)。
//     前の分類 gas_regions・liquid_regions は読まずに注意を出す
//   - 層(T-1236): settled_umol の層・書かなかった粒の読み方(舞う粒は知らせ・全部が層)・書き間違いの誤り・層つきで根を置く
// GPU の刻みは今まで通り gpu_probe_* が同じ初めの世界で確かめる(初めの世界の作り方だけが変わった)。
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/fixed.hlsli"
#include "common/units.hlsli"
#include "core/aliases.h"
#include "multires_test_scene.h"
#include "reaction_test_table.h"
#include "script/luau_sandbox.h"
#include "script/reaction_package.h"
#include "script/reaction_table_loader.h"
#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"
#include "sim/multires_state.h"
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

    // 試験のパッケージ + init.luau が modEntry を返す Mod を読んで表の定義にする
    std::expected<ReactionTableDefinition, std::string> DefinitionWithMod(const std::string& modEntry) {
        auto sandbox = script::LuauSandbox::Create(script::SandboxLimits{});
        if (!sandbox)
            return std::unexpected(std::string("殻を作れない"));

        (*sandbox)->Seal();
        const script::PackageSource mod{.name = "layout_mod",
                                        .files = {{"package.luau", "return { format = 1 }"}, {"init.luau", modEntry}}};
        std::vector<script::PackageSource>
            packages = ReadCombustionTestPackages();  // 試験のパッケージ + 依存の標準(T-0498)
        packages.push_back(mod);
        const auto loaded = script::LoadPackages(**sandbox, packages, {.basePackage = "combustion_test"});
        if (!loaded)
            return std::unexpected(loaded.error());

        if (!loaded->rejected.empty())
            return std::unexpected(loaded->rejected.front().reason);

        return script::ReadReactionTableDefinition(*loaded);
    }

    // 同じ Mod を読んでベイクする
    std::expected<BakedReactionTable, std::string> BakeWithMod(const std::string& modEntry) {
        const auto definition = DefinitionWithMod(modEntry);
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

    // 前の分類 name の注意(D-510)の数
    size_t RetiredNotes(const BakedReactionTable& table, std::string_view name) {
        const std::string text = std::format("{} は読まない", name);

        return (size_t)rng::count_if(
            table.warnings, [&](const std::string& warning) { return warning.find(text) != std::string::npos; });
    }

    // 流れる所(D-510・T-0724): 区画の箱はやめた。試験のパッケージは地球の環境(基準の大気・重力)に依るので、
    // 何も書かなくても気体も液体も世界いっぱいで流れる(仮の世界では世界いっぱいの区画 1 つ・端は閉じた壁)
    void TestFlowAreas() {
        const std::array<uint32_t, 3> whole = {PROBE_GRID_SIZE, PROBE_GRID_SIZE, PROBE_GRID_SIZE};
        const auto plain = BakeWithMod("return {}");
        EXPECT(plain.has_value());
        if (plain) {
            ProbeSimOptions options;
            SetLayoutRegions(plain->worldLayout, options);
            EXPECT(options.gas.Enabled() && options.liquid.Enabled());
            EXPECT((options.gas.minCell == std::array<uint32_t, 3>{0, 0, 0}) && options.gas.sizeCells == whole);
            EXPECT(options.gas.boundary[0] == GasBoundary::Wall && options.gas.boundary[1] == GasBoundary::Wall &&
                   options.gas.boundary[2] == GasBoundary::Wall);
            EXPECT((options.liquid.minCell == std::array<uint32_t, 3>{0, 0, 0}) && options.liquid.sizeCells == whole);
            EXPECT(options.liquid.advection);
            EXPECT(RetiredNotes(*plain, "gas_regions") == 0 && RetiredNotes(*plain, "liquid_regions") == 0);
        }

        // 前の分類を書いても読まない(形の誤りも見ない)。注意が 1 つずつ出て、流れる所は同じ
        const auto retired = BakeWithMod(R"(return {
            gas_regions = { smoke = { min_cell = { 24, 26, 24 }, size_cells = { 16, 14, 16 }, colour = 1 } },
            liquid_regions = { pond = { min_cell = { 60, 0, 44 }, size_cells = { 12, 8, 3 } } },
        })");
        EXPECT(retired.has_value());
        if (retired) {
            EXPECT(RetiredNotes(*retired, "gas_regions") == 1 && RetiredNotes(*retired, "liquid_regions") == 1);
            EXPECT(retired->worldLayout.gasRegion && retired->worldLayout.gasRegion->sizeCells == whole);
            EXPECT(retired->worldLayout.liquidRegion && retired->worldLayout.liquidRegion->sizeCells == whole);
        }

        // 基準の大気・重力の無い世界(ブランクの表)では流れない
        const auto blank = script::LoadBlankReactionTable();
        EXPECT(blank.has_value());
        if (!blank)
            return;

        const auto empty = BakeWorldLayout(WorldLayoutDefinition{}, blank->table);
        EXPECT(empty && !empty->gasRegion && !empty->liquidRegion);
        if (empty) {
            ProbeSimOptions options;
            options.gas.sizeCells = {1, 1, 1};
            SetLayoutRegions(*empty, options);
            EXPECT(!options.gas.Enabled() && !options.liquid.Enabled());
        }
    }

    // セルの器(T-0558・ADR-0260): 凝縮相は合計 2^36 µmol まで書ける(0.5 m 角の水 6917 mol も)。合計と気体のぶんを超えたら誤り
    void TestCellAmountLimits() {
        constexpr uint64_t LIMIT = fx::CELL_AMOUNT_LIMIT_MICROMOLES;
        constexpr uint64_t GAS_LIMIT = fx::CELL_GAS_AMOUNT_LIMIT_MICROMOLES;
        const auto region = [](const std::string& amounts) {
            return "return { world_cells = { dense = { layer = 2, min_cell = { 0, 0, 0 }, size_cells = { 1, 1, 1 }, "
                   "amounts_umol = { " +
                   amounts + " }, temperature_mk = 300000 } } }";
        };

        // --- 凝縮相(セルロース)は 2147 mol を超えて上限ちょうどまで ---
        const std::string full = std::format("cellulose = {}, oxygen = 1", LIMIT - 1);
        const auto baked = BakeWithMod(region(full));
        if (!baked)
            std::printf("読めない: %s\n", baked.error().c_str());

        EXPECT(baked.has_value());
        if (baked) {
            const std::vector<reaction::RxCell> world = MakeProbeInitialWorld(*baked);
            const uint32_t slot = reaction::RxFindSlot(world[0], baked->SpeciesId("cellulose"));
            EXPECT(slot != reaction::RX_NO_SLOT && world[0].amounts[slot] == LIMIT - 1);
        }

        // --- 合計の上限・1 物質の上限・気体のぶんの上限 ---
        EXPECT(FailsWith(region(std::format("cellulose = {}, oxygen = 2", LIMIT - 1)), "合計"));
        EXPECT(FailsWith(region(std::format("cellulose = {}", LIMIT + 1)), "amounts_umol.cellulose"));
        EXPECT(FailsWith(region(std::format("oxygen = {}", GAS_LIMIT)), "気体のぶん"));
        EXPECT(
            FailsWith(region(std::format("oxygen = {}, nitrogen = {}", GAS_LIMIT / 2, GAS_LIMIT / 2)), "気体のぶん"));
        EXPECT(BakeWithMod(region(std::format("oxygen = {}", GAS_LIMIT - 1))).has_value());

        // --- 材料のひな形も同じ器 ---
        const auto material = [](uint64_t amount) {
            return std::format(
                "return {{ material_templates = {{ dense = {{ label = \"x\", contents = {{ {{ species = "
                "\"cellulose\", micromoles = {} }} }} }} }} }}",
                amount);
        };
        EXPECT(BakeWithMod(material(LIMIT)).has_value());
        EXPECT(FailsWith(material(LIMIT + 1), "合計"));
    }

    // --- 層(T-1236・ADR-0489): settled_umol・書かなかった粒の読み方・層つきで根を置く ---

    // 粒の物質(ばらける固体)test_sand と、砂の床(層つき)・砂の山(層を書かない)を足す Mod。floorSettled が空なら床も層を書かない
    std::string SandMod(const std::string& floorSettled) {
        const std::string settled = floorSettled.empty() ? "" : "settled_umol = { " + floorSettled + " }, ";
        return R"(return {
            species = { test_sand = { composition = { C = 1 }, formation_enthalpy_j_per_mol = -100000,
                                      heat_capacity_mj_per_mol_k = 40000, thermal_conductivity_mw_per_m_k = 300,
                                      phase = "solid", density_kg_per_m3 = 1600, solid_motion = "loose",
                                      repose_angle_deg = 34 } },
            world_cells = {
                sand_floor = { layer = 2, min_cell = { 0, 0, 0 }, size_cells = { 16, 2, 16 },
                               amounts_umol = { test_sand = 20000000, nitrogen = 4000 }, )" +
               settled + R"(temperature_mk = 300000 },
                sand_heap = { layer = 3, min_cell = { 0, 2, 0 }, size_cells = { 1, 1, 1 },
                              amounts_umol = { test_sand = 1000000 }, temperature_mk = 300000 },
            },
        })";
    }

    // 組み立ての領域 index の物質 name の層
    uint64_t RegionLayer(const BakedReactionTable& table, const WorldLayout& layout, size_t index,
                         std::string_view name) {
        const WorldCellRegion& region = layout.cellRegions[index];
        const uint32_t slot = reaction::RxFindSlot(region.cell, table.SpeciesId(name));
        return slot == reaction::RX_NO_SLOT ? 0 : region.layers[slot];
    }

    size_t NotesNaming(const WorldLayout& layout, std::string_view text) {
        return (size_t)rng::count_if(layout.unsettledGrainNotes,
                                     [&](const std::string& note) { return note.find(text) != std::string::npos; });
    }

    void TestSettledLayers() {
        const auto definition = DefinitionWithMod(SandMod("test_sand = 15000000"));
        if (!definition)
            std::printf("読めない: %s\n", definition.error().c_str());

        EXPECT(definition.has_value());
        if (!definition)
            return;

        const auto baked = BakeReactionTable(*definition);
        EXPECT(baked.has_value());
        if (!baked)
            return;

        // --- (a) 舞う粒(今の読み方): 書いた層だけ。書かなかった粒は層 0 で、領域ごとに知らせる ---
        const WorldLayout& floating = baked->worldLayout;
        const size_t floor = floating.cellRegions.size() - 2;
        const size_t heap = floating.cellRegions.size() - 1;
        EXPECT(RegionLayer(*baked, floating, floor, "test_sand") == 15000000);
        EXPECT(RegionLayer(*baked, floating, floor, "nitrogen") == 0);
        EXPECT(RegionLayer(*baked, floating, heap, "test_sand") == 0);
        EXPECT(floating.unsettledGrainNotes.size() == 1 && NotesNaming(floating, "sand_heap") == 1 &&
               NotesNaming(floating, "test_sand") == 1);

        // --- (b) 全部が層: 書かなかった粒は層 = 量・知らせなし。書いた層はそのまま ---
        const auto settled = BakeWorldLayout(definition->worldLayout, *baked, UnwrittenGrainLayers::Settled);
        EXPECT(settled.has_value());
        if (settled) {
            EXPECT(RegionLayer(*baked, *settled, heap, "test_sand") == 1000000);
            EXPECT(RegionLayer(*baked, *settled, floor, "test_sand") == 15000000);
            EXPECT(settled->unsettledGrainNotes.empty());
        }

        // --- 0 と書けば舞わせると書いたことになる(知らせない)---
        const auto zero = BakeWithMod(SandMod("test_sand = 0"));
        EXPECT(zero && RegionLayer(*zero, zero->worldLayout, floor, "test_sand") == 0 &&
               NotesNaming(zero->worldLayout, "sand_floor") == 0 && NotesNaming(zero->worldLayout, "sand_heap") == 1);

        // --- 書き間違い: 層 > 量・中身に無い物質・知らない物質・負の数・同じ物質が 2 回 ---
        EXPECT(FailsWith(SandMod("test_sand = 20000001"), "超える"));
        EXPECT(FailsWith(SandMod("oxygen = 1"), "settled_umol.oxygen: 中身(amounts_umol)に無い"));
        EXPECT(FailsWith(SandMod("unobtainium = 1"), "中身(amounts_umol)に無い"));
        EXPECT(FailsWith(SandMod("test_sand = -1"), "settled_umol.test_sand"));

        // --- 層つきで根を置く(多重解像度の世界。根のセル 1 つ = 格子のセル 1 つ)---
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(*baked, 4, 0, 4));
        const uint32_t root = PlaceLayoutRootBlock(nest, floating, 0, 0, 0);
        const uint32_t sand = baked->SpeciesId("test_sand");
        bool placed = true;
        for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
            const uint32_t y = multires::MrCellY(index);
            const reaction::RxCell cell = LoadNestCell(nest, root, index);
            const MrCellLayers layers = LoadNestCellLayers(nest, root, index);
            const uint32_t slot = reaction::RxFindSlot(cell, sand);
            const bool heapCell = y == 2 && multires::MrCellX(index) == 0 && multires::MrCellZ(index) == 0;
            const uint64_t want = y < 2 ? 15000000 : 0;
            const bool hasSand = y < 2 || heapCell;
            placed = placed && (slot != reaction::RX_NO_SLOT) == hasSand &&
                     (slot == reaction::RX_NO_SLOT || layers.amounts[slot] == want);
        }

        EXPECT(placed);
        EXPECT(CountLayerMisfits(nest) == 0);

        // 層を書かない同じ組み立てとは要約が違い、層の無い置き方と同じ値のセル
        const auto plain = BakeWithMod(SandMod(""));
        EXPECT(plain.has_value());
        if (!plain)
            return;

        MultiresNest plainNest = MakeMultiresNest(test::MakeMultiresCapacity(*plain, 4, 0, 4));
        const uint32_t plainRoot = PlaceLayoutRootBlock(plainNest, plain->worldLayout, 0, 0, 0);
        EXPECT(HashWholeNest(plainNest) != HashWholeNest(nest));
        EXPECT(!nest_detail::BlockKeepsLayers(plainNest, plainRoot));
        const reaction::RxCell plainCell = LoadNestCell(plainNest, plainRoot, 0);
        const reaction::RxCell layeredCell = LoadNestCell(nest, root, 0);
        EXPECT(std::memcmp(&plainCell, &layeredCell, sizeof(plainCell)) == 0);
    }

}  // namespace

int main() {
    TestSampleMatchesOld();
    TestRuntimeAndReplay();
    TestBlank();
    TestModAddsAndErrors();
    TestCellAmountLimits();
    TestFlowAreas();
    TestSettledLayers();

    std::printf("world_layout_test: %s\n", failureCount == 0 ? "OK" : "FAILED");

    return failureCount == 0 ? 0 : 1;
}
