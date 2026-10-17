// cell_volume_test.cpp — 1 セルの体積を相で分け合う(T-0593・T-0612・ADR-0268。sim/cell_volume・sim/probe_gas・sim/probe_liquid)の試験。
//
// 確かめること(仮の世界の CPU リファレンス。気体と液体の区画の Step を直接回す):
//   1. 気体: 圧力の高い部屋と低い部屋の間の砂の壁。砂が無い / 1 µmol / 半分 / 満ちる / 密度を書かない砂 で、向こうへ渡る気体の量を比べる。
//      満ちた壁は 0 µmol(ビット単位)・1 µmol と密度の無い砂は砂の無い壁とほぼ同じ・半分は渡り始めが遅い
//      (終わりは圧力が釣り合うので同じ量に近い)。毎刻み 元素・エネルギー・運動量が帳簿込みで一定
//   2. 液体: 水の溜まりと空き地の間の砂の壁で同じ比べ方(満ちた壁は 0 µmol)。孔のある砂の床に水が染み込む。毎刻み 水の量と運動量が一定
//   3. 燃えて固体が減る: 満ちた壁を途中で半分に減らす(反応が固体を使った時と同じくセルの成分が変わる)と、気体も水も渡り始める
//   4. 液体が入ると空気が押し出される: 気体と液体の区画を重ね、上の水が下へ落ちると、下のセルの空気が上へ移る
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "reaction_test_table.h"
#include "sim/cell_volume.h"
#include "sim/probe_gas.h"
#include "sim/probe_liquid.h"
#include "sim/probe_sim.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr int32_t AIR_TEMPERATURE = 300000;  // mK
    constexpr uint64_t FULL_WATER = 6900000000;  // µmol(0.5 m 角の水 ≒ 6939 mol の少し下)
    constexpr uint64_t NITROGEN = 4000000;       // µmol(液体の区画の空気。0.125 m³・300 K・1 気圧)
    constexpr uint64_t OXYGEN = 1000000;

    // 試験の表 + 基準の大気(N2・O2)+ 水(液体)・砂(密度あり)・密度を書かない砂・石(密度あり)
    BakedReactionTable MakeTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.atmosphere = {
            .points = {{.heightMillimeters = 0,
                        .temperatureMilliKelvin = (uint32_t)AIR_TEMPERATURE,
                        .fractions = {{.species = "nitrogen", .partsPerMillion = 790000},
                                      {.species = "oxygen", .partsPerMillion = 210000}}}},
            .surfacePressurePascals = 101325,
        };
        definition.species.push_back({.name = "liquid_water",
                                      .composition = {{.element = "H", .count = 2}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -285830,
                                      .heatCapacity = 75300,
                                      .thermalConductivity = 600,
                                      .phase = SpeciesPhase::Liquid,
                                      .density = 1000});
        const auto solid = [](const char* name, uint32_t density) {
            return SpeciesDefinition{.name = name,
                                     .composition = {{.element = "C", .count = 1}},
                                     .formationEnthalpy = 0,
                                     .heatCapacity = 8500,
                                     .thermalConductivity = 2000,
                                     .phase = SpeciesPhase::Solid,
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

    // 壁の砂
    enum class Barrier : uint8_t { None, Tiny, Half, Packed, Loose };

    const char* BarrierName(Barrier barrier) {
        switch (barrier) {
            case Barrier::None: return "砂なし";
            case Barrier::Tiny: return "砂 1 µmol";
            case Barrier::Half: return "砂が体積の半分";
            case Barrier::Packed: return "砂で満ちる";
            case Barrier::Loose: return "密度を書かない砂(満ちる量)";
        }

        return "";
    }

    // 砂(密度あり)がセルの体積の num/den を占める物質量(µmol。満ちる時は 1 µmol 多く)
    uint64_t FillAmount(const BakedReactionTable& table, uint64_t num, uint64_t den) {
        const uint64_t molarVolume = table.speciesMolarVolume[table.SpeciesId("grain")];
        const uint64_t amount = PROBE_CELL_VOLUME_PICOLITERS * num / den / molarVolume;
        return num == den ? amount + 1 : amount;
    }

    // 壁のセルの砂(物質 ID・量)。無ければ量 0
    SpeciesAmount BarrierGrain(const BakedReactionTable& table, Barrier barrier) {
        const uint32_t grain = table.SpeciesId("grain");
        switch (barrier) {
            case Barrier::None: return {.species = grain, .amount = 0};
            case Barrier::Tiny: return {.species = grain, .amount = 1};
            case Barrier::Half: return {.species = grain, .amount = FillAmount(table, 1, 2)};
            case Barrier::Packed: return {.species = grain, .amount = FillAmount(table, 1, 1)};
            case Barrier::Loose: return {.species = table.SpeciesId("loose_grain"), .amount = FillAmount(table, 1, 1)};
        }

        return {};
    }

    reaction::RxCell MakeCell(const BakedReactionTable& table, std::vector<SpeciesAmount> amounts) {
        std::erase_if(amounts, [](const SpeciesAmount& entry) { return entry.amount == 0; });
        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(table, amounts, AIR_TEMPERATURE);
    }

    // 気体の量の合計(凝縮相でない物質)
    uint64_t GasAmount(const BakedReactionTable& table, const reaction::RxCell& cell) {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (table.speciesCondensed[cell.species[i]] == 0)
                sum += cell.amounts[i];
        }

        return sum;
    }

    uint64_t AmountOf(const reaction::RxCell& cell, uint32_t species) {
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (cell.species[i] == species)
                return cell.amounts[i];
        }

        return 0;
    }

    // --- 保存の合計(気体: セル + 運動エネルギーのぶん − 帳簿。probe_gas_test と同じ形)---
    struct Totals {
        std::vector<uint64_t> elements;
        int64_t energy = 0;
        std::array<int64_t, 3> momentum = {};

        bool operator==(const Totals&) const = default;
    };

    Totals Sum(const BakedReactionTable& table, std::span<const reaction::RxCell> cells, const ProbeGasField& gas) {
        Totals totals;
        totals.elements.assign(table.elementNames.size(), 0);
        for (const reaction::RxCell& cell : cells) {
            totals.energy += cell.energy;
            const std::vector<uint64_t> elements = CountElements(table, cell);
            for (size_t e = 0; e < elements.size(); ++e)
                totals.elements[e] += elements[e];
        }

        for (size_t species = 0; species < gas.Ledger().amounts.size(); ++species) {
            const int64_t amount = gas.Ledger().amounts[species];
            for (size_t e = 0; e < totals.elements.size(); ++e)
                totals.elements[e] -= (uint64_t)amount *
                                      table.speciesElements[(species * table.elementNames.size()) + e];
        }

        totals.energy += gas.KineticEnergySum() - gas.Ledger().energy;
        constexpr std::array<uint32_t, 3> GRID_AXIS = {0, 2, 1};
        for (const std::array<int64_t, 3>& momentum : gas.Momentum()) {
            for (uint32_t axis = 0; axis < 3; ++axis)
                totals.momentum[GRID_AXIS[axis]] += momentum[axis];
        }

        for (uint32_t axis = 0; axis < 3; ++axis)
            totals.momentum[axis] -= gas.Ledger().momentum[axis];

        return totals;
    }

    // ===== 1. 気体: 圧力の高い部屋 | 砂の壁 | 低い部屋(区画 9 × 2 × 2・閉じた壁)=====
    constexpr std::array<uint32_t, 3> GAS_MIN = {20, 0, 20};
    constexpr std::array<uint32_t, 3> GAS_SIZE = {9, 2, 2};
    constexpr uint32_t GAS_BARRIER_X = 4;
    constexpr uint64_t GAS_TICKS = 60;       // 1 秒
    constexpr uint64_t GAS_EARLY_TICKS = 2;  // 33 ms

    ProbeGasOptions GasOptions(std::array<uint32_t, 3> minCell, std::array<uint32_t, 3> size) {
        return {.minCell = minCell, .sizeCells = size, .randomSeed = 777};
    }

    std::array<uint32_t, 3> RegionCoordinates(uint32_t region, std::array<uint32_t, 3> size) {
        return {region % size[0], region / (size[0] * size[2]), (region / size[0]) % size[2]};
    }

    std::vector<reaction::RxCell> MakeGasWorld(const BakedReactionTable& table, const ProbeGasField& field,
                                               Barrier barrier) {
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        const SpeciesAmount grain = BarrierGrain(table, barrier);
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const auto at = RegionCoordinates(region, GAS_SIZE);
            const reaction::RxCell air = field.ReferenceCell(table, GAS_MIN[1] + at[1]);
            std::vector<SpeciesAmount> amounts;
            for (uint32_t i = 0; i < air.speciesCount; ++i) {
                uint64_t amount = air.amounts[i];
                if (at[0] < GAS_BARRIER_X)
                    amount = amount * 3 / 2;  // 圧力の高い部屋(1.5 気圧)

                if (at[0] == GAS_BARRIER_X && barrier == Barrier::Half)
                    amount /= 2;  // 孔の空気(同じ圧力)

                if (at[0] == GAS_BARRIER_X && barrier == Barrier::Packed)
                    amount = 0;

                amounts.push_back({.species = air.species[i], .amount = amount});
            }

            if (at[0] == GAS_BARRIER_X)
                amounts.push_back(grain);

            cells[field.GridIndex(region)] = MakeCell(table, amounts);
        }

        return cells;
    }

    // 低い部屋(壁の向こう)の気体の量
    uint64_t FarGas(const BakedReactionTable& table, const ProbeGasField& field,
                    std::span<const reaction::RxCell> cells) {
        uint64_t sum = 0;
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            if (RegionCoordinates(region, GAS_SIZE)[0] > GAS_BARRIER_X)
                sum += GasAmount(table, cells[field.GridIndex(region)]);
        }

        return sum;
    }

    // 壁の砂を半分にする(燃えて固体が減った時と同じく、セルの成分が変わる)
    void BurnBarrier(const BakedReactionTable& table, std::span<reaction::RxCell> cells, uint32_t index) {
        const uint32_t grain = table.SpeciesId("grain");
        std::vector<SpeciesAmount> amounts;
        for (uint32_t i = 0; i < cells[index].speciesCount; ++i) {
            const uint64_t amount = cells[index].species[i] == grain ? FillAmount(table, 1, 2)
                                                                     : cells[index].amounts[i];
            amounts.push_back({.species = cells[index].species[i], .amount = amount});
        }

        cells[index] = MakeCell(table, amounts);
    }

    struct GasRun {
        int64_t crossed = 0;  // 低い部屋へ渡った気体(µmol)
        int64_t
            crossedEarly = 0;  // 渡り始め(GAS_EARLY_TICKS 刻み)。終わりは圧力が釣り合うので、通りやすさはこちらで比べる
        bool conserved = true;
        uint32_t blockedCells = 0;
        uint32_t partialCells = 0;
        uint64_t hash = 0;
    };

    // burnTick が 0 でなければ、その刻みの前に壁の砂を半分にする
    GasRun RunGas(const BakedReactionTable& table, Barrier barrier, uint64_t ticks, uint64_t burnTick) {
        GasRun run;
        auto field = ProbeGasField::Create(table, GasOptions(GAS_MIN, GAS_SIZE));
        EXPECT(field.has_value());
        if (!field)
            return run;

        std::vector<reaction::RxCell> cells = MakeGasWorld(table, *field, barrier);
        const uint64_t farBefore = FarGas(table, *field, cells);
        Totals baseline = Sum(table, cells, *field);
        std::vector<uint32_t> changed;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            if (burnTick != 0 && tick == burnTick) {
                for (uint32_t region = 0; region < field->RegionCellCount(); ++region) {
                    if (RegionCoordinates(region, GAS_SIZE)[0] == GAS_BARRIER_X)
                        BurnBarrier(table, cells, field->GridIndex(region));
                }

                baseline = Sum(table, cells, *field);
            }

            const ProbeGasStepReport report = field->Step(table, cells, tick, changed);
            EXPECT(!report.skippedCellOverflow);
            run.blockedCells = std::max(run.blockedCells, report.blockedCells);
            run.partialCells = std::max(run.partialCells, report.partialCells);
            run.conserved = run.conserved && Sum(table, cells, *field) == baseline;
            if (tick + 1 == GAS_EARLY_TICKS)
                run.crossedEarly = (int64_t)FarGas(table, *field, cells) - (int64_t)farBefore;
        }

        run.crossed = (int64_t)FarGas(table, *field, cells) - (int64_t)farBefore;
        run.hash = field->Hash();
        for (uint32_t region = 0; region < field->RegionCellCount(); ++region)
            run.hash ^= HashReactionCell(cells[field->GridIndex(region)]) + region;

        return run;
    }

    void TestGasThroughGrain(const BakedReactionTable& table) {
        std::array<GasRun, 5> runs;
        for (uint32_t b = 0; b < runs.size(); ++b) {
            runs[b] = RunGas(table, (Barrier)b, GAS_TICKS, 0);
            std::printf("気体 %s: 向こうへ 33 ms で %lld・1 秒で %lld µmol・保存 %s・満ちたセル %u・一部 %u\n",
                        BarrierName((Barrier)b), (long long)runs[b].crossedEarly, (long long)runs[b].crossed,
                        runs[b].conserved ? "yes" : "no", runs[b].blockedCells, runs[b].partialCells);
            EXPECT(runs[b].conserved);
        }

        const int64_t none = runs[(size_t)Barrier::None].crossed;
        EXPECT(none > 0);
        EXPECT(runs[(size_t)Barrier::Packed].crossed == 0);
        EXPECT(runs[(size_t)Barrier::Packed].blockedCells == 4);
        EXPECT(runs[(size_t)Barrier::Tiny].crossed * 100 >= none * 95);
        EXPECT(runs[(size_t)Barrier::Loose].crossed * 100 >= none * 95);  // 密度を書かない物は場所を取らない
        EXPECT(runs[(size_t)Barrier::Loose].partialCells == 0);
        EXPECT(runs[(size_t)Barrier::Half].crossedEarly > 0);
        EXPECT(runs[(size_t)Barrier::Half].crossedEarly < runs[(size_t)Barrier::None].crossedEarly);

        // 燃えて固体が減る: 満ちた壁を 0.5 秒で半分に
        const GasRun burn = RunGas(table, Barrier::Packed, GAS_TICKS, GAS_TICKS / 2);
        std::printf("気体 砂で満ちる → 0.5 秒で半分に燃える: 向こうへ %lld µmol・保存 %s\n", (long long)burn.crossed,
                    burn.conserved ? "yes" : "no");
        EXPECT(burn.crossed > 0);
        EXPECT(burn.conserved);

        // 決定性
        EXPECT(RunGas(table, Barrier::Half, 20, 0).hash == RunGas(table, Barrier::Half, 20, 0).hash);
    }

    // ===== 2. 液体: 水の溜まり | 砂の壁 | 空き地(区画 6 × 3 × 1。床 y 0 は石)=====
    constexpr std::array<uint32_t, 3> LIQUID_MIN = {20, 0, 40};
    constexpr std::array<uint32_t, 3> LIQUID_SIZE = {6, 3, 1};
    constexpr uint32_t LIQUID_BARRIER_X = 2;
    constexpr uint64_t LIQUID_TICKS = 900;        // 15 秒
    constexpr uint64_t LIQUID_EARLY_TICKS = 120;  // 2 秒
    // 渡り始めの目安: 向こうの水がこれに届いた刻み(T-0610 の移流で 2 秒には両方とも釣り合いを越えて揺れているので、量でなく届いた時で比べる)
    constexpr uint64_t LIQUID_REACH_MICROMOLES = FULL_WATER / 2;

    ProbeLiquidOptions LiquidOptions(std::array<uint32_t, 3> minCell, std::array<uint32_t, 3> size) {
        return {.minCell = minCell, .sizeCells = size};
    }

    std::vector<SpeciesAmount> Air(const BakedReactionTable& table) {
        return {{.species = table.SpeciesId("nitrogen"), .amount = NITROGEN},
                {.species = table.SpeciesId("oxygen"), .amount = OXYGEN}};
    }

    SpeciesAmount Stone(const BakedReactionTable& table) {
        const uint64_t molarVolume = table.speciesMolarVolume[table.SpeciesId("stone")];
        return {.species = table.SpeciesId("stone"), .amount = (PROBE_CELL_VOLUME_PICOLITERS / molarVolume) + 1};
    }

    std::vector<reaction::RxCell> MakeLiquidWorld(const BakedReactionTable& table, const ProbeLiquidField& field,
                                                  Barrier barrier) {
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const auto at = RegionCoordinates(region, LIQUID_SIZE);
            std::vector<SpeciesAmount> amounts = Air(table);
            if (at[1] == 0)
                amounts = {Stone(table)};
            else if (at[0] < LIQUID_BARRIER_X)
                amounts.push_back({.species = table.SpeciesId("liquid_water"), .amount = FULL_WATER});
            else if (at[0] == LIQUID_BARRIER_X)
                amounts.push_back(BarrierGrain(table, barrier));

            cells[field.GridIndex(region)] = MakeCell(table, amounts);
        }

        return cells;
    }

    uint64_t WaterWhere(const BakedReactionTable& table, const ProbeLiquidField& field,
                        std::span<const reaction::RxCell> cells, std::array<uint32_t, 3> size, uint32_t minX,
                        uint32_t minY) {
        uint64_t sum = 0;
        const uint32_t water = table.SpeciesId("liquid_water");
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const auto at = RegionCoordinates(region, size);
            if (at[0] >= minX && at[1] >= minY)
                sum += AmountOf(cells[field.GridIndex(region)], water);
        }

        return sum;
    }

    struct LiquidRun {
        uint64_t crossed = 0;       // 空き地(壁の向こう)の水(µmol)
        uint64_t crossedEarly = 0;  // 渡り始め(LIQUID_EARLY_TICKS 刻み)
        uint64_t reachTick = 0;     // 向こうの水が LIQUID_REACH_MICROMOLES に届いた刻み(0 = 届かない)
        bool conserved = true;
    };

    // 液体の帳簿込みの和(区画のセルのエネルギー + 位置 + 運動 − 帳簿 と、面の運動量 − 帳簿。T-0610)
    struct LiquidBook {
        int64_t energy = 0;  // mJ
        std::array<int64_t, 3> momentum = {};

        bool operator==(const LiquidBook&) const = default;
    };

    LiquidBook MeasureLiquidBook(const ProbeLiquidField& field, std::span<const reaction::RxCell> cells) {
        LiquidBook book;
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region)
            book.energy += cells[field.GridIndex(region)].energy;

        book.energy += field.PotentialEnergySum() + field.KineticEnergySum() - field.Ledger().energy;
        for (uint32_t axis = 0; axis < 3; ++axis)
            book.momentum[axis] = field.MomentumSum(axis) - field.Ledger().external[axis];

        return book;
    }

    bool LiquidConserved(const BakedReactionTable& table, const ProbeLiquidField& field,
                         std::span<const reaction::RxCell> cells, uint64_t water, const LiquidBook& before) {
        return WaterWhere(table, field, cells, LIQUID_SIZE, 0, 0) == water && MeasureLiquidBook(field, cells) == before;
    }

    LiquidRun RunLiquid(const BakedReactionTable& table, Barrier barrier, uint64_t ticks, uint64_t burnTick) {
        LiquidRun run;
        auto field = ProbeLiquidField::Create(table, LiquidOptions(LIQUID_MIN, LIQUID_SIZE));
        EXPECT(field.has_value());
        if (!field)
            return run;

        std::vector<reaction::RxCell> cells = MakeLiquidWorld(table, *field, barrier);
        const uint64_t water = WaterWhere(table, *field, cells, LIQUID_SIZE, 0, 0);
        std::vector<uint32_t> changed;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            if (burnTick != 0 && tick == burnTick) {
                for (uint32_t region = 0; region < field->RegionCellCount(); ++region) {
                    const auto at = RegionCoordinates(region, LIQUID_SIZE);
                    if (at[0] == LIQUID_BARRIER_X && at[1] != 0)
                        BurnBarrier(table, cells, field->GridIndex(region));
                }
            }

            const LiquidBook before = MeasureLiquidBook(*field, cells);  // 燃やした後(燃えた分は区画の外の段)
            (void)field->Step(table, cells, changed);
            run.conserved = run.conserved && LiquidConserved(table, *field, cells, water, before);
            const uint64_t crossed = WaterWhere(table, *field, cells, LIQUID_SIZE, LIQUID_BARRIER_X + 1, 0);
            if (run.reachTick == 0 && crossed >= LIQUID_REACH_MICROMOLES)
                run.reachTick = tick + 1;

            if (tick + 1 == LIQUID_EARLY_TICKS)
                run.crossedEarly = crossed;
        }

        run.crossed = WaterWhere(table, *field, cells, LIQUID_SIZE, LIQUID_BARRIER_X + 1, 0);

        return run;
    }

    void TestLiquidThroughGrain(const BakedReactionTable& table) {
        std::array<LiquidRun, 5> runs;
        for (uint32_t b = 0; b < runs.size(); ++b) {
            runs[b] = RunLiquid(table, (Barrier)b, LIQUID_TICKS, 0);
            std::printf(
                "液体 %s: 向こうへ 2 秒で %llu・15 秒で %llu µmol(水 %llu µmol のうち)・%llu µmol に届いた刻み "
                "%llu・保存 %s\n",
                BarrierName((Barrier)b), (unsigned long long)runs[b].crossedEarly, (unsigned long long)runs[b].crossed,
                (unsigned long long)(FULL_WATER * 4), (unsigned long long)LIQUID_REACH_MICROMOLES,
                (unsigned long long)runs[b].reachTick, runs[b].conserved ? "yes" : "no");
            EXPECT(runs[b].conserved);
        }

        const uint64_t none = runs[(size_t)Barrier::None].crossed;
        EXPECT(none > FULL_WATER);
        EXPECT(runs[(size_t)Barrier::Packed].crossed == 0);
        EXPECT(runs[(size_t)Barrier::Tiny].crossed * 100 >= none * 95);
        EXPECT(runs[(size_t)Barrier::Loose].crossed * 100 >= none * 95);
        EXPECT(runs[(size_t)Barrier::Half].crossedEarly > 0);
        EXPECT(runs[(size_t)Barrier::Packed].reachTick == 0);
        EXPECT(runs[(size_t)Barrier::None].reachTick != 0);
        EXPECT(runs[(size_t)Barrier::Half].reachTick > runs[(size_t)Barrier::None].reachTick);

        const LiquidRun burn = RunLiquid(table, Barrier::Packed, LIQUID_TICKS, LIQUID_TICKS / 3);
        std::printf("液体 砂で満ちる → 5 秒で半分に燃える: 向こうへ %llu µmol・保存 %s\n",
                    (unsigned long long)burn.crossed, burn.conserved ? "yes" : "no");
        EXPECT(burn.crossed > 0);
        EXPECT(burn.conserved);
    }

    // --- 孔のある砂の床に水が染み込む(1 × 3 × 1。y 0 石・y 1 砂が体積の半分・y 2 水 1/4 セル)---
    void TestSoak(const BakedReactionTable& table) {
        constexpr std::array<uint32_t, 3> MIN = {30, 0, 40};
        constexpr std::array<uint32_t, 3> SIZE = {1, 3, 1};
        auto field = ProbeLiquidField::Create(table, LiquidOptions(MIN, SIZE));
        EXPECT(field.has_value());
        if (!field)
            return;

        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        cells[field->GridIndex(0)] = MakeCell(table, {Stone(table)});
        std::vector<SpeciesAmount> bed = Air(table);
        bed.push_back(BarrierGrain(table, Barrier::Half));
        cells[field->GridIndex(1)] = MakeCell(table, bed);
        std::vector<SpeciesAmount> pool = Air(table);
        pool.push_back({.species = table.SpeciesId("liquid_water"), .amount = FULL_WATER / 4});
        cells[field->GridIndex(2)] = MakeCell(table, pool);

        // 孔の面(一部だけ開いた面)を落ちる水: 位置のエネルギーが熱になり、帳簿込みの和は毎刻みビット単位で変わらない
        const LiquidBook baseline = MeasureLiquidBook(*field, cells);
        bool conserved = true;
        std::vector<uint32_t> changed;
        for (uint64_t tick = 0; tick < 600; ++tick) {
            (void)field->Step(table, cells, changed);
            conserved = conserved && MeasureLiquidBook(*field, cells) == baseline;
        }

        const uint32_t water = table.SpeciesId("liquid_water");
        const uint64_t soaked = AmountOf(cells[field->GridIndex(1)], water);
        const uint64_t left = AmountOf(cells[field->GridIndex(2)], water);
        std::printf(
            "液体 砂の床に染みる: 10 秒で床の孔に %llu µmol・上に %llu µmol・熱になった位置のエネルギー %lld mJ・保存 "
            "%s\n",
            (unsigned long long)soaked, (unsigned long long)left,
            (long long)(field->Ledger().energy - field->PotentialEnergySum() - field->KineticEnergySum()),
            conserved ? "yes" : "no");
        EXPECT(conserved);
        EXPECT(soaked + left == FULL_WATER / 4);
        EXPECT(soaked * 10 >= (FULL_WATER / 4) * 9);
    }

    // ===== 4. 液体が入ると空気が押し出される(1 × 4 × 1。y 0 石・y 1〜3 空気・y 3 に水 1/2 セル)=====
    void TestDisplacement(const BakedReactionTable& table) {
        constexpr std::array<uint32_t, 3> MIN = {34, 0, 40};
        constexpr std::array<uint32_t, 3> SIZE = {1, 4, 1};
        auto gas = ProbeGasField::Create(table, GasOptions(MIN, SIZE));
        auto liquid = ProbeLiquidField::Create(table, LiquidOptions(MIN, SIZE));
        EXPECT(gas.has_value() && liquid.has_value());
        if (!gas || !liquid)
            return;

        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        cells[gas->GridIndex(0)] = MakeCell(table, {Stone(table)});
        for (uint32_t y = 1; y < 4; ++y) {
            const reaction::RxCell air = gas->ReferenceCell(table, y);
            std::vector<SpeciesAmount> amounts;
            for (uint32_t i = 0; i < air.speciesCount; ++i)
                amounts.push_back({.species = air.species[i], .amount = air.amounts[i] / (y == 3 ? 2 : 1)});

            if (y == 3)
                amounts.push_back({.species = table.SpeciesId("liquid_water"), .amount = FULL_WATER / 2});

            cells[gas->GridIndex(y)] = MakeCell(table, amounts);
        }

        const uint64_t bottomBefore = GasAmount(table, cells[gas->GridIndex(1)]);
        const Totals baseline = Sum(table, cells, *gas);
        bool conserved = true;
        std::vector<uint32_t> changed;
        for (uint64_t tick = 0; tick < 600; ++tick) {
            (void)gas->Step(table, cells, tick, changed);
            (void)liquid->Step(table, cells, changed);
            conserved = conserved && Sum(table, cells, *gas).elements == baseline.elements;
        }

        const uint32_t water = table.SpeciesId("liquid_water");
        const uint64_t bottomWater = AmountOf(cells[gas->GridIndex(1)], water);
        const uint64_t bottomAfter = GasAmount(table, cells[gas->GridIndex(1)]);
        std::printf(
            "押し出し: 10 秒で下のセルの水 %llu µmol(全部 %llu)・下のセルの空気 %llu → %llu µmol・元素の保存 %s\n",
            (unsigned long long)bottomWater, (unsigned long long)(FULL_WATER / 2), (unsigned long long)bottomBefore,
            (unsigned long long)bottomAfter, conserved ? "yes" : "no");
        EXPECT(conserved);
        EXPECT(bottomWater * 10 >= (FULL_WATER / 2) * 9);
        EXPECT(bottomAfter * 10 <= bottomBefore * 7);  // 水が半分を占め、空気の約半分が上へ移る
    }

    // --- 空き(sim/cell_volume)の値 ---
    void TestShare(const BakedReactionTable& table) {
        const reaction::RxCell packed = MakeCell(table, {BarrierGrain(table, Barrier::Packed)});
        const reaction::RxCell half = MakeCell(
            table, {BarrierGrain(table, Barrier::Half),
                    {.species = table.SpeciesId("liquid_water"), .amount = FULL_WATER / 4}});
        const reaction::RxCell loose = MakeCell(table, {BarrierGrain(table, Barrier::Loose)});
        const CellVolumeShare packedShare = ShareCellVolume(table, packed);
        const CellVolumeShare halfShare = ShareCellVolume(table, half);
        EXPECT(LiquidOpenQ16(packedShare, PROBE_CELL_VOLUME_PICOLITERS) == 0);
        EXPECT(GasOpenQ16(packedShare, PROBE_CELL_VOLUME_PICOLITERS) == 0);
        const uint32_t liquidOpen = LiquidOpenQ16(halfShare, PROBE_CELL_VOLUME_PICOLITERS);
        const uint32_t gasOpen = GasOpenQ16(halfShare, PROBE_CELL_VOLUME_PICOLITERS);
        EXPECT(liquidOpen >= CELL_OPEN_FULL / 2 && liquidOpen <= (CELL_OPEN_FULL / 2) + 2);
        EXPECT(gasOpen > CELL_OPEN_FULL / 4 && gasOpen < CELL_OPEN_FULL / 4 + 400);  // 水 1/4 セル弱
        EXPECT(GasOpenQ16(ShareCellVolume(table, loose), PROBE_CELL_VOLUME_PICOLITERS) == CELL_OPEN_FULL);
        std::printf("空き: 砂半分 + 水 1/4 → 液体の空き %u/65536・気体の空き %u/65536\n", liquidOpen, gasOpen);
    }

}  // namespace

int main() {
    const BakedReactionTable table = MakeTable();
    TestShare(table);
    TestGasThroughGrain(table);
    TestLiquidThroughGrain(table);
    TestSoak(table);
    TestDisplacement(table);

    std::printf("cell_volume_test: %s\n", failureCount == 0 ? "OK" : "FAILED");
    return failureCount == 0 ? 0 : 1;
}
