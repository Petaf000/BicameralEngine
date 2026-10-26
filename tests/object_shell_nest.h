// object_shell_nest.h — 物の箱の層の流れ(T-1143)を、気体の流れる多重解像度の世界の中に置く場面(T-1144。T-1143 の次の一手 1)。
// 世界: 根のレベル level(0 = 0.5 m・1 = 0.25 m)の空気だけの 4 m 角の箱。下は壁・上と横は開いた端(外は基準の大気)。
//   気体は sim/multires_gas(根の 1 段の旧気体・陽解法の小刻み)で流れ、熱と反応は StepNest(伝導つき)。
// 1 刻み = 世界の刻み(StepNest → 気体)→ 囲むセルを読む(object_heat_nest::LoadCells)→ 物の熱 → 層の流れ → 層を作る・返す
//   → 変わったセルを世界へ書いて帳簿の理由「物との受け渡し」に載せる(object_heat_nest::StoreCells)。
// 閉じた 1 セル(object_shell_test の既定の場面)と違い、炎の上の熱い気体は世界の流れで上へ去り、横と下から新しい空気が入る。
// 使うのは tests/object_shell_test.cpp(--case の nest)。
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "object_heat_nest_scene.h"
#include "sim/multires_gas.h"

namespace bicameral::test::object_shell_nest {

    namespace rx = bicameral::reaction;
    namespace mr = bicameral::multires;

    constexpr uint32_t WORLD_METRES_QUARTERS = 16;  // 4 m = 0.25 m × 16
    // 物を置く 0.25 m の座標(横は真ん中・上は地面から 1 m)
    constexpr std::array<uint32_t, 3> OBJECT_QUARTER = {8, 4, 8};

    struct ShellNest {
        sim::MultiresNest nest;
        sim::MultiresGas gas;
        std::vector<object_heat_nest::NestCellRef> refs;
        std::vector<rx::RxCell> before;  // 物の熱の前の囲むセル(StoreCells が差を書く)
        sim::ConservedTotals start;
        int32_t level = 0;
        bool stored = true;              // 世界へ書けた
        bool balanced = true;            // 世界の保存(Σ − 帳簿)が一定
        uint64_t brokenTick = 0;         // 世界の保存が初めて崩れた刻み(崩れた欄を printf する。T-1185)
        int32_t hottestAboveKelvin = 0;  // 物の囲むセルの 1 つ上のセルの一番熱い温度(熱い気体が上へ去るか)
    };

    inline object_heat_nest::NestCellRef CellRefAt(const sim::MultiresNest& nest, int32_t level,
                                                   std::array<uint32_t, 3> place) {
        const uint32_t slot = sim::LookupBlock(nest, level, place[0] & ~7u, place[1] & ~7u, place[2] & ~7u);

        return {.slot = slot, .index = mr::MrCellIndex(place[0] & 7u, place[1] & 7u, place[2] & 7u)};
    }

    inline std::array<uint32_t, 3> ObjectPlace(int32_t level) {
        const auto shift = static_cast<uint32_t>(1 - level);

        return {OBJECT_QUARTER[0] >> shift, OBJECT_QUARTER[1] >> shift, OBJECT_QUARTER[2] >> shift};
    }

    inline std::unique_ptr<ShellNest> MakeShellNest(const sim::BakedReactionTable& table, int32_t level,
                                                    uint64_t seed) {
        const uint32_t cellsPerAxis = WORLD_METRES_QUARTERS >> static_cast<uint32_t>(1 - level);
        const uint32_t blocksPerAxis = cellsPerAxis / 8;
        const uint32_t count = blocksPerAxis * blocksPerAxis * blocksPerAxis;
        uint32_t indexEntries = 1;
        while (indexEntries < 2 * count)
            indexEntries *= 2;

        auto world = std::make_unique<ShellNest>();
        world->level = level;
        world->nest = sim::MakeMultiresNest({.worldBlocks = count,
                                             .observerBlocks = 0,
                                             .fractions = 1,
                                             .pages = count,
                                             .indexEntries = indexEntries,
                                             .ledgerColumns = 1 + static_cast<uint32_t>(table.species.size()),
                                             .rootLevel = level});
        world->nest.edges.world = {.rootLevel = level,
                                   .lowX = 0,
                                   .lowY = 0,
                                   .lowZ = 0,
                                   .highX = cellsPerAxis,
                                   .highY = cellsPerAxis,
                                   .highZ = cellsPerAxis,
                                   .top = mr::WE_OPEN,
                                   .bottom = mr::WE_WALL,
                                   .sides = mr::WE_OPEN};
        auto gas = sim::MultiresGas::Create(world->nest, table, {.randomSeed = seed, .floorHeightMillimeters = 0});
        if (!gas) {
            std::printf("世界の気体を作れない: %s\n", gas.error().c_str());
            std::exit(1);
        }

        world->gas = *gas;
        std::vector<rx::RxCell> layers;
        for (uint32_t layer = 0; layer < cellsPerAxis; ++layer)
            layers.push_back(world->gas.ReferenceCell(world->nest, table, layer));

        for (uint32_t bz = 0; bz < cellsPerAxis; bz += 8) {
            for (uint32_t by = 0; by < cellsPerAxis; by += 8) {
                for (uint32_t bx = 0; bx < cellsPerAxis; bx += 8) {
                    std::vector<rx::RxCell> cells(mr::MR_BLOCK_CELLS);
                    for (uint32_t index = 0; index < cells.size(); ++index) {
                        const std::array<uint32_t, 3> place = {bx + (index % 8), by + ((index / 8) % 8),
                                                               bz + (index / 64)};
                        cells[index] = layers[place[mr::WE_UP_AXIS]];
                    }

                    sim::PlaceRootBlock(world->nest, bx, by, bz, cells);
                }
            }
        }

        const std::array<uint32_t, 3> place = ObjectPlace(level);
        world->refs = {CellRefAt(world->nest, level, place)};
        world->start = sim::ComputeConservedTotals(world->nest, table, level);

        return world;
    }

    // 囲むセルを世界の値にする(物を作った直後に 1 回。物の熱の前の値)
    inline void AttachCells(ShellNest& world, OhWorld& objects) {
        objects.cells.clear();
        for (const object_heat_nest::NestCellRef& ref : world.refs)
            objects.cells.push_back(sim::LoadNestCell(world.nest, ref.slot, ref.index));
    }

    // 世界の刻み(反応と伝導 → 気体)の後に囲むセルを読む。物の熱の前に呼ぶ
    inline void BeforeObject(const sim::BakedReactionTable& table, ShellNest& world, OhWorld& objects, uint64_t seed,
                             uint64_t tick) {
        sim::StepNest(world.nest, table, seed, tick, {.conduction = true});
        std::vector<uint32_t> changedSlots;
        (void)world.gas.Step(world.nest, table, tick, changedSlots);
        object_heat_nest::LoadCells(world.nest, world.refs, objects);
        world.before = objects.cells;
    }

    // 物の熱・層の流れ・層を作る / 返すで変わった囲むセルを世界へ書く。刻みの終わりに呼ぶ
    // 世界の保存が崩れた欄を書く(エネルギー・元素・登録した保存量の欄。T-1185)
    inline void PrintBrokenColumns(const sim::ConservedTotals& now, const sim::ConservedTotals& start, uint64_t tick) {
        std::printf("  世界の Σ − 帳簿 が崩れた刻み %llu: エネルギー %s", static_cast<unsigned long long>(tick),
                    now.energy == start.energy ? "同じ" : "違う");
        for (size_t e = 0; e < now.elements.size(); ++e) {
            if (!(now.elements[e] == start.elements[e]))
                std::printf("・元素 %zu 違う", e);
        }

        for (size_t f = 0; f < now.fields.size(); ++f) {
            if (!(now.fields[f] == start.fields[f]))
                std::printf("・欄 %zu 違う", f);
        }

        std::printf("\n");
    }

    // world.before は書いた後の値に揃える(同じ刻みにもう一度呼んでも、前の差を帳簿に二度載せない。T-1185:
    // 終わりに層を返す時の 2 回目の呼び出しが最後の刻みの差を二度載せ、世界の Σ − 帳簿 が崩れて見えた)
    inline void AfterObject(const sim::BakedReactionTable& table, const OhCpuTable& shared, ShellNest& world,
                            const OhWorld& objects, bool checkBalance, uint64_t tick) {
        const auto speciesCount = static_cast<uint32_t>(table.species.size());
        world.stored = object_heat_nest::StoreCells(world.nest, world.refs, objects, world.before, world.level,
                                                    speciesCount) &&
                       world.stored;
        world.before = objects.cells;
        if (checkBalance && world.balanced) {
            const sim::ConservedTotals now = sim::ComputeConservedTotals(world.nest, table, world.level);
            if (!(now == world.start)) {
                world.balanced = false;
                world.brokenTick = tick;
                PrintBrokenColumns(now, world.start, tick);
            }
        }

        std::array<uint32_t, 3> above = ObjectPlace(world.level);
        ++above[mr::WE_UP_AXIS];
        const object_heat_nest::NestCellRef ref = CellRefAt(world.nest, world.level, above);
        const rx::RxCell cell = sim::LoadNestCell(world.nest, ref.slot, ref.index);
        world.hottestAboveKelvin = std::max(world.hottestAboveKelvin,
                                            rx::RxComputeThermal(shared, cell).temperature / 1000);
    }

    // 物の保存の和(ボクセル + 対の端数 + 帳簿の物との受け渡しの入 − 出 − 入った光)。[0] = エネルギー、[1..] = 元素(ボクセルの単位)
    inline std::vector<int64_t> ObjectTotals(const sim::BakedReactionTable& table, const OhTables& tables,
                                             const OhWorld& objects, const ShellNest& world) {
        const auto speciesCount = static_cast<uint32_t>(table.species.size());
        const auto shift = static_cast<uint32_t>(3 * (objects.objects[0].bodyLevel - world.level));
        std::vector<int64_t> columns(1 + speciesCount, 0);
        for (const rx::RxCell& voxel : objects.voxels) {
            columns[0] += voxel.energy;
            for (uint32_t i = 0; i < voxel.speciesCount; ++i)
                columns[1 + voxel.species[i]] += static_cast<int64_t>(voxel.amounts[i]);
        }

        for (const rx::OhFlow& carry : objects.carries) {
            columns[0] += carry.energy;
            for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
                if (tables.gases.species[g] != 0)
                    columns[1 + tables.gases.species[g]] += carry.amounts[g];
            }
        }

        columns[0] += sim::WorldLedgerNetUnits(world.nest, mr::WL_REASON_OBJECT_EXCHANGE, mr::WL_COLUMN_ENERGY,
                                               world.level)
                      << shift;
        columns[0] -= objects.sunIn;
        for (uint32_t species = 1; species < speciesCount; ++species) {
            const int64_t net = sim::WorldLedgerNetUnits(world.nest, mr::WL_REASON_OBJECT_EXCHANGE,
                                                         mr::WL_COLUMN_SPECIES_BASE + species, world.level);
            columns[1 + species] += net << shift;
        }

        const size_t elementCount = table.elementNames.size();
        std::vector<int64_t> totals(1 + elementCount, 0);
        totals[0] = columns[0];
        for (uint32_t species = 1; species < speciesCount; ++species) {
            for (size_t e = 0; e < elementCount; ++e)
                totals[1 + e] += columns[1 + species] * table.speciesElements[(species * elementCount) + e];
        }

        return totals;
    }

}  // namespace bicameral::test::object_shell_nest
