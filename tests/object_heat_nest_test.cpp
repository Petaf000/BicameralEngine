// object_heat_nest_test.cpp — 物の熱(tests/object_heat_cpu.h)を多重解像度の世界(MultiresNest)のセルにつなぐ最初の形の CPU の試験(T-1077・ADR-0451・ADR-0459)。
// 1 刻み = 世界の刻み(StepNest)→ 囲むセルを世界から読む(LoadNestCell。変わったセルは物のブロックを起こす)→ 物の熱の刻み(StepObjectHeat)
//   → 変わったセルの一様のブロックを枠の順に頁に広げ(ExpandChangedBlocks)、書く(StoreNestCell・PokeNestBlock。液体の 1 段〔T-0717〕と同じ
//   「刻みの段の外から書く」口)。
// 場面: 4 m 角の箱(下 1 m が石、上は空気)の真ん中の空気のセルに燃えている小枝 3 本と紙 1 枚、+x の隣のセルに冷えた薪 1 本。
//   一様の空気の場面: 8 m 角の箱の上の根(石に触れない一様の空気。頁なし)に同じ物を置き、物の熱が初めて書く時に頁に広げる(T-1077 ①)。
// 確かめる:
//   1. 保存(帳簿の上で): 物との受け渡しを世界の帳簿の理由「物との受け渡し」(WL_REASON_OBJECT_EXCHANGE)に載せ、
//      世界だけ(ComputeConservedTotals = セル − 帳簿)と、物(ボクセル + 対の端数)+ 帳簿のその理由の入 − 出 が、エネルギーと元素で毎 60 刻み一定
//   2. 決定的: 2 回で同じ
//   3. 世界のセルが物の熱を受け取る(燃えている所の空気が温まる)・冷えた薪のブロックは眠る
//   4. 0.5 m と 0.25 m の世界で、燃える固体の残りを並べる(記録だけ)
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "object_heat_nest_scene.h"

namespace {

    using namespace bicameral;
    namespace rx = bicameral::reaction;
    namespace mr = bicameral::multires;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    using namespace test::object_heat_nest;  // 場面と CPU の口(object_heat_nest_scene.h)

    constexpr uint32_t TICKS = 600;

    // --- 場面 ---

    struct Run {
        uint64_t digest = 0;
        bool ledgerWorld = true;    // 世界の保存の検査(帳簿込み)が一定
        bool ledgerObjects = true;  // 物 + 帳簿の物との受け渡しが一定
        bool stored = true;
        int32_t fireAirStartMilliKelvin = 0;
        int32_t fireAirEndMilliKelvin = 0;
        int32_t logAirEndMilliKelvin = 0;
        uint64_t fuelStart = 0;
        uint64_t fuelEnd = 0;
        uint64_t logAwakeBlocks = 0;  // 後半 300 刻みで冷えた薪の回したブロック(段の和)
        size_t logBlocks = 0;
        uint32_t expanded = 0;  // 頁に広げたブロック(世界の数え)
    };

    uint64_t FuelCarbon(const sim::BakedReactionTable& table, const test::OhWorld& world, uint32_t objects) {
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t carbon = table.SpeciesId("carbon");
        uint64_t total = 0;
        for (uint32_t o = 0; o < objects; ++o) {
            const rx::OhObject object = world.objects[o];
            for (uint32_t v = 0; v < object.voxelCount; ++v) {
                const rx::RxCell& cell = world.voxels[object.voxelBegin + v];
                total += (6 * rx::OhAmountOf(cell, cellulose)) + rx::OhAmountOf(cell, carbon);
            }
        }

        return total;
    }

    // 物(ボクセル + 対の端数。ボクセルの単位)+ 帳簿の物との受け渡しの入 − 出(セルの単位 × 8^(L − Lc))を、エネルギーと元素ごとに
    std::vector<int64_t> ObjectColumns(const sim::BakedReactionTable& table, const test::OhTables& tables,
                                       const test::OhWorld& world, const sim::MultiresNest& nest, int32_t level) {
        const auto speciesCount = static_cast<uint32_t>(table.species.size());
        const auto shift = static_cast<uint32_t>(3 * (BODY_LEVEL - level));
        std::vector<int64_t> columns(1 + speciesCount, 0);
        for (const rx::RxCell& voxel : world.voxels) {
            columns[0] += voxel.energy;
            for (uint32_t i = 0; i < voxel.speciesCount; ++i)
                columns[1 + voxel.species[i]] += static_cast<int64_t>(voxel.amounts[i]);
        }

        for (const rx::OhFlow& carry : world.carries) {
            columns[0] += carry.energy;
            for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
                if (tables.gases.species[g] != 0)
                    columns[1 + tables.gases.species[g]] += carry.amounts[g];
            }
        }

        columns[0] += sim::WorldLedgerNetUnits(nest, mr::WL_REASON_OBJECT_EXCHANGE, mr::WL_COLUMN_ENERGY, level)
                      << shift;
        for (uint32_t species = 1; species < speciesCount; ++species) {
            const int64_t net = sim::WorldLedgerNetUnits(nest, mr::WL_REASON_OBJECT_EXCHANGE,
                                                         mr::WL_COLUMN_SPECIES_BASE + species, level);
            columns[1 + species] += net << shift;
        }

        // 物の中で反応が物質を変えるので、物質の列を元素に直して比べる(エネルギーはそのまま)
        const size_t elementCount = table.elementNames.size();
        std::vector<int64_t> totals(1 + elementCount, 0);
        totals[0] = columns[0];
        for (uint32_t species = 1; species < speciesCount; ++species) {
            for (size_t e = 0; e < elementCount; ++e)
                totals[1 + e] += columns[1 + species] * table.speciesElements[(species * elementCount) + e];
        }

        return totals;
    }

    Run RunScene(const sim::BakedReactionTable& table, const test::OhTables& tables, int32_t level,
                 bool uniformAir = false) {
        NestScene scene = MakeNestScene(table, level, uniformAir);
        sim::MultiresNest& nest = scene.nest;
        test::OhWorld& world = scene.world;

        Run run;
        const sim::ReactionTableView view = table.View();
        run.fireAirStartMilliKelvin = rx::RxComputeThermal(view, world.cells[0]).temperature;
        run.fuelStart = FuelCarbon(table, world, 4);
        const sim::ConservedTotals worldInitial = sim::ComputeConservedTotals(nest, table, BODY_LEVEL);
        const std::vector<int64_t> objectInitial = ObjectColumns(table, tables, world, nest, level);
        const uint32_t expandedBefore = nest.counters[mr::MR_COUNTER_EXPANDED];
        uint64_t logAwakeAtHalf = 0;
        for (uint32_t tick = 1; tick <= TICKS; ++tick) {
            run.stored = run.stored && StepNestScene(table, tables, scene, tick);
            if (tick == TICKS / 2)
                logAwakeAtHalf = world.awakeExchangeBlocks;

            if (tick % 60 != 0)
                continue;

            // 帳簿の上で: 世界だけ(セル − 帳簿)が一定・物 + 帳簿の「物との受け渡し」の入 − 出 が一定(T-1077)。旧の検査(世界 + 物)も
            run.ledgerWorld = run.ledgerWorld && sim::ComputeConservedTotals(nest, table, BODY_LEVEL) == worldInitial;
            const std::vector<int64_t> objectNow = ObjectColumns(table, tables, world, nest, level);
            for (size_t column = 0; column < objectNow.size() && run.ledgerObjects; ++column) {
                if (objectNow[column] != objectInitial[column])
                    std::printf("物 + 帳簿が崩れた: 刻み %u 列(0 = エネルギー・1 + 元素)%zu 差 %lld\n", tick, column,
                                static_cast<long long>(objectNow[column] - objectInitial[column]));
            }

            run.ledgerObjects = run.ledgerObjects && objectNow == objectInitial;
        }

        run.fireAirEndMilliKelvin = rx::RxComputeThermal(view, world.cells[0]).temperature;
        run.logAirEndMilliKelvin = rx::RxComputeThermal(view, world.cells[1]).temperature;
        run.fuelEnd = FuelCarbon(table, world, 4);
        run.logBlocks = world.objects[4].blocksX * world.objects[4].blocksY * world.objects[4].blocksZ;
        run.logAwakeBlocks = world.awakeExchangeBlocks - logAwakeAtHalf;
        run.expanded = nest.counters[mr::MR_COUNTER_EXPANDED] - expandedBefore;
        for (const rx::RxCell& voxel : world.voxels)
            run.digest = fx::FxHashCombine(run.digest, sim::HashReactionCell(voxel));

        for (const rx::RxCell& cell : sim::DenseCellImage(nest))
            run.digest = fx::FxHashCombine(run.digest, sim::HashReactionCell(cell));

        return run;
    }

    void Print(const char* label, const Run& run) {
        std::printf(
            "%s: 燃える物の囲むセル %.3f K → %.3f K・冷えた薪の囲むセル %.3f K・燃える固体の残り %.1f %%・"
            "後半 %u 刻みで回したブロック(全部の物)%llu・頁に広げた %u・保存(帳簿の上で)世界 %s・物 + 帳簿 %s・"
            "書き込み %s・要約 %016llx\n",
            label, run.fireAirStartMilliKelvin / 1000.0, run.fireAirEndMilliKelvin / 1000.0,
            run.logAirEndMilliKelvin / 1000.0,
            100.0 * static_cast<double>(run.fuelEnd) / static_cast<double>(std::max<uint64_t>(run.fuelStart, 1)),
            TICKS / 2, static_cast<unsigned long long>(run.logAwakeBlocks), run.expanded,
            run.ledgerWorld ? "0" : "崩れた", run.ledgerObjects ? "0" : "崩れた", run.stored ? "OK" : "失敗",
            static_cast<unsigned long long>(run.digest));
    }

}  // namespace

int main() {
    const sim::BakedReactionTable table = MakeTable();
    const auto tables = test::MakeObjectHeatTables(table, DEFAULT_DIFFUSIVITY_MM2_PER_S);
    EXPECT(tables.has_value());
    if (!tables)
        return 1;

    const Run coarse = RunScene(table, *tables, 0);
    const Run again = RunScene(table, *tables, 0);
    const Run fine = RunScene(table, *tables, 1);
    Print("0.5 m の世界", coarse);
    Print("0.25 m の世界", fine);
    EXPECT(coarse.ledgerWorld && fine.ledgerWorld && coarse.ledgerObjects && fine.ledgerObjects);
    EXPECT(coarse.stored && fine.stored);

    // --- 一様の空気の根(頁に広げてから書く。T-1077 ①)---
    for (const int32_t level : {0, 1}) {
        const Run uniform = RunScene(table, *tables, level, true);
        Print(level == 0 ? "0.5 m の世界(一様の空気の根)" : "0.25 m の世界(一様の空気の根)", uniform);
        EXPECT(uniform.ledgerWorld && uniform.ledgerObjects && uniform.stored);
        EXPECT(uniform.expanded >= 1);
        EXPECT(uniform.fireAirEndMilliKelvin > uniform.fireAirStartMilliKelvin);
    }
    EXPECT(coarse.digest == again.digest);
    EXPECT(coarse.fireAirEndMilliKelvin > coarse.fireAirStartMilliKelvin);
    EXPECT(coarse.fuelEnd < coarse.fuelStart);
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("object_heat_nest: 全部通った\n");

    return 0;
}
