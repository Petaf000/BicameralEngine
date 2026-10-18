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

#include "common/liquid_flow.hlsli"
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
        // 液体: |p| < 2^frictionShift の 0 でない面(T-0776 の前は摩擦が効かなかった)・気体: 1 cm/s 未満で動くセル
        uint32_t smallMomentum = 0;
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

    // 刻み first から 60 刻みの間の、面の運動量の最大(液体)
    uint64_t PeakMomentum(const RunResult& result, size_t first) {
        uint64_t peak = 0;
        for (size_t tick = first; tick < first + 60 && tick < result.series.size(); ++tick)
            peak = std::max(peak, result.series[tick].maxSpeed);

        return peak;
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
            const ProbeLiquidStepReport report = field->Step(table, cells, tick, changed);
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

    // 1 列の積んだ水の刻みごとの値(T-0781 の測り。`--trace`): 質量が動いた刻みと 5 秒ごとに、y = 1〜3 の液体の質量・縦の面の運動量・
    // 縦の外からの運動量の内訳を出す
    void TraceColumn(const BakedReactionTable& table, uint32_t restMicrometersPerSecond, uint64_t ticks) {
        const Scene scene{{16, 0, 16}, {1, 6, 1}, ticks, 300};
        auto field = ProbeLiquidField::Create(table, {.minCell = scene.minCell,
                                                      .sizeCells = scene.size,
                                                      .restSpeedMicrometersPerSecond = restMicrometersPerSecond});
        if (!field)
            return;

        std::vector<reaction::RxCell> cells = MakePond(table, scene, 3, false);
        std::printf(
            "== 1 列の積んだ水・丸め %u µm/s\n  刻み  質量 y1 y2 y3(mg)  面 0-1 1-2 2-3 3-4  壁 重さ 摩擦 丸め  "
            "動いた\n",
            restMicrometersPerSecond);
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            std::vector<uint32_t> changed;
            const ProbeLiquidStepReport report = field->Step(table, cells, tick, changed);
            if (report.movedMilligrams == 0 && tick % 300 != 0)
                continue;

            const auto mass = [&](uint32_t y) {
                return (unsigned long long)ProbeLiquidField::LiquidMass(table, cells[GridIndexOf(scene, 0, y, 0)]);
            };
            const auto face = [&](uint32_t y) {
                return (long long)field->FaceMomentum(1)[y];  // 1 列なので区画の番号 = y
            };
            std::printf("  %5llu  %llu %llu %llu  %lld %lld %lld %lld  %lld %lld %lld %lld  %llu\n",
                        (unsigned long long)tick, mass(1), mass(2), mass(3), face(0), face(1), face(2), face(3),
                        (long long)report.wall[1], (long long)report.gravity[1], (long long)report.friction[1],
                        (long long)report.rested[1], (unsigned long long)report.movedMilligrams);
        }
    }

    // --- 動き続ける面の調べ(T-0782・T-0783。`--diagnose`)---
    // 面ごとの運動量・この刻みに動く質量の見込み(運動量 × g ÷ 流れの分母)・両側の質量・濡れた高さ・圧力・押す力を出し、
    // 最後の window 刻みで面ごとに「動いた質量の差し引き(net)と合計(gross)・向きが変わった数」を数えて、一方向の流れか往復かを分ける
    struct DiagnoseFace {
        int64_t net = 0;
        uint64_t gross = 0;
        uint32_t flips = 0;
        int sign = 0;
    };

    std::vector<liquid::LqCellView> ReadViews(const BakedReactionTable& table, const ProbeLiquidField& field,
                                              std::span<const reaction::RxCell> cells) {
        std::vector<liquid::LqCellView> views(field.RegionCellCount());
        for (uint32_t region = 0; region < views.size(); ++region) {
            const reaction::RxCell& cell = cells[field.GridIndex(region)];
            views[region] = liquid::LqMakeCellView(
                ProbeLiquidField::LiquidMass(table, cell) / 1000000, ProbeLiquidField::LiquidVolume(table, cell),
                ShareCellVolume(table, cell).solid, PROBE_LIQUID_CELL_VOLUME_PICOLITERS, 0, 0);
        }

        return views;
    }

    // 戻り値: 最後の刻みまでに全部静止した(面の運動量・流れの端数がどれも 0 で、セルが変わらない刻みが最後まで続く。T-0778)
    bool Diagnose(const BakedReactionTable& table, const Scene& scene, uint32_t depth, bool stone, const char* name,
                  uint64_t ticks, uint64_t window) {
        auto field = ProbeLiquidField::Create(table, {.minCell = scene.minCell, .sizeCells = scene.size});
        if (!field)
            return false;

        const ProbeLiquidOptions& options = field->Options();
        const liquid::LqParams params = liquid::LqMakeParams(
            table.environment.gravityMicrometersPerSecond2, PROBE_LIQUID_CELL_EDGE_MILLIMETERS, options.frictionShift,
            options.overfillStiffness, options.maxSpeedMillimetersPerSecond, options.advection ? 1u : 0u);
        const uint64_t fluxDenominator = liquid::LqFluxDenominator(params);
        const auto [sx, sy, sz] = scene.size;
        const auto region = [&](uint32_t x, uint32_t y, uint32_t z) {
            return x + (sx * (z + (sz * y)));
        };
        const std::array<uint32_t, 3> stride = {1, sx * sz, sx};
        const auto flux = [&](int64_t face) {
            const auto mass = (int64_t)liquid::LqMulDiv(fx::FxAbsU64(face), params.gravityMicrometersPerSecond2,
                                                        fluxDenominator);
            return face < 0 ? -mass : mass;
        };

        std::vector<reaction::RxCell> cells = MakePond(table, scene, depth, stone);
        const uint32_t count = field->RegionCellCount();
        uint64_t lastActive = 0;  // 最後に動いていた刻み(1 から)
        std::array<std::vector<DiagnoseFace>, 3> stats;
        for (auto& axisStats : stats)
            axisStats.assign(count, {});

        std::printf(
            "== %s(%u × %u × %u・深さ %u)\n  刻み  最大 x y z  動く面(層 y ごと。横/縦)  動いた mg  膜(y = "
            "%u)の最小/最大 mg\n",
            name, sx, sy, sz, depth, depth + 1);
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            std::vector<uint32_t> changed;
            const ProbeLiquidStepReport report = field->Step(table, cells, tick, changed);
            bool active = report.changedCells != 0;
            for (uint32_t axis = 0; axis < 3 && !active; ++axis) {
                for (uint32_t r = 0; r < count && !active; ++r)
                    active = field->FaceMomentum(axis)[r] != 0 || field->FaceCarry(axis, r) != 0;
            }

            lastActive = active ? tick + 1 : lastActive;
            if (tick + window >= ticks) {
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    for (uint32_t r = 0; r < count; ++r) {
                        const int64_t moved = flux(field->FaceMomentum(axis)[r]);
                        DiagnoseFace& stat = stats[axis][r];
                        stat.net += moved;
                        stat.gross += fx::FxAbsU64(moved);
                        const int sign = moved > 0 ? 1 : (moved < 0 ? -1 : 0);
                        stat.flips += sign != 0 && stat.sign != 0 && sign != stat.sign ? 1 : 0;
                        stat.sign = sign != 0 ? sign : stat.sign;
                    }
                }
            }

            if ((tick + 1) % 1800 != 0)
                continue;

            std::array<uint64_t, 3> peak = {};
            std::vector<std::array<uint32_t, 2>> moving(sy, {0, 0});
            for (uint32_t axis = 0; axis < 3; ++axis) {
                for (uint32_t r = 0; r < count; ++r) {
                    const int64_t face = field->FaceMomentum(axis)[r];
                    peak[axis] = std::max(peak[axis], fx::FxAbsU64(face));
                    if (face != 0)
                        ++moving[r / (sx * sz)][axis == 1 ? 1 : 0];
                }
            }

            const std::vector<liquid::LqCellView> views = ReadViews(table, *field, cells);
            uint64_t filmMin = UINT64_MAX;
            uint64_t filmMax = 0;
            for (uint32_t z = 0; z < sz; ++z) {
                for (uint32_t x = 0; x < sx; ++x) {
                    const uint64_t mass = views[region(x, depth + 1, z)].mass;
                    filmMin = std::min(filmMin, mass);
                    filmMax = std::max(filmMax, mass);
                }
            }

            std::printf("  %5llu  %llu %llu %llu  ", (unsigned long long)tick + 1, (unsigned long long)peak[0],
                        (unsigned long long)peak[1], (unsigned long long)peak[2]);
            for (uint32_t y = 0; y < sy; ++y)
                std::printf("%u/%u ", moving[y][0], moving[y][1]);

            std::printf(" %llu  %llu/%llu\n", (unsigned long long)report.movedMilligrams, (unsigned long long)filmMin,
                        (unsigned long long)filmMax);
        }

        // --- 最後の状態の面(運動量の大きい順)---
        const std::vector<liquid::LqCellView> views = ReadViews(table, *field, cells);
        std::vector<int64_t> pressure(count, 0);
        for (uint32_t z = 0; z < sz; ++z) {
            for (uint32_t x = 0; x < sx; ++x) {
                uint64_t above = 0;
                for (uint32_t y = sy; y-- > 0;) {
                    const uint32_t r = region(x, y, z);
                    const liquid::LqCellView& below = y == 0 ? views[r] : views[region(x, y - 1, z)];
                    const liquid::LqColumnStep step = liquid::LqColumnPressure(views[r], above, y == 0 ? 0u : 1u, below,
                                                                               params);
                    pressure[r] = step.pressure;
                    above = step.above;
                }
            }
        }

        struct Entry {
            uint64_t magnitude;
            uint32_t axis;
            uint32_t region;
        };
        std::vector<Entry> entries;
        uint32_t steady = 0;
        uint32_t back = 0;
        uint64_t steadyGross = 0;
        uint64_t backGross = 0;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            for (uint32_t r = 0; r < count; ++r) {
                const DiagnoseFace& stat = stats[axis][r];
                if (stat.gross != 0) {
                    const bool oneWay = fx::FxAbsU64(stat.net) * 2 > stat.gross;
                    (oneWay ? steady : back) += 1;
                    (oneWay ? steadyGross : backGross) += stat.gross;
                }

                if (field->FaceMomentum(axis)[r] != 0)
                    entries.push_back({fx::FxAbsU64(field->FaceMomentum(axis)[r]), axis, r});
            }
        }

        std::printf("  最後の %llu 刻み: 一方向の面 %u(動いた合計 %llu mg)・往復の面 %u(%llu mg)\n",
                    (unsigned long long)window, steady, (unsigned long long)steadyGross, back,
                    (unsigned long long)backGross);
        std::ranges::sort(entries, std::greater{}, &Entry::magnitude);
        std::printf(
            "  面(x y z 軸)  運動量  動く mg/刻み  net/gross(%llu 刻み)  向きの変わり  質量 a/b  濡れ a/b(Q16)  圧力 "
            "a/b  押す力\n",
            (unsigned long long)window);
        for (size_t i = 0; i < std::min<size_t>(entries.size(), 16); ++i) {
            const Entry& entry = entries[i];
            const uint32_t a = entry.region;
            const uint32_t b = a + stride[entry.axis];
            const uint32_t x = a % sx;
            const uint32_t z = (a / sx) % sz;
            const uint32_t y = a / (sx * sz);
            const int64_t face = field->FaceMomentum(entry.axis)[a];
            const DiagnoseFace& stat = stats[entry.axis][a];
            const int64_t push = liquid::LqNetPush(entry.axis == 1 ? 1u : 0u, views[a], pressure[a], views[b],
                                                   pressure[b]);
            std::printf("  (%u %u %u %c)  %lld  %lld  %lld/%llu  %u  %llu/%llu  %llu/%llu  %lld/%lld  %lld\n", x, y, z,
                        "xyz"[entry.axis], (long long)face, (long long)flux(face), (long long)stat.net,
                        (unsigned long long)stat.gross, stat.flips, (unsigned long long)views[a].mass,
                        (unsigned long long)views[b].mass, (unsigned long long)liquid::LqWetHeight(views[a]),
                        (unsigned long long)liquid::LqWetHeight(views[b]), (long long)pressure[a],
                        (long long)pressure[b], (long long)push);
        }

        // 運動量の大きい 2 つの面を、さらに 240 刻み追う(値が変わった刻みだけ。運動量・端数・両側の質量・押す力)
        for (size_t i = 0; i < std::min<size_t>(entries.size(), 2); ++i) {
            const uint32_t axis = entries[i].axis;
            const uint32_t a = entries[i].region;
            const uint32_t b = a + stride[axis];
            std::printf("  面 (%u %u %u %c) を追う: 刻み  運動量  端数  質量 a/b  押す力\n", a % sx, a / (sx * sz),
                        (a / sx) % sz, "xyz"[axis]);
            std::vector<reaction::RxCell> traced = cells;
            ProbeLiquidField copy = *field;
            int64_t lastFace = INT64_MIN;
            uint64_t lastA = 0;
            uint64_t lastB = 0;
            for (uint32_t tick = 0; tick < 240; ++tick) {
                std::vector<uint32_t> changed;
                (void)copy.Step(table, traced, ticks + tick, changed);
                const std::vector<liquid::LqCellView> now = ReadViews(table, copy, traced);
                const int64_t face = copy.FaceMomentum(axis)[a];
                if (face == lastFace && now[a].mass == lastA && now[b].mass == lastB)
                    continue;

                lastFace = face;
                lastA = now[a].mass;
                lastB = now[b].mass;
                std::vector<int64_t> p(count, 0);
                for (uint32_t z = 0; z < sz; ++z) {
                    for (uint32_t x = 0; x < sx; ++x) {
                        uint64_t above = 0;
                        for (uint32_t y = sy; y-- > 0;) {
                            const uint32_t r = region(x, y, z);
                            const liquid::LqCellView& below = y == 0 ? now[r] : now[region(x, y - 1, z)];
                            const liquid::LqColumnStep step = liquid::LqColumnPressure(now[r], above, y == 0 ? 0u : 1u,
                                                                                       below, params);
                            p[r] = step.pressure;
                            above = step.above;
                        }
                    }
                }

                std::printf("    %u  %lld  %lld  %llu/%llu  %lld\n", tick + 1, (long long)face,
                            (long long)copy.FaceCarry(axis, a), (unsigned long long)now[a].mass,
                            (unsigned long long)now[b].mass,
                            (long long)liquid::LqNetPush(axis == 1 ? 1u : 0u, now[a], p[a], now[b], p[b]));
            }
        }

        const bool rested = lastActive < ticks;
        if (rested)
            std::printf("  全部静止: %.2f 秒(刻み %llu から)\n", (double)lastActive / TICKS_PER_SECOND,
                        (unsigned long long)lastActive + 1);
        else
            std::printf("  %llu 秒で静止しない\n", (unsigned long long)(ticks / TICKS_PER_SECOND));

        return rested;
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
        (void)field->Step(table, cells, 0, changed);
        const Activity activity = MeasureLiquid(scene, *field, before, cells, faces, 0);
        EXPECT(activity.activeCells == 0);
    }

}  // namespace

// ============================================================================================
int main(int argc, char** argv) {
    const bool longRun = argc > 1 && std::string_view(argv[1]) == "--long";
    if (argc > 1 && std::string_view(argv[1]) == "--diagnose") {
        const BakedReactionTable table = MakeLiquidTable();
        const uint64_t seconds = argc > 2 ? std::stoull(argv[2]) : 60;
        // T-0778 の確かめ: 3 つの場面が全部静止して眠る(静止の丸めなし)
        bool rested = Diagnose(table, Scene{{16, 0, 16}, {16, 6, 16}, 0, 0}, 3, true, "積んだ水(3 層)に石",
                               seconds * 60, 300);
        rested = Diagnose(table, Scene{{16, 0, 16}, {32, 4, 32}, 0, 0}, 1, true, "池に石", seconds * 60, 300) && rested;
        rested = Diagnose(table, Scene{{16, 0, 16}, {16, 6, 16}, 0, 0}, 3, false, "積んだ水(3 層)", seconds * 60,
                          300) &&
                 rested;
        std::printf(rested ? "3 つの場面が全部静止\n" : "静止しない場面がある\n");
        return rested ? 0 : 1;
    }

    if (argc > 1 && std::string_view(argv[1]) == "--trace") {
        const BakedReactionTable table = MakeLiquidTable();
        TraceColumn(table, 0, 3600);
        TraceColumn(table, 1000, 3600);
        return 0;
    }

    const BakedReactionTable gasTable = MakeGasTable();
    BakedReactionTable viscousTable = MakeGasTable();
    viscousTable.environment.gasEddyViscosityMm2PerS = 200000;  // 0.2 m²/s(6 m の部屋の揺れが約 18 秒で e 分の 1)
    BakedReactionTable stillTable = MakeGasTable();
    stillTable.environment.gasEddyViscosityMm2PerS = 1000000;  // 1 m²/s(同じく約 3.6 秒)
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
        [&] { return RunPond(liquidTable, deepScene, 3, true, 0, "積んだ水(3 層)に石・丸めなし"); },
        // 気体の粘り(T-0758・D-516): 格子で解けない乱れの粘り 0.2 m²/s(世界の値)で乱れが静まり、静止の丸めで眠るか
        [&] { return RunGasOnly(viscousTable, gasScene, 0, "気体だけ・粘り 0.2 m²/s・丸めなし"); },
        [&] { return RunGasOnly(stillTable, gasScene, 1024, "気体だけ・粘り 1 m²/s・静止の丸め 1024 単位"); },
        [&] {
            return RunGasReference(viscousTable, gasScene, 1024,
                                   "気体 + 伝導 + 反応・粘り 0.2 m²/s・静止の丸め 1024 単位");
        },
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

    // 気体の粘り(T-0758): 揺れが静まる(最後の最大の速さが粘りなしより小さい。長い場面は 4 分の 1 未満)。
    // 60 秒で眠りきらない(熱の残りの自然の対流が数 cm/s 残る。T-0758 の作業ログ)ので、眠る数は出すだけ
    const auto finalSpeed = [](const RunResult& result) {
        return result.series.back().maxSpeed;
    };
    EXPECT(finalSpeed(results[11]) * (longRun ? 4 : 1) < finalSpeed(results[2]));
    EXPECT(finalSpeed(results[12]) * (longRun ? 4 : 1) < finalSpeed(results[2]));

    // 浅い池(1 層)に石: 揺れは育たずに静まる(T-0757。前は面の運動量が 60 秒育ち続けた)
    EXPECT(PeakMomentum(results[5], results[5].series.size() - 60) * 4 < PeakMomentum(results[5], 0));

    // 水に沈んだ石の真下の柱: 回り続けずに静まる(T-0759。前は面の運動量 9.5 × 10^8 が速さの上限に当たったまま一定)
    EXPECT(PeakMomentum(results[10], results[10].series.size() - 60) * 2 < PeakMomentum(results[10], 0));

    // 積んだ水は静止の丸めを書かなくても整数の釣り合いで眠る(T-0781・ADR-0341。長い場面: 丸めなし 47.9 秒・1 mm/s 19.9 秒)
    if (longRun) {
        EXPECT(results[8].series.back().activeCells == 0);
        EXPECT(results[9].series.back().activeCells == 0);
    }

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("すべて通った\n");
    return 0;
}
