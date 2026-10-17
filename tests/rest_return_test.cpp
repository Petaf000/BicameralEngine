// rest_return_test.cpp —
// 乱した気体・液体が眠りに戻るか(T-0723。研究。運ぶ計算の共通の土台の段 4 ①・20
// §2.6・ADR-0326)。
//
// 区画の箱をやめると(D-510)気体・液体は世界のどこでも流れ、費用は活性(眠る/起こす)で収める。眠れるのは、乱れが消えた後に
// セルと欄が「前の刻みとビット単位で同じ」に戻る時だけ。今の仮の世界の CPU
// の気体(sim/probe_gas)と液体(sim/probe_liquid)を
// 「世界いっぱい」の区画(区画の端 =
// 世界の端の壁)で流し、乱した後の活性のブロック(4³ セル)の割合を時間で追う:
//   気体: 静かな大気に 1 秒の火(1 セルに 15 刻みごとのつつき 4 回)。気体 + 伝導
//   + 反応(ProbeReference)と気体だけ(ProbeGasField)。 液体:
//   静かな池の水面に石を落とす(水面のセルを石にし、押しのけた水を上のセルへ)。液体だけ(ProbeLiquidField)。
// 活性 = セルが前の刻みと違う・運動量の欄が前と違う・運動量が 0
// でない(次の刻みに動く)のどれか。 戻らない時に何が残るかを分ける:
// 運動量の残り(音の揺れ・摩擦の効かない小さな運動量)/
// 熱だけの変化(伝導の小さな流れ)/ 運動量 0
// のまま物質が動く(丸めの往復)。静止の丸め(ProbeGasOptions::restSpeedUnits・ProbeLiquidOptions::
// restSpeedMicrometersPerSecond)を入れた時の戻り方も比べる。
//
// 既定(ctest):
// 小さい世界・短い刻みで、静止の丸めを入れても保存(元素・エネルギー・運動量 −
// 帳簿)が毎刻みビット単位で一定・ 2
// 回の実行が同じハッシュ、を確かめ、活性の推移を出す。`--long`:
// 打ち切り条件の場面(火が消えて 60 秒)を並列に流して推移を出す。 結果はチケット
// T-0723 に書く。浮動小数点は表示だけ。
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "reaction_test_table.h"
#include "sim/probe_gas.h"
#include "sim/probe_liquid.h"
#include "sim/probe_sim.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;
    std::mutex printMutex;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        const std::scoped_lock lock(printMutex);
        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint32_t BLOCK_EDGE = 4;  // 活性を数えるブロック(セル)
    constexpr uint64_t
        SLOW_GAS_MICROMETERS_PER_SECOND = 10000;  // 気体の「遅い」セル(1 cm/s。丸めの閾値を上げた時に眠れる数の見当)
    constexpr int32_t AIR_TEMPERATURE = 300000;   // mK
    constexpr uint64_t FIRE_TICKS = 60;           // 1 秒の火
    constexpr uint64_t FIRE_PULSE_INTERVAL = 15;  // つつきの間(4 回で約 2700 K × 4 の熱)
    constexpr uint64_t FULL_WATER = 6900000000;   // µmol(0.5 m 角の水の少し下。probe_liquid_test と同じ)
    constexpr uint64_t NITROGEN = 4000000;        // µmol
    constexpr uint64_t OXYGEN = 1000000;
    constexpr uint64_t STONE = 21000000000;  // µmol(密度 2000 でセルを満たす)

    // --- 場面の大きさ(区画 = 世界)---
    struct Scene {
        std::array<uint32_t, 3> minCell;
        std::array<uint32_t, 3> size;  // x・y(上)・z
        uint64_t quietTicks;           // 乱れが終わってから見る刻み
        uint64_t reportEvery;          // 推移を出す間(刻み)
    };

    // --- 1 刻みの活性 ---
    struct Activity {
        uint32_t activeCells = 0;
        uint32_t activeBlocks = 0;
        uint32_t movingCells = 0;     // 運動量が 0 でない(気体はセル・液体は + 側の 3 面のどれか)
        uint32_t heatOnlyCells = 0;   // 運動量が前も後も 0
                                      // で、物質は同じでエネルギーだけ変わった(伝導・熱の小さな流れ)
        uint32_t stillMoveCells = 0;  // 運動量が前も後も 0 なのに物質が変わった(丸めの往復)
        uint32_t
            smallMomentum = 0;  // 液体: 摩擦の効かない(|p| < 2^frictionShift)0 でない面・気体: 1 cm/s 未満で動くセル
        uint64_t maxSpeed = 0;  // 気体: µm/s・液体: 面の運動量の最大(mg)
        uint32_t rested = 0;    // 静止の丸めで止めたセル・面
    };

    struct RunResult {
        std::string name;
        std::vector<Activity> series;  // 乱れが終わった後の刻みごと
        uint32_t blocks = 0;
        bool conserved = true;
        uint64_t hash = 0;
        Scene scene{};
    };

    uint32_t BlockCount(const Scene& scene) {
        uint32_t count = 1;
        for (const uint32_t size : scene.size)
            count *= (size + BLOCK_EDGE - 1) / BLOCK_EDGE;

        return count;
    }

    // 区画の中の番号(x + 幅 × (z + 奥行き × y))→ ブロックの番号
    uint32_t BlockOf(const Scene& scene, uint32_t region) {
        const uint32_t x = region % scene.size[0];
        const uint32_t z = (region / scene.size[0]) % scene.size[2];
        const uint32_t y = region / (scene.size[0] * scene.size[2]);
        const uint32_t bx = (scene.size[0] + BLOCK_EDGE - 1) / BLOCK_EDGE;
        const uint32_t bz = (scene.size[2] + BLOCK_EDGE - 1) / BLOCK_EDGE;

        return (x / BLOCK_EDGE) + (bx * ((z / BLOCK_EDGE) + (bz * (y / BLOCK_EDGE))));
    }

    uint32_t GridIndexOf(const Scene& scene, uint32_t x, uint32_t y, uint32_t z) {
        return (scene.minCell[0] + x) + (PROBE_GRID_SIZE * (scene.minCell[1] + y)) +
               (PROBE_SLICE_CELL_COUNT * (scene.minCell[2] + z));
    }

    bool SameAmounts(const reaction::RxCell& a, const reaction::RxCell& b) {
        if (a.speciesCount != b.speciesCount)
            return false;

        for (uint32_t i = 0; i < a.speciesCount; ++i) {
            if (a.species[i] != b.species[i] || a.amounts[i] != b.amounts[i])
                return false;
        }

        return true;
    }

    // セル 1 つの活性を数える(moving* は運動量が 0 でないか、changedMomentum
    // は欄が前と違うか)
    void CountCell(const Scene& scene, uint32_t region, const reaction::RxCell& before, const reaction::RxCell& after,
                   bool movingBefore, bool movingAfter, bool changedMomentum, Activity& activity,
                   std::vector<uint8_t>& blockActive) {
        const bool sameAmounts = SameAmounts(before, after);
        const bool changed = !sameAmounts || before.energy != after.energy;
        if (!changed && !changedMomentum && !movingAfter)
            return;

        ++activity.activeCells;
        blockActive[BlockOf(scene, region)] = 1;
        activity.movingCells += movingAfter ? 1 : 0;
        if (movingBefore || movingAfter)
            return;

        activity.heatOnlyCells += changed && sameAmounts ? 1 : 0;
        activity.stillMoveCells += sameAmounts ? 0 : 1;
    }

    void Finish(Activity& activity, const std::vector<uint8_t>& blockActive) {
        activity.activeBlocks = (uint32_t)std::ranges::count(blockActive, 1);
    }

    // --- 推移の表示 ---
    void PrintRun(const Scene& scene, const RunResult& result) {
        const std::scoped_lock lock(printMutex);
        const uint32_t cells = scene.size[0] * scene.size[1] * scene.size[2];
        std::printf("== %s(区画 %u × %u × %u = %u セル・ブロック %u 個〔%u³〕・保存 %s)\n", result.name.c_str(),
                    scene.size[0], scene.size[1], scene.size[2], cells, result.blocks, BLOCK_EDGE,
                    result.conserved ? "yes" : "no");
        std::printf(
            "   秒  活性のセル  活性のブロック  動くセル  熱だけ  丸めの往復 "
            " 小さい運動量  最大の速さ  丸めた\n");
        uint64_t firstBelow = 0;  // 活性のブロックが 10% 以下に初めて戻った刻み(0 = 戻らない)
        uint64_t lastAbove = 0;   // 10% を超えていた最後の刻み
        uint64_t firstSilent = 0;
        for (uint64_t tick = 0; tick < result.series.size(); ++tick) {
            const Activity& a = result.series[tick];
            const bool below = (uint64_t)a.activeBlocks * 10 <= result.blocks;
            if (below && firstBelow == 0)
                firstBelow = tick + 1;

            if (!below)
                lastAbove = tick + 1;

            if (a.activeCells == 0 && firstSilent == 0)
                firstSilent = tick + 1;

            if ((tick + 1) % scene.reportEvery != 0)
                continue;

            std::printf("  %4.1f  %9u  %6u (%5.1f%%)  %8u  %6u  %10u  %12u  %10llu  %6u\n", (double)(tick + 1) / 60.0,
                        a.activeCells, a.activeBlocks, 100.0 * a.activeBlocks / result.blocks, a.movingCells,
                        a.heatOnlyCells, a.stillMoveCells, a.smallMomentum, (unsigned long long)a.maxSpeed, a.rested);
        }

        std::printf(
            "   → 10%% 以下に初めて戻った %.2f 秒・10%% を超えていた最後 "
            "%.2f 秒・全部静止 %.2f 秒(0 = 起きない)\n",
            (double)firstBelow / 60.0, (double)lastAbove / 60.0, (double)firstSilent / 60.0);
    }

}  // namespace

// ============================================================================================
// 気体
// ============================================================================================
namespace {

    // 試験の表から CO を作る規則を除き、基準の大気(N2 79%・O2 21%・300 K・1
    // 気圧)を足したもの(probe_gas_test と同じ)
    BakedReactionTable MakeGasTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        std::erase_if(definition.rules, [](const RuleDefinition& rule) {
            return std::ranges::any_of(rule.products,
                                       [](const RuleTerm& term) { return term.species == "carbon_monoxide"; });
        });

        definition.environment = StandardWorldPhysics();
        definition.atmosphere = {
            .points = {{.heightMillimeters = 0,
                        .temperatureMilliKelvin = (uint32_t)AIR_TEMPERATURE,
                        .fractions = {{.species = "nitrogen", .partsPerMillion = 790000},
                                      {.species = "oxygen", .partsPerMillion = 210000}}}},
            .surfacePressurePascals = 101325,
        };

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    ProbeGasOptions MakeGasOptions(const Scene& scene, uint32_t restSpeedUnits) {
        return {
            .minCell = scene.minCell, .sizeCells = scene.size, .randomSeed = 12345, .restSpeedUnits = restSpeedUnits};
    }

    std::vector<reaction::RxCell> MakeAirWorld(const BakedReactionTable& table, const ProbeGasField& field) {
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const uint32_t index = field.GridIndex(region);
            cells[index] = field.ReferenceCell(table, (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE);
        }

        return cells;
    }

    // 気体だけの場面の火のつつき 1 回の熱(mJ。基準の大気のセルを約 2700 K
    // 温める。ProbeReference のつつきと同じ見当)
    int64_t PulseEnergy(const BakedReactionTable& table, const reaction::RxCell& air) {
        std::vector<SpeciesAmount> amounts;
        for (uint32_t i = 0; i < air.speciesCount; ++i)
            amounts.push_back({.species = air.species[i], .amount = air.amounts[i]});

        return MakeReactionCell(table, amounts, AIR_TEMPERATURE + (int32_t)PROBE_POKE_HEATING_MILLIKELVIN).energy -
               MakeReactionCell(table, amounts, AIR_TEMPERATURE).energy;
    }

    struct GasTotals {
        std::vector<uint64_t> elements;
        int64_t energy = 0;
        std::array<int64_t, 3> momentum = {};

        bool operator==(const GasTotals&) const = default;
    };

    GasTotals SumGas(const BakedReactionTable& table, std::span<const reaction::RxCell> cells,
                     const ProbeGasField& field) {
        GasTotals totals;
        totals.elements.assign(table.elementNames.size(), 0);
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const reaction::RxCell& cell = cells[field.GridIndex(region)];
            totals.energy += cell.energy;
            const std::vector<uint64_t> elements = CountElements(table, cell);
            for (size_t e = 0; e < elements.size(); ++e)
                totals.elements[e] += elements[e];
        }

        totals.energy += field.KineticEnergySum() - field.Ledger().energy;
        constexpr std::array<uint32_t, 3> GRID_AXIS = {0, 2, 1};  // 気体の欄の軸(x・y・上)→ 格子の軸
        for (const std::array<int64_t, 3>& momentum : field.Momentum()) {
            for (uint32_t axis = 0; axis < 3; ++axis)
                totals.momentum[GRID_AXIS[axis]] += momentum[axis];
        }

        for (uint32_t axis = 0; axis < 3; ++axis)
            totals.momentum[axis] -= field.Ledger().momentum[axis];

        return totals;
    }

    // 刻みの後の活性(cells は区画を含む格子の全部のセル)
    Activity MeasureGas(const Scene& scene, const ProbeGasField& field, std::span<const reaction::RxCell> before,
                        std::span<const reaction::RxCell> after, std::span<const std::array<int64_t, 3>> momentumBefore,
                        uint32_t rested) {
        Activity activity;
        activity.rested = rested;
        std::vector<uint8_t> blockActive(BlockCount(scene), 0);
        const std::array<int64_t, 3> zero = {};
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const std::array<int64_t, 3>& momentum = field.Momentum()[region];
            const uint32_t index = field.GridIndex(region);
            CountCell(scene, region, before[index], after[index], momentumBefore[region] != zero, momentum != zero,
                      momentumBefore[region] != momentum, activity, blockActive);
            const uint64_t mass = field.Mass()[region];
            if (mass == 0)
                continue;

            uint64_t cellSpeed = 0;  // µm/s(軸ごとの速さの最大)
            for (const int64_t value : momentum) {
                const uint64_t units = (value < 0 ? (uint64_t)-value : (uint64_t)value) / mass;  // 2^-20 m/s
                cellSpeed = std::max(cellSpeed, (units * 1000000) >> 20);
            }

            activity.maxSpeed = std::max(activity.maxSpeed, cellSpeed);
            activity.smallMomentum += momentum != zero && cellSpeed < SLOW_GAS_MICROMETERS_PER_SECOND ? 1 : 0;
        }

        Finish(activity, blockActive);

        return activity;
    }

    // 気体 + 伝導 + 反応(ProbeReference)。火 = 区画の真ん中の床の上のセルへのつつき
    RunResult RunGasReference(const BakedReactionTable& table, const Scene& scene, uint32_t restSpeedUnits,
                              std::string name) {
        RunResult result{.name = std::move(name), .blocks = BlockCount(scene), .scene = scene};
        const auto field = ProbeGasField::Create(table, MakeGasOptions(scene, restSpeedUnits));
        EXPECT(field.has_value());
        if (!field)
            return result;

        ProbeReference reference(table, MakeAirWorld(table, *field));
        EXPECT(reference.EnableGas(MakeGasOptions(scene, restSpeedUnits)).has_value());
        const std::array<uint32_t, 3> fire = {scene.minCell[0] + (scene.size[0] / 2), scene.minCell[1] + 1,
                                              scene.minCell[2] + (scene.size[2] / 2)};
        GasTotals afterFire;
        for (uint64_t tick = 0; tick < FIRE_TICKS + scene.quietTicks; ++tick) {
            std::vector<ProbeCommand> commands;
            if (tick < FIRE_TICKS && tick % FIRE_PULSE_INTERVAL == 0)
                commands.push_back(MakePokeCommand(tick, 0, fire[0], fire[1], fire[2]));

            const std::vector<reaction::RxCell> before(reference.State(tick).begin(), reference.State(tick).end());
            const std::vector<std::array<int64_t, 3>> momentumBefore(reference.GasField()->Momentum().begin(),
                                                                     reference.GasField()->Momentum().end());
            reference.Advance(tick, commands);
            const ProbeGasField& gas = *reference.GasField();
            const GasTotals totals = SumGas(table, reference.State(tick + 1), gas);
            if (tick + 1 == FIRE_TICKS)
                afterFire = totals;

            if (tick + 1 <= FIRE_TICKS)
                continue;

            result.conserved = result.conserved && totals == afterFire;
            result.series.push_back(MeasureGas(scene, gas, before, reference.State(tick + 1), momentumBefore,
                                               reference.LastGasReport().restedCells));
        }

        const uint64_t ticks = FIRE_TICKS + scene.quietTicks;
        result.hash = reference.StateHash(ticks) ^ reference.GasField()->Hash();

        return result;
    }

    // 気体だけ(伝導・反応なし)。火 = 同じセルへ同じ見当の熱を直接足す
    RunResult RunGasOnly(const BakedReactionTable& table, const Scene& scene, uint32_t restSpeedUnits,
                         std::string name) {
        RunResult result{.name = std::move(name), .blocks = BlockCount(scene), .scene = scene};
        auto field = ProbeGasField::Create(table, MakeGasOptions(scene, restSpeedUnits));
        EXPECT(field.has_value());
        if (!field)
            return result;

        std::vector<reaction::RxCell> cells = MakeAirWorld(table, *field);
        const uint32_t fire = GridIndexOf(scene, scene.size[0] / 2, 1, scene.size[2] / 2);
        const int64_t pulse = PulseEnergy(table, cells[fire]);
        GasTotals afterFire;
        for (uint64_t tick = 0; tick < FIRE_TICKS + scene.quietTicks; ++tick) {
            if (tick < FIRE_TICKS && tick % FIRE_PULSE_INTERVAL == 0)
                cells[fire].energy += pulse;

            const std::vector<reaction::RxCell> before = cells;
            const std::vector<std::array<int64_t, 3>> momentumBefore(field->Momentum().begin(),
                                                                     field->Momentum().end());
            std::vector<uint32_t> changed;
            const ProbeGasStepReport report = field->Step(table, cells, tick, changed);
            const GasTotals totals = SumGas(table, cells, *field);
            if (tick + 1 == FIRE_TICKS)
                afterFire = totals;

            if (tick + 1 <= FIRE_TICKS)
                continue;

            result.conserved = result.conserved && totals == afterFire;
            result.series.push_back(MeasureGas(scene, *field, before, cells, momentumBefore, report.restedCells));
        }

        result.hash = field->Hash();
        for (const reaction::RxCell& cell : cells)
            result.hash = (result.hash * 1099511628211ull) ^ HashReactionCell(cell);

        return result;
    }

}  // namespace

// ============================================================================================
// 液体
// ============================================================================================
namespace {

    // 試験の表 + 液体の水 + 石(probe_liquid_test と同じ)
    BakedReactionTable MakeLiquidTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.species.push_back({.name = "liquid_water",
                                      .composition = {{.element = "H", .count = 2}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -285830,
                                      .heatCapacity = 75300,
                                      .thermalConductivity = 600,
                                      .phase = SpeciesPhase::Liquid,
                                      .density = 1000});
        definition.species.push_back({.name = "stone",
                                      .composition = {{.element = "C", .count = 1}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 8500,
                                      .thermalConductivity = 2000,
                                      .phase = SpeciesPhase::Solid,
                                      .density = 2000});

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    reaction::RxCell MakePondCell(const BakedReactionTable& table, bool stone, uint64_t water) {
        std::vector<SpeciesAmount> amounts = {{.species = table.SpeciesId("nitrogen"), .amount = NITROGEN},
                                              {.species = table.SpeciesId("oxygen"), .amount = OXYGEN}};
        if (water != 0)
            amounts.push_back({.species = table.SpeciesId("liquid_water"), .amount = water});

        if (stone)
            amounts = {{.species = table.SpeciesId("stone"), .amount = STONE}};

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(table, amounts, AIR_TEMPERATURE);
    }

    // 池: 床(y = 0)は石、y = 1 〜 depth は水、その上は空気。石を落とす = 水面の真ん中のセルを石にし、水を上のセルへ。
    // 水は 1 層なら初めから静止(TestStillPond)。積んだ水(depth ≥ 2)はセルが満ちきらない分だけ上の水が落ちて落ち着く
    std::vector<reaction::RxCell> MakePond(const BakedReactionTable& table, const Scene& scene, uint32_t depth,
                                           bool dropStone) {
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        const uint32_t surface = depth;
        for (uint32_t y = 0; y < scene.size[1]; ++y) {
            for (uint32_t z = 0; z < scene.size[2]; ++z) {
                for (uint32_t x = 0; x < scene.size[0]; ++x)
                    cells[GridIndexOf(scene, x, y, z)] = MakePondCell(table, y == 0, y <= surface ? FULL_WATER : 0);
            }
        }

        if (dropStone) {
            const uint32_t cx = scene.size[0] / 2;
            const uint32_t cz = scene.size[2] / 2;
            cells[GridIndexOf(scene, cx, surface, cz)] = MakePondCell(table, true, 0);
            cells[GridIndexOf(scene, cx, surface + 1, cz)] = MakePondCell(table, false, FULL_WATER);
        }

        return cells;
    }

    struct LiquidTotals {
        std::vector<uint64_t> amounts;
        int64_t energy = 0;
        std::array<int64_t, 3> momentum = {};

        bool operator==(const LiquidTotals&) const = default;
    };

    LiquidTotals SumLiquid(const BakedReactionTable& table, const ProbeLiquidField& field,
                           std::span<const reaction::RxCell> cells) {
        LiquidTotals totals;
        totals.amounts.assign(table.species.size(), 0);
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const reaction::RxCell& cell = cells[field.GridIndex(region)];
            totals.energy += cell.energy;
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                totals.amounts[cell.species[i]] += cell.amounts[i];
        }

        totals.energy += field.PotentialEnergySum() + field.KineticEnergySum() - field.Ledger().energy;
        for (uint32_t axis = 0; axis < 3; ++axis)
            totals.momentum[axis] = field.MomentumSum(axis) - field.Ledger().external[axis];

        return totals;
    }

    using FaceCopy = std::array<std::vector<int64_t>, 3>;

    FaceCopy CopyFaces(const ProbeLiquidField& field) {
        FaceCopy faces;
        for (uint32_t axis = 0; axis < 3; ++axis)
            faces[axis].assign(field.FaceMomentum(axis).begin(), field.FaceMomentum(axis).end());

        return faces;
    }

    Activity MeasureLiquid(const Scene& scene, const ProbeLiquidField& field, std::span<const reaction::RxCell> before,
                           std::span<const reaction::RxCell> after, const FaceCopy& facesBefore, uint32_t rested) {
        Activity activity;
        activity.rested = rested;
        std::vector<uint8_t> blockActive(BlockCount(scene), 0);
        const int64_t smallLimit = (int64_t)1 << field.Options().frictionShift;
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            bool movingBefore = false;
            bool movingAfter = false;
            bool changedMomentum = false;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const int64_t face = field.FaceMomentum(axis)[region];
                movingBefore = movingBefore || facesBefore[axis][region] != 0;
                movingAfter = movingAfter || face != 0;
                changedMomentum = changedMomentum || facesBefore[axis][region] != face;
                const uint64_t magnitude = face < 0 ? (uint64_t)-face : (uint64_t)face;
                activity.maxSpeed = std::max(activity.maxSpeed, magnitude);
                activity.smallMomentum += face != 0 && (int64_t)magnitude < smallLimit ? 1 : 0;
            }

            const uint32_t index = field.GridIndex(region);
            CountCell(scene, region, before[index], after[index], movingBefore, movingAfter, changedMomentum, activity,
                      blockActive);
        }

        Finish(activity, blockActive);

        return activity;
    }

    // 池(深さ depth)に石を落として(dropStone)quietTicks 刻み流す(液体だけ)
    RunResult RunPond(const BakedReactionTable& table, const Scene& scene, uint32_t depth, bool dropStone,
                      uint32_t restMicrometersPerSecond, std::string name) {
        RunResult result{.name = std::move(name), .blocks = BlockCount(scene), .scene = scene};
        auto field = ProbeLiquidField::Create(table, {.minCell = scene.minCell,
                                                      .sizeCells = scene.size,
                                                      .restSpeedMicrometersPerSecond = restMicrometersPerSecond});
        EXPECT(field.has_value());
        if (!field)
            return result;

        std::vector<reaction::RxCell> cells = MakePond(table, scene, depth, dropStone);
        LiquidTotals first;
        for (uint64_t tick = 0; tick < scene.quietTicks; ++tick) {
            const std::vector<reaction::RxCell> before = cells;
            const FaceCopy facesBefore = CopyFaces(*field);
            std::vector<uint32_t> changed;
            const ProbeLiquidStepReport report = field->Step(table, cells, changed);
            const LiquidTotals totals = SumLiquid(table, *field, cells);
            if (tick == 0)
                first = totals;  // 石を置いた分(区画の外の段の変化)は最初の刻みで帳簿に入る

            result.conserved = result.conserved && totals == first;
            result.series.push_back(MeasureLiquid(scene, *field, before, cells, facesBefore, report.restedFaces));
        }

        result.hash = field->Hash();
        for (const reaction::RxCell& cell : cells)
            result.hash = (result.hash * 1099511628211ull) ^ HashReactionCell(cell);

        return result;
    }

    // 石を落とさない池は 1 刻め から静止(物差しの確かめ)
    void TestStillPond(const BakedReactionTable& table, const Scene& scene) {
        auto field = ProbeLiquidField::Create(table, {.minCell = scene.minCell, .sizeCells = scene.size});
        EXPECT(field.has_value());
        if (!field)
            return;

        std::vector<reaction::RxCell> cells = MakePond(table, scene, 1, false);
        const std::vector<reaction::RxCell> before = cells;
        const FaceCopy faces = CopyFaces(*field);
        std::vector<uint32_t> changed;
        (void)field->Step(table, cells, changed);
        const Activity activity = MeasureLiquid(scene, *field, before, cells, faces, 0);
        EXPECT(activity.activeCells == 0);
    }

}  // namespace

// ============================================================================================
int main(int argc, char** argv) {
    const bool longRun = argc > 1 && std::string_view(argv[1]) == "--long";
    const BakedReactionTable gasTable = MakeGasTable();
    const BakedReactionTable liquidTable = MakeLiquidTable();

    // 区画 = 世界(端は壁)。小さい場面は ctest、--long
    // は打ち切り条件の場面(火が消えて 60 秒)
    const Scene gasScene = longRun ? Scene{{24, 0, 24}, {12, 12, 12}, 3600, 300}
                                   : Scene{{24, 0, 24}, {8, 8, 8}, 240, 60};
    const Scene pondScene = longRun ? Scene{{16, 0, 16}, {32, 4, 32}, 3600, 300}
                                    : Scene{{16, 0, 16}, {12, 4, 12}, 600, 120};
    const Scene deepScene = longRun ? Scene{{16, 0, 16}, {16, 6, 16}, 3600, 300}
                                    : Scene{{16, 0, 16}, {8, 6, 8}, 600, 120};

    TestStillPond(liquidTable, pondScene);

    // 1 単位(2^-20 m/s)と 2^10 単位(約 1 mm/s)の静止の丸め。液体は 1 µm/s と 1
    // mm/s
    std::vector<std::function<RunResult()>> jobs = {
        [&] { return RunGasReference(gasTable, gasScene, 0, "気体 + 伝導 + 反応・丸めなし"); },
        [&] { return RunGasReference(gasTable, gasScene, 1, "気体 + 伝導 + 反応・静止の丸め 1 単位"); },
        [&] { return RunGasOnly(gasTable, gasScene, 0, "気体だけ・丸めなし"); },
        [&] { return RunGasOnly(gasTable, gasScene, 1, "気体だけ・静止の丸め 1 単位"); },
        [&] { return RunGasOnly(gasTable, gasScene, 1024, "気体だけ・静止の丸め 1024 単位(約 1 mm/s)"); },
        [&] { return RunPond(liquidTable, pondScene, 1, true, 0, "池に石・丸めなし"); },
        [&] { return RunPond(liquidTable, pondScene, 1, true, 1, "池に石・静止の丸め 1 µm/s"); },
        [&] { return RunPond(liquidTable, pondScene, 1, true, 1000, "池に石・静止の丸め 1 mm/s"); },
        [&] { return RunPond(liquidTable, deepScene, 3, false, 0, "積んだ水(3 層)が落ち着く・丸めなし"); },
        [&] { return RunPond(liquidTable, deepScene, 3, false, 1000, "積んだ水(3 層)が落ち着く・静止の丸め 1 mm/s"); },
    };
    if (longRun)
        jobs.push_back(
            [&] { return RunGasReference(gasTable, gasScene, 1024, "気体 + 伝導 + 反応・静止の丸め 1024 単位"); });
    else
        jobs.push_back([&] { return RunGasOnly(gasTable, gasScene, 1, "気体だけ・静止の丸め 1 単位(2 回目)"); });

    std::vector<RunResult> results(jobs.size());
    std::vector<std::thread> threads;
    for (size_t i = 0; i < jobs.size(); ++i)
        threads.emplace_back([&, i] { results[i] = jobs[i](); });

    for (std::thread& thread : threads)
        thread.join();

    for (const RunResult& result : results) {
        PrintRun(result.scene, result);
        EXPECT(result.conserved);
    }

    if (!longRun)
        EXPECT(results[3].hash == results.back().hash);  // 静止の丸めも決定的

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("すべて通った\n");
    return 0;
}
