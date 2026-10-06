// multires_subcycle_test.cpp — 細かいレベルの熱の刻み(T-0108。multires_conduction.hlsli の「細かいレベルの刻み」・
// sim::MultiresStepOptions の subcycleBaseLevel / maxSubcycleGap)の CPU のテスト。確かめること:
//   - 物差し: 根 1 つ(レベル k、外は断熱)に x の向きの余弦の温度の山を置き、山の減り方を解析解(連続の熱方程式)と比べて、
//     レベルごとの「実効の拡散率 ÷ 本当の値」を出す。分けない時は頭打ちのレベルから 1 段ごとに約 1/4、分けると Δk ≤ 3 まで 1 に近い
//   - 鎖の場面(レベル 0〜6。tests/multires_conduction_scene.h)を分けて刻んでも、「世界 + 帳簿」の保存量が最初とビット一致・
//     活性だけ刻んでも全部刻んだ時とビット一致・温度が最初の範囲の外に出ない(粗い側の上限を粗い刻み 1 回分の合計に掛けた)
//   - たくさんの要求の場面(深さ 26 段・木箱が燃える・影)でも同じ・2 回の実行で一致
// 本当の拡散率: 面の係数(2^-32 mJ/mK)の本当の値 = G の係数 × 4^k(頭打ちなし)、セルの熱容量 = 8 × C の上限(heat_conduction.hlsli)。
// 余弦の山(波数 q、一辺 h、8 セル)の連続の解は 1 刻みに exp(−(係数 ÷ 熱容量) × (qh)²) 倍(qh = π/8)。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <string_view>
#include <vector>

#include "common/multires_conduction.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "multires_activity_scene.h"
#include "multires_conduction_scene.h"
#include "sim/multires_nest.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr uint32_t MAX_GAP = MULTIRES_MAX_SUBCYCLE_GAP;  // 小刻みは最大 4^3 = 64 回(17 §4)
    constexpr uint64_t TOTALS_EVERY = 4;                     // 保存量の合計(256bit。debug で重い)を確かめる刻みの間隔

    // 試験の表の小刻みの設定(基準は空気と木箱の小さい方。tests/multires_conduction_scene.h)
    MultiresStepOptions SubcycleOptions(const BakedReactionTable& table, uint32_t maxGap) {
        return test::SubcycleTestOptions(table, maxGap);
    }

    // --- 物差し: 余弦の温度の山の減り方 ---

    constexpr int32_t STICK_MEAN = 400000;      // mK
    constexpr int32_t STICK_AMPLITUDE = 40000;  // mK
    constexpr int32_t STICK_HIGHEST_LEVEL = 7;
    constexpr double STICK_DECAY = 0.5;  // 解析解で山が exp(−0.5) になるまで刻む
    constexpr uint64_t STICK_MAX_TICKS = 3000;
    constexpr double STICK_TOLERANCE = 0.05;  // 分けた後の目標: |実効 ÷ 本当 − 1| ≤ 5%(空間の離散化の差 −1.3% を含む)
    const double STICK_WAVE = std::numbers::pi / static_cast<double>(MR_BLOCK_EDGE);  // qh

    // x の列の平均の温度の、余弦の成分の振幅(mK)
    double CosineAmplitude(const MultiresNest& nest, const ReactionTableView& view) {
        double sum = 0.0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const int32_t temperature = RxComputeThermal(view, LoadNestCell(nest, 0, index)).temperature;
            const double weight = std::cos(STICK_WAVE * (static_cast<double>(MrCellX(index)) + 0.5));
            sum += static_cast<double>(temperature - STICK_MEAN) * weight;
        }

        return sum * 2.0 / static_cast<double>(MR_BLOCK_CELLS);
    }

    struct StickResult {
        double ratio = 0.0;  // 実効の拡散率 ÷ 本当の値
        uint64_t ticks = 0;
        uint32_t drifts = 0;  // 保存量が最初と違った回数
    };

    StickResult MeasureLevel(const BakedReactionTable& table, int32_t level, const MultiresStepOptions& options) {
        MultiresCapacity capacity = test::MakeMultiresCapacity(table, 1, 0, 0);
        capacity.rootLevel = level;
        MultiresNest nest = MakeMultiresNest(capacity);
        std::vector<RxCell> cells(MR_BLOCK_CELLS);
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const double wave = std::cos(STICK_WAVE * (static_cast<double>(MrCellX(index)) + 0.5));
            const auto temperature = static_cast<int32_t>(std::lround(STICK_MEAN + (STICK_AMPLITUDE * wave)));
            cells[index] = test::MakeConductionAir(table, temperature);
        }

        PlaceRootBlock(nest, 0, 0, 0, cells);

        // --- 解析解の 1 刻みの減り方 ---
        const ReactionTableView view = table.View();
        const MrThermal mean = MrCellThermal(view, test::MakeConductionAir(table, STICK_MEAN));
        const double coefficient = static_cast<double>(mean.conductance) *
                                   static_cast<double>(HC_LIMIT_PER_CONDUCTANCE) * std::pow(4.0, level);
        const double heatCapacity = 8.0 * static_cast<double>(mean.capacityLimit);
        const double rate = coefficient / heatCapacity * STICK_WAVE * STICK_WAVE;

        StickResult result;
        result.ticks = std::clamp<uint64_t>(static_cast<uint64_t>(std::ceil(STICK_DECAY / rate)), 2, STICK_MAX_TICKS);
        const ConservedTotals initial = ComputeConservedTotals(nest, table, level);
        const double start = CosineAmplitude(nest, view);
        for (uint64_t tick = 0; tick < result.ticks; ++tick) {
            StepNest(nest, table, test::CONDUCTION_SEED, tick, options);
            if (tick % (TOTALS_EVERY * 4) == 0 || tick + 1 == result.ticks)
                result.drifts += ComputeConservedTotals(nest, table, level) == initial ? 0 : 1;
        }

        const double end = CosineAmplitude(nest, view);
        result.ratio = std::log(start / end) / (static_cast<double>(result.ticks) * rate);

        return result;
    }

    // 現実の伝導率(試験の表の 1/5000〜1/20000。multires_conduction_test の計測)なら、頭打ちにならない最後のレベルと、
    // Δk ≤ 3 で本当の速さになるレベルの上限(レベル k の一辺 = 0.5 m × 2^-k)
    void LogRealBaseLevel(const BakedReactionTable& table) {
        constexpr uint32_t AIR_SCALE = 5000;
        constexpr uint32_t CRATE_SCALE = 20000;
        const ReactionTableView view = table.View();
        MrThermal air = MrCellThermal(view, test::MakeConductionAir(table, 300000));
        MrThermal crate = MrCellThermal(view, test::MakeMultiresRootCells(table)[MrCellIndex(3, 4, 5)]);
        air.conductance /= AIR_SCALE;
        crate.conductance /= CRATE_SCALE;
        const int32_t airBase = MrSubcycleBaseLevel(air);
        const int32_t crateBase = MrSubcycleBaseLevel(crate);
        const auto gap = static_cast<int32_t>(MAX_GAP);
        Log(Channel::Sim, Level::Info,
            "物差し: 現実の伝導率なら頭打ちにならない最後のレベルは空気 {}・木箱 {}(Δk ≤ {} で本当の速さはレベル "
            "{}・{} まで)",
            airBase, crateBase, MAX_GAP, airBase + gap, crateBase + gap);
    }

    void MeasureStick(const BakedReactionTable& table) {
        const MultiresStepOptions whole = {.conduction = true};
        const MultiresStepOptions split = SubcycleOptions(table, MAX_GAP);
        const int32_t base = split.subcycleBaseLevel;
        Log(Channel::Sim, Level::Info, "物差し: 頭打ちにならない最後のレベル {}・小刻みは最大 4^{} 回", base, MAX_GAP);
        LogRealBaseLevel(table);
        for (int32_t level = 0; level <= STICK_HIGHEST_LEVEL; ++level) {
            const StickResult before = MeasureLevel(table, level, whole);
            const StickResult after = MeasureLevel(table, level, split);
            const uint32_t shift = MrSubcycleShift(level, base, MAX_GAP);
            Log(Channel::Sim, Level::Info,
                "物差し: レベル {} の実効の拡散率 ÷ 本当の値 = 分けない {:.4f}({} 刻み)・分ける {:.4f}(小刻み 4^{} "
                "回・{} 刻み)",
                level, before.ratio, before.ticks, after.ratio, shift, after.ticks);
            Expect(before.drifts == 0 && after.drifts == 0,
                   std::format("物差し: レベル {} で保存量が最初とビット一致", level));
            if (level <= base + static_cast<int32_t>(MAX_GAP))
                Expect(std::abs(after.ratio - 1.0) <= STICK_TOLERANCE,
                       std::format("物差し: レベル {} で分けた後の実効の拡散率が本当の値の ±{}% の中", level,
                                   STICK_TOLERANCE * 100.0));

            if (level > base + 1)
                Expect(before.ratio < 0.5,
                       std::format("物差し: レベル {} は分けないと頭打ちで遅い(今までの制限の確認)", level));
        }
    }

    // --- 鎖の場面を分けて刻む ---

    void CheckChain(const BakedReactionTable& table) {
        constexpr uint64_t CHAIN_TICKS = 60;
        const MultiresStepOptions split = SubcycleOptions(table, MAX_GAP);
        const MultiresStepOptions whole = {.conduction = true};
        MultiresNest active = test::MakeChainNest(table, 16);
        MultiresNest full = test::MakeChainNest(table, 16);
        MultiresNest unsplit = test::MakeChainNest(table, 16);
        const ReactionTableView view = table.View();
        const test::TemperatureRange start = test::RealTemperatures(active, view);
        const ConservedTotals initial = ComputeConservedTotals(active, table, test::CHAIN_LEVELS);
        uint32_t drifts = 0;
        uint32_t mismatches = 0;
        uint32_t overshoots = 0;
        for (uint64_t tick = 0; tick < CHAIN_TICKS; ++tick) {
            StepActive(active, table, test::CONDUCTION_SEED, tick, split);
            StepNest(full, table, test::CONDUCTION_SEED, tick, split);
            StepActive(unsplit, table, test::CONDUCTION_SEED, tick, whole);
            mismatches += test::SameWorld(active, full) ? 0 : 1;
            if (tick % TOTALS_EVERY == 0 || tick + 1 == CHAIN_TICKS)
                drifts += ComputeConservedTotals(active, table, test::CHAIN_LEVELS) == initial ? 0 : 1;

            const test::TemperatureRange range = test::RealTemperatures(active, view);
            overshoots += range.low < start.low || range.high > start.high ? 1 : 0;
        }

        Expect(drifts == 0, "鎖(分ける): 「世界 + 帳簿」の保存量が最初とビット一致(4 刻みごと)");
        Expect(mismatches == 0, "鎖(分ける): 活性だけ刻んでも全部刻んだ時とビット一致");
        Expect(overshoots == 0, "鎖(分ける): 温度が最初の範囲の外に出ない");
        Expect(active.counters[MR_COUNTER_PAGE_SHORTAGE] == 0, "鎖(分ける): 頁は足りている");

        const test::TemperatureRange splitRange = test::RealTemperatures(active, view);
        const test::TemperatureRange unsplitRange = test::RealTemperatures(unsplit, view);
        Log(Channel::Sim, Level::Info,
            "鎖: {} 刻みで世界の最高 {} mK(分けない {} mK)・端数のブロック {}・刻む印を付けた数 {}(分けない {})",
            CHAIN_TICKS, splitRange.high, unsplitRange.high, test::CountFractionBlocks(active),
            active.counters[MR_COUNTER_SCHEDULED], unsplit.counters[MR_COUNTER_SCHEDULED]);

        // --- 費用の目安: 1 刻みに流れを計算するブロック × 小刻みの数(細かい部分の大きさに比例する)---
        uint64_t substepBlocks = 0;
        uint32_t realBlocks = 0;
        for (uint32_t slot = 0; slot < active.capacity.worldBlocks; ++slot) {
            const MrBlock& block = active.blocks[slot];
            if (block.kind != MR_BLOCK_REAL)
                continue;

            realBlocks += 1;
            substepBlocks += uint64_t{1} << (2 * MrSubcycleShift(block.level, split.subcycleBaseLevel, MAX_GAP));
        }

        Log(Channel::Sim, Level::Info, "鎖: 本物のブロック {} ・1 刻みのブロック × 小刻み {}", realBlocks,
            substepBlocks);
    }

    // --- たくさんの要求の場面を分けて刻む ---

    uint64_t RunStress(const BakedReactionTable& table, uint64_t ticks, bool check) {
        const MultiresStepOptions split = SubcycleOptions(table, MAX_GAP);
        MultiresNest active = test::MakeActivityNest(table);
        MultiresNest full = test::MakeActivityNest(table);
        const ConservedTotals initial = ComputeConservedTotals(active, table, test::STRESS_MAX_LEVEL);
        uint32_t drifts = 0;
        uint32_t mismatches = 0;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const std::vector<MrRequest> requests = test::MakeStressRequests(active, tick);
            test::BeginActivityTick(active, tick, requests);
            test::EndActivityTick(active, table, tick, true, split);
            if (!check)
                continue;

            test::BeginActivityTick(full, tick, requests);
            test::EndActivityTick(full, table, tick, false, split);
            mismatches += test::SameWorld(active, full) ? 0 : 1;
            if (tick % TOTALS_EVERY == 0 || tick + 1 == ticks)
                drifts += ComputeConservedTotals(active, table, test::STRESS_MAX_LEVEL) == initial ? 0 : 1;

            if (mismatches == 1 && !test::SameWorld(active, full))
                Log(Channel::Sim, Level::Error, "刻み {}: 活性だけ刻んだ時と全部刻んだ時が違う", tick);
        }

        if (check) {
            Expect(drifts == 0, "たくさんの要求(分ける): 「世界 + 帳簿」の保存量が最初とビット一致(4 刻みごと)");
            Expect(mismatches == 0, "たくさんの要求(分ける): 活性だけ刻んでも全部刻んだ時とビット一致");
            Log(Channel::Sim, Level::Info,
                "たくさんの要求(分ける): {} 刻み・端数のブロック {}・端数の不足 {}・頁の不足 {}", ticks,
                test::CountFractionBlocks(active), active.counters[MR_COUNTER_FRACTION_SHORTAGE],
                active.counters[MR_COUNTER_PAGE_SHORTAGE]);
        }

        return HashWholeNest(active);
    }

    void CheckStress(const BakedReactionTable& table) {
        constexpr uint64_t STRESS_TICKS = 8;
        const uint64_t first = RunStress(table, STRESS_TICKS, true);
        const uint64_t second = RunStress(table, STRESS_TICKS, false);
        Expect(first == second, "たくさんの要求(分ける): 2 回の実行で全部が一致");
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "multires_subcycle_test: FAILED(表を作れない)");
            return 1;
        }

        MeasureStick(*table);
        CheckChain(*table);
        CheckStress(*table);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_subcycle_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_subcycle_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
