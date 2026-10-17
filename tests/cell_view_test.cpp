// cell_view_test.cpp — セルの見立て(shaders/common/cell_view.hlsli の MakeCellView。T-0713・ADR-0333)の試験。
//
// 確かめること(CPU。見立ては今の仕組みの値を置き換える前に、同じ値をビット単位で出すこと):
//   1. いろいろなセル(気体・液体・固体・密度の無い固体・溢れる固体と液体・空)× レベル -3・0・2・5 で、
//      熱(MrCellThermal・RxComputeThermal・MrFaceLimit。レベル 0 は熱のキャッシュ HcMakeCache)・
//      体積の分け合い(ShareCellVolume・LiquidOpenQ16・GasOpenQ16)・液体の質量と体積(ProbeLiquidField)・
//      液体の圧力の元(probe_liquid の ReadState の式)・気体の圧力(gas_reference の DeriveOpenCell の式)と同じ
//   2. 多重解像度の世界の CPU の木(鎖の場面を伝導で 20 刻み)の本物のブロックの全部のセルを、ブロックのレベルで同じく比べる
//   3. 成分の数に上限の無い形のセル(RxWideCell)でも同じ見立て(9 成分は熱を MrCellThermal と比べる)
//   4. 費用: 見立て 1 つを作る時間と、今の別々の数え方(MrCellThermal + ShareCellVolume + 空き + 液体の質量と体積)の時間(docs/perf.md)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "common/cell_view.hlsli"
#include "core/aliases.h"
#include "multires_conduction_scene.h"
#include "reaction_test_table.h"
#include "sim/cell_view.h"
#include "sim/cell_volume.h"
#include "sim/gas_internal.h"
#include "sim/multires_nest.h"
#include "sim/probe_liquid.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        if (failureCount < 20)
            std::printf("FAILED line %d: %s\n", line, text);

        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr int64_t REFERENCE_PRESSURE = 101325000000;  // µPa(1 気圧。差を確かめるだけ)
    constexpr int32_t LEVELS[] = {-3, 0, 2, 5};

    static_assert(PROBE_LIQUID_CELL_VOLUME_PICOLITERS == PROBE_CELL_VOLUME_PICOLITERS);
    static_assert(CV_PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 == gas_detail::PRESSURE_PER_AMOUNT_TEMPERATURE_Q32);
    static_assert(CvCellVolumePicoliters(0) == PROBE_CELL_VOLUME_PICOLITERS);
    static_assert(CvCellVolumePicoliters(1) == PROBE_CELL_VOLUME_PICOLITERS / 8);
    static_assert(CvCellVolumePicoliters(-1) == PROBE_CELL_VOLUME_PICOLITERS * 8);

    // 試験の表(燃焼の試験の表)+ 基準の大気 + 水(液体)・砂(密度あり)・密度を書かない砂・石(cell_volume_test と同じ)
    BakedReactionTable MakeTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.species.push_back({.name = "liquid_water",
                                      .composition = {{.element = "H", .count = 2}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -285830,
                                      .heatCapacity = 75300,
                                      .thermalConductivity = 600,
                                      .liquid = true,
                                      .density = 1000});
        const auto solid = [](const char* name, uint32_t density) {
            return SpeciesDefinition{.name = name,
                                     .composition = {{.element = "C", .count = 1}},
                                     .formationEnthalpy = 0,
                                     .heatCapacity = 8500,
                                     .thermalConductivity = 2000,
                                     .condensed = true,
                                     .density = density};
        };
        definition.species.push_back(solid("grain", 2650));
        definition.species.push_back(solid("loose_grain", 0));
        definition.species.push_back(solid("stone", 2000));

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    // 今の仕組みの気体の圧力(gas_reference.cpp の DeriveRaw → DeriveOpenCell の式。温度は渡した値)
    int64_t ReferenceGasPressure(uint64_t amount, uint32_t temperature, uint32_t open) {
        const fx::FxU128 amountTemperature = fx::FxMulU64Full(amount, temperature);
        const uint64_t full = fx::FxShiftRightU128(
            fx::FxMulU64Full(amountTemperature.lo, gas_detail::PRESSURE_PER_AMOUNT_TEMPERATURE_Q32), 32);
        if (open == CELL_OPEN_FULL)
            return (int64_t)full;

        const uint32_t volume = std::max(open, 256u);

        return (int64_t)fx::FxDivU128By64(fx::FxMulU64Full(full, CELL_OPEN_FULL), volume).quotient;
    }

    // 1 セル × 1 レベルの見立てを、今の仕組みの値と比べる
    void CompareView(const BakedReactionTable& table, const CellViewTable& viewTable, const reaction::RxCell& cell,
                     int32_t level) {
        const CvCellView view = MakeCellView(viewTable, cell, level, REFERENCE_PRESSURE);
        const ReactionTableView reactionView = table.View();

        // --- 熱と伝導の係数 ---
        const MrThermal thermal = MrCellThermal(reactionView, cell);
        const reaction::RxThermal rxThermal = reaction::RxComputeThermal(reactionView, cell);
        EXPECT(view.thermal.temperature == thermal.temperature);
        EXPECT(view.thermal.conductance == thermal.conductance);
        EXPECT(view.thermal.capacityLimit == thermal.capacityLimit);
        EXPECT(view.faceLimit == MrFaceLimit(thermal, level));
        EXPECT(view.heat == rxThermal.heat);
        EXPECT(view.heatCapacity == rxThermal.heatCapacity);
        if (level == 0) {
            const reaction::HcThermalCache cache = reaction::HcMakeCache(
                rxThermal, reaction::HcCellConductance(reactionView, cell));
            EXPECT(view.thermal.temperature == cache.temperature);
            EXPECT(view.thermal.conductance == cache.conductance);
            EXPECT(view.faceLimit == cache.faceLimit);
        }

        // --- 体積の分け合い(cell_volume)---
        const CellVolumeShare share = ShareCellVolume(table, cell);
        EXPECT(view.solid.volume == share.solid);
        EXPECT(view.liquid.volume == share.liquid);
        EXPECT(view.gas.volume == 0);
        EXPECT(view.liquidOpen == LiquidOpenQ16(share, PROBE_CELL_VOLUME_PICOLITERS));
        EXPECT(view.gasOpen == GasOpenQ16(share, PROBE_CELL_VOLUME_PICOLITERS));
        EXPECT(view.liquidRoom == PROBE_CELL_VOLUME_PICOLITERS - std::min(share.solid, PROBE_CELL_VOLUME_PICOLITERS));

        // --- 液体(probe_liquid の ReadState)---
        const uint64_t liquidMass = ProbeLiquidField::LiquidMass(table, cell);
        EXPECT(view.liquid.mass == liquidMass);
        if (share.liquid < CV_VOLUME_CAP)
            EXPECT(view.liquid.volume == ProbeLiquidField::LiquidVolume(table, cell));

        const uint64_t massMilligrams = liquidMass / 1000000;
        const uint32_t open = LiquidOpenQ16(share, PROBE_CELL_VOLUME_PICOLITERS);
        const uint64_t equivalent = open == CELL_OPEN_FULL || open == 0
                                        ? massMilligrams
                                        : fx::FxDivU128By64(fx::FxMulU64Full(massMilligrams, CELL_OPEN_FULL), open)
                                              .quotient;
        EXPECT(view.liquidMass == massMilligrams);
        EXPECT(view.liquidEquivalentMass == equivalent);
        const uint64_t filled = std::min(share.liquid, view.liquidRoom);
        EXPECT(view.liquidFill ==
               (view.liquidRoom == 0
                    ? 0u
                    : (uint32_t)fx::FxDivU128By64(fx::FxMulU64Full(filled, CELL_OPEN_FULL), view.liquidRoom).quotient));

        // --- 気体(相ごとの物質量の和はセルの和)---
        uint64_t gasAmount = 0;
        uint64_t total = 0;
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            total += cell.amounts[i];
            if (table.speciesCondensed[cell.species[i]] == 0)
                gasAmount += cell.amounts[i];
        }

        EXPECT(view.gas.amount == gasAmount);
        EXPECT(view.gas.amount + view.liquid.amount + view.solid.amount == total);
        EXPECT(view.gas.heatCapacity + view.liquid.heatCapacity + view.solid.heatCapacity == view.heatCapacity);
        const int64_t pressure = ReferenceGasPressure(gasAmount, thermal.temperature, view.gasOpen);
        EXPECT(view.gasPressure == pressure);
        EXPECT(view.gasPressureDeviation == pressure - REFERENCE_PRESSURE);
    }

    // --- いろいろなセル ---

    uint64_t NextRandom(uint64_t& state) {
        state = (state * 6364136223846793005ull) + 1442695040888963407ull;

        return state >> 17;
    }

    std::vector<reaction::RxCell> MakeVariedCells(const BakedReactionTable& table, uint32_t count) {
        const uint32_t water = table.SpeciesId("liquid_water");
        const uint32_t grain = table.SpeciesId("grain");
        const uint32_t looseGrain = table.SpeciesId("loose_grain");
        const uint32_t stone = table.SpeciesId("stone");
        const uint32_t nitrogen = table.SpeciesId("nitrogen");
        const uint32_t oxygen = table.SpeciesId("oxygen");

        // --- 決まったセル: 空・空気・満ちた水・溢れた水・満ちた砂・溢れた砂・密度の無い砂・砂と水の半分ずつ ---
        std::vector<reaction::RxCell> cells;
        cells.push_back(reaction::RxCell{});
        const auto add = [&](std::vector<SpeciesAmount> amounts, int32_t millikelvin) {
            rng::sort(amounts, {}, &SpeciesAmount::species);
            cells.push_back(MakeReactionCell(table, amounts, millikelvin));
        };
        add({{nitrogen, 4013000}, {oxygen, 1067000}}, 300000);
        add({{water, 6900000000}}, 290000);
        add({{water, 9000000000}, {nitrogen, 4000000}}, 350000);
        add({{grain, 26000000000}}, 300000);
        add({{grain, 40000000000}, {oxygen, 1000000}}, 900000);
        add({{looseGrain, 26000000000}, {nitrogen, 4000000}}, 300000);
        add({{grain, 13000000000}, {water, 3000000000}, {nitrogen, 2000000}}, 330000);
        add({{stone, 10000000000}, {water, 1000}, {oxygen, 1}}, 1200000);

        // --- 乱数のセル: 表の物質から 1〜8 個・物質量は相ごとの幅・温度 200〜2500 K ---
        uint64_t state = 20261017;
        const uint32_t speciesCount = (uint32_t)table.species.size();
        for (uint32_t c = 0; c < count; ++c) {
            std::vector<SpeciesAmount> amounts;
            const uint32_t want = 1 + (uint32_t)(NextRandom(state) % 8);
            for (uint32_t k = 0; k < want; ++k) {
                const uint32_t id = 1 + (uint32_t)(NextRandom(state) % (speciesCount - 1));
                if (rng::find(amounts, id, &SpeciesAmount::species) != amounts.end())
                    continue;

                const bool condensed = table.speciesCondensed[id] != 0;
                const uint64_t limit = condensed ? 8000000000ull : 3000000ull;
                amounts.push_back({id, 1 + (NextRandom(state) % limit)});
            }

            rng::sort(amounts, {}, &SpeciesAmount::species);
            const int32_t millikelvin = 200000 + (int32_t)(NextRandom(state) % 2300000);
            cells.push_back(MakeReactionCell(table, amounts, millikelvin));
        }

        return cells;
    }

    // --- 1. いろいろなセル × レベル ---
    void TestVariedCells(const BakedReactionTable& table, const CellViewTable& viewTable) {
        const std::vector<reaction::RxCell> cells = MakeVariedCells(table, 3000);
        for (const reaction::RxCell& cell : cells) {
            for (const int32_t level : LEVELS)
                CompareView(table, viewTable, cell, level);
        }

        std::printf("いろいろなセル: %zu セル × %zu レベル\n", cells.size(), std::size(LEVELS));
    }

    // --- 2. 多重解像度の世界の CPU の木 ---
    void TestMultiresNest(const BakedReactionTable& table, const CellViewTable& viewTable) {
        MultiresNest nest = test::MakeChainNest(table, 16);
        MultiresStepOptions options;
        options.conduction = true;
        for (uint64_t tick = 0; tick < 20; ++tick)
            StepActive(nest, table, test::CONDUCTION_SEED, tick, options);

        uint32_t compared = 0;
        int32_t finest = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                CompareView(table, viewTable, LoadNestCell(nest, slot, index), block.level);
                ++compared;
            }

            finest = std::max(finest, block.level);
        }

        EXPECT(finest == test::CHAIN_LEVELS);
        std::printf("多重解像度の木: %u セル(いちばん細かいレベル %d)\n", compared, finest);

        // 根のセルの場面(木箱。multires_test_scene)もレベル 0 で
        for (const reaction::RxCell& cell : test::MakeMultiresRootCells(table))
            CompareView(table, viewTable, cell, 0);
    }

    // --- 3. 成分の数に上限の無い形のセル ---
    void TestWideCells(const BakedReactionTable& table, const CellViewTable& viewTable) {
        std::vector<SpeciesAmount> amounts;
        for (uint32_t id = 1; id < table.species.size() && amounts.size() < 9; ++id)
            amounts.push_back({id, table.speciesCondensed[id] != 0 ? 1000000000ull : 1000000ull});

        // 8 成分まで: 狭い形と同じ見立て
        const std::span<const SpeciesAmount> narrow(amounts.data(), 8);
        const CvCellView fromNarrow = MakeCellView(viewTable, MakeReactionCell(table, narrow, 400000), 2, 0);
        const CvCellView fromWide = MakeCellView(viewTable, MakeWideReactionCell(table, narrow, 400000), 2, 0);
        EXPECT(fromNarrow.thermal.temperature == fromWide.thermal.temperature);
        EXPECT(fromNarrow.faceLimit == fromWide.faceLimit);
        EXPECT(fromNarrow.gasPressure == fromWide.gasPressure);
        EXPECT(fromNarrow.solid.volume == fromWide.solid.volume);
        EXPECT(fromNarrow.liquidEquivalentMass == fromWide.liquidEquivalentMass);

        // 9 成分: 熱は MrCellThermal と同じ
        EXPECT(amounts.size() == 9);
        const reaction::RxWideCell wide = MakeWideReactionCell(table, amounts, 400000);
        const CvCellView view = MakeCellView(viewTable, wide, 0, 0);
        const MrThermal thermal = MrCellThermal(table.View(), wide);
        EXPECT(view.thermal.temperature == thermal.temperature);
        EXPECT(view.thermal.conductance == thermal.conductance);
        EXPECT(view.thermal.capacityLimit == thermal.capacityLimit);
    }

    // --- 4. 費用(1 スレッド。見立てを作る時間と今の別々の数え方の時間)---
    void MeasureCost(const BakedReactionTable& table, const CellViewTable& viewTable) {
        const std::vector<reaction::RxCell> cells = MakeVariedCells(table, 4096);
        const ReactionTableView reactionView = table.View();
        constexpr uint32_t ROUNDS = 50;
        using Clock = std::chrono::steady_clock;

        uint64_t sink = 0;
        const Clock::time_point viewBegin = Clock::now();
        for (uint32_t round = 0; round < ROUNDS; ++round) {
            for (const reaction::RxCell& cell : cells) {
                const CvCellView view = MakeCellView(viewTable, cell, 0, REFERENCE_PRESSURE);
                sink += view.faceLimit + view.gasOpen + view.liquidOpen + (uint64_t)view.gasPressure +
                        view.liquidEquivalentMass;
            }
        }

        const Clock::time_point viewEnd = Clock::now();
        for (uint32_t round = 0; round < ROUNDS; ++round) {
            for (const reaction::RxCell& cell : cells) {
                const MrThermal thermal = MrCellThermal(reactionView, cell);
                const CellVolumeShare share = ShareCellVolume(table, cell);
                sink += MrFaceLimit(thermal, 0) + GasOpenQ16(share, PROBE_CELL_VOLUME_PICOLITERS) +
                        LiquidOpenQ16(share, PROBE_CELL_VOLUME_PICOLITERS) + ProbeLiquidField::LiquidMass(table, cell) +
                        ProbeLiquidField::LiquidVolume(table, cell);
            }
        }

        const Clock::time_point oldEnd = Clock::now();
        const uint64_t calls = uint64_t{ROUNDS} * cells.size();
        const auto perCell = [&](Clock::duration duration) {
            return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() * 10 / calls;
        };
        const uint64_t viewTenths = perCell(viewEnd - viewBegin);
        const uint64_t oldTenths = perCell(oldEnd - viewEnd);
        std::printf("費用: 見立て %llu.%llu ns/セル・今の別々の数え方 %llu.%llu ns/セル(%llu 回。sink %llu)\n",
                    (unsigned long long)(viewTenths / 10), (unsigned long long)(viewTenths % 10),
                    (unsigned long long)(oldTenths / 10), (unsigned long long)(oldTenths % 10),
                    (unsigned long long)calls, (unsigned long long)(sink & 0xff));
    }

}  // namespace

int main() {
    const BakedReactionTable table = MakeTable();
    const std::vector<CvSpeciesPhase> phases = MakeSpeciesPhases(table);
    const CellViewTable viewTable{.reaction = table.View(), .phases = phases};

    // 相の表: 水は液体・砂と石は固体・空気は気体
    EXPECT(phases[table.SpeciesId("liquid_water")].phase == CV_PHASE_LIQUID);
    EXPECT(phases[table.SpeciesId("grain")].phase == CV_PHASE_SOLID);
    EXPECT(phases[table.SpeciesId("loose_grain")].molarVolume == 0);
    EXPECT(phases[table.SpeciesId("nitrogen")].phase == CV_PHASE_GAS);

    TestVariedCells(table, viewTable);
    TestMultiresNest(table, viewTable);
    TestWideCells(table, viewTable);
    MeasureCost(table, viewTable);

    if (failureCount != 0) {
        std::printf("cell_view_test: %d 件の失敗\n", failureCount);
        return 1;
    }

    std::printf("cell_view_test: OK\n");
    return 0;
}
