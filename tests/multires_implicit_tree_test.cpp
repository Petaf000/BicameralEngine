// multires_implicit_tree_test.cpp — 細かいレベルの熱の陰解法(方式②)を木の伝導につないだ形(T-0119。D-434 の案 a。
// sim::MultiresStepOptions の implicitConduction。engine/src/sim/multires_implicit_conduction.cpp)の CPU のテスト。確かめること:
//   - 物差し(余弦の山。x の向きに根を並べ、外は断熱): 山の長さは基準のレベルの 8 セルと同じ実際の長さ(レベル k なら 8 × 2^Δk セル。
//     T-0110 の物差しと同じ)。基準より細かいレベル(Δk 1〜5)も実効の拡散率が本当の値の ±5%・保存量が最初とビット一致。
//     1 ブロック(8 セル)の短い山も記録だけする(Δk ≥ 3 では 1 刻みより速く減る山で、後退 Euler は 1 刻みの減りが 1/(1 + λΔt) と遅い)
//   - 熱い点(レベル 6 の根の 1 セルだけ 1500 K): 保存量がビット一致・温度が最初の範囲の外に出ない・V サイクルの回数(費用の記録)
//   - 鎖の場面(レベル 0〜6・いちばん細かいブロックが 1500 K): 「世界 + 帳簿」の保存量が最初とビット一致・活性だけ刻んでも
//     全部刻んだ時とビット一致・温度が最初の範囲の外に出ない・刻みごとの V サイクルの回数と系のセルの数(T-0117 の見積もりと比べる)
//   - たくさんの要求の場面(深さ 26 段・木箱が燃える・相変化と反応で熱容量が変わる)でも同じ・2 回の実行で一致
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <string>
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

    constexpr uint64_t TOTALS_EVERY = 4;  // 保存量の合計(256bit。debug で重い)を確かめる刻みの間隔

    // 案 a: 方式①は分けない + 基準より細かい所は全部②(D-434)
    MultiresStepOptions ImplicitOptions(const BakedReactionTable& table) {
        MultiresStepOptions options = test::SubcycleTestOptions(table, 0);
        options.implicitConduction = true;

        return options;
    }

    // 刻みごとの陰解法の費用をまとめる
    struct CostSummary {
        uint32_t firstCycles = 0;
        uint32_t maxCycles = 0;
        uint64_t cycleSum = 0;
        uint64_t passSum = 0;
        uint64_t cellUpdateSum = 0;
        uint32_t maxCells = 0;
        uint32_t limitedCells = 0;
        uint64_t ticks = 0;

        void Add(const MultiresNest& nest) {
            const ImplicitCost& cost = nest.implicitCost;
            if (ticks == 0)
                firstCycles = cost.cycles;

            ticks += 1;
            maxCycles = std::max(maxCycles, cost.cycles);
            cycleSum += cost.cycles;
            passSum += cost.passes;
            cellUpdateSum += cost.cellUpdates;
            maxCells = std::max(maxCells, nest.implicitCells);
            limitedCells += cost.limitedCells;
        }

        [[nodiscard]] std::string Text() const {
            const uint64_t count = std::max<uint64_t>(ticks, 1);

            return std::format(
                "V サイクル 初め {}・最大 {}・平均 {:.1f} / 直列の段 平均 {} / 節の計算 平均 {} / 系のセル 最大 {} / "
                "安全網で戻したセル 計 {}",
                firstCycles, maxCycles, static_cast<double>(cycleSum) / static_cast<double>(count), passSum / count,
                cellUpdateSum / count, maxCells, limitedCells);
        }
    };

    std::string LevelsText(const MultiresNest& nest) {
        std::string text;
        for (const auto& [nodes, widest] : nest.implicitLevels)
            text += std::format("[{}, {}] ", nodes, widest);

        return text;
    }

    // --- 物差し: 余弦の温度の山の減り方(multires_subcycle_test と同じ)---

    constexpr int32_t STICK_MEAN = 400000;      // mK
    constexpr int32_t STICK_AMPLITUDE = 40000;  // mK
    constexpr int32_t STICK_HIGHEST_LEVEL = 7;
    constexpr int32_t STICK_LONGEST_GAP = 5;  // 長い山を測る Δk の上限(根 32 個)
    constexpr double STICK_DECAY = 0.5;
    constexpr uint64_t STICK_MAX_TICKS = 3000;
    constexpr double STICK_TOLERANCE = 0.05;
    const double STICK_WAVE = std::numbers::pi / static_cast<double>(MR_BLOCK_EDGE);  // qh

    // x の向きの余弦の成分の振幅(mK)。山の長さ = 根の数 × 8 セル(根は枠 0 から x の順)
    double CosineAmplitude(const MultiresNest& nest, const ReactionTableView& view, uint32_t roots) {
        const double wave = STICK_WAVE / static_cast<double>(roots);
        double sum = 0.0;
        for (uint32_t root = 0; root < roots; ++root) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const int32_t temperature = RxComputeThermal(view, LoadNestCell(nest, root, index)).temperature;
                const double x = static_cast<double>((root * MR_BLOCK_EDGE) + MrCellX(index)) + 0.5;
                sum += static_cast<double>(temperature - STICK_MEAN) * std::cos(wave * x);
            }
        }

        return sum * 2.0 / static_cast<double>(roots * MR_BLOCK_CELLS);
    }

    // レベル level の根を x の向きに roots 個並べる(セルは cellAt(根, セルの番号))
    template <typename CellAt>
    MultiresNest MakeRowNest(const BakedReactionTable& table, int32_t level, uint32_t roots, CellAt cellAt) {
        MultiresCapacity capacity = test::MakeMultiresCapacity(table, roots, 0, 0);
        capacity.rootLevel = level;
        MultiresNest nest = MakeMultiresNest(capacity);
        std::vector<RxCell> cells(MR_BLOCK_CELLS);
        for (uint32_t root = 0; root < roots; ++root) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                cells[index] = cellAt(root, index);

            PlaceRootBlock(nest, int64_t{root} * MR_BLOCK_EDGE, 0, 0, cells);
        }

        return nest;
    }

    struct StickResult {
        double ratio = 0.0;
        uint64_t ticks = 0;
        uint32_t drifts = 0;
        CostSummary cost;
    };

    StickResult MeasureLevel(const BakedReactionTable& table, int32_t level, uint32_t roots,
                             const MultiresStepOptions& options) {
        const double waveNumber = STICK_WAVE / static_cast<double>(roots);
        MultiresNest nest = MakeRowNest(table, level, roots, [&](uint32_t root, uint32_t index) {
            const double x = static_cast<double>((root * MR_BLOCK_EDGE) + MrCellX(index)) + 0.5;
            const double wave = std::cos(waveNumber * x);
            const auto temperature = static_cast<int32_t>(std::lround(STICK_MEAN + (STICK_AMPLITUDE * wave)));

            return test::MakeConductionAir(table, temperature);
        });

        // --- 解析解の 1 刻みの減り方 ---
        const ReactionTableView view = table.View();
        const MrThermal mean = MrCellThermal(view, test::MakeConductionAir(table, STICK_MEAN));
        const double coefficient = static_cast<double>(mean.conductance) *
                                   static_cast<double>(HC_LIMIT_PER_CONDUCTANCE) * std::pow(4.0, level);
        const double heatCapacity = 8.0 * static_cast<double>(mean.capacityLimit);
        const double rate = coefficient / heatCapacity * waveNumber * waveNumber;

        StickResult result;
        result.ticks = std::clamp<uint64_t>(static_cast<uint64_t>(std::ceil(STICK_DECAY / rate)), 2, STICK_MAX_TICKS);
        const ConservedTotals initial = ComputeConservedTotals(nest, table, level);
        const double start = CosineAmplitude(nest, view, roots);
        for (uint64_t tick = 0; tick < result.ticks; ++tick) {
            StepNest(nest, table, test::CONDUCTION_SEED, tick, options);
            result.cost.Add(nest);
            if (tick % (TOTALS_EVERY * 4) == 0 || tick + 1 == result.ticks)
                result.drifts += ComputeConservedTotals(nest, table, level) == initial ? 0 : 1;
        }

        const double end = CosineAmplitude(nest, view, roots);
        result.ratio = std::log(start / end) / (static_cast<double>(result.ticks) * rate);

        return result;
    }

    void MeasureStick(const BakedReactionTable& table) {
        const MultiresStepOptions options = ImplicitOptions(table);
        const int32_t base = options.subcycleBaseLevel;
        Log(Channel::Sim, Level::Info, "物差し: 基準(頭打ちにならない最後のレベル){}。それより細かいレベルは陰解法",
            base);
        for (int32_t level = 0; level <= STICK_HIGHEST_LEVEL; ++level) {
            // --- 長い山(基準の 8 セルと同じ実際の長さ)---
            const int32_t gap = std::max(level - base, 0);
            if (gap <= STICK_LONGEST_GAP) {
                const uint32_t roots = 1u << gap;
                const StickResult result = MeasureLevel(table, level, roots, options);
                Log(Channel::Sim, Level::Info,
                    "物差し: レベル {}(Δk {}・山 {} セル)の実効の拡散率 ÷ 本当の値 = {:.4f}({} 刻み)/ {}", level, gap,
                    roots * MR_BLOCK_EDGE, result.ratio, result.ticks, result.cost.Text());
                Expect(result.drifts == 0, std::format("物差し: レベル {} で保存量が最初とビット一致", level));
                Expect(std::abs(result.ratio - 1.0) <= STICK_TOLERANCE,
                       std::format("物差し: レベル {} の実効の拡散率が本当の値の ±{}% の中", level,
                                   STICK_TOLERANCE * 100.0));
            }

            // --- 短い山(1 ブロック。記録だけ)---
            const StickResult result = MeasureLevel(table, level, 1, options);
            Log(Channel::Sim, Level::Info, "物差し: レベル {} の 8 セルの山の実効の拡散率 ÷ 本当の値 = {:.4f}({} 刻み)",
                level, result.ratio, result.ticks);
            Expect(result.drifts == 0, std::format("物差し: レベル {} の 8 セルの山で保存量が最初とビット一致", level));
        }
    }

    // --- 熱い点: レベル 6 の根の 1 セルだけ熱い(鋭い熱。V サイクルが増える場面)---

    void CheckHotPoint(const BakedReactionTable& table) {
        constexpr int32_t HOT_LEVEL = 6;
        constexpr uint64_t HOT_TICKS = 12;
        const RxCell air = test::MakeConductionAir(table, 300000);
        const RxCell hot = test::MakeConductionAir(table, 1500000);
        MultiresNest nest = MakeRowNest(table, HOT_LEVEL, 1, [&](uint32_t /*root*/, uint32_t index) {
            return index == MrCellIndex(3, 4, 5) ? hot : air;
        });
        const MultiresStepOptions options = ImplicitOptions(table);
        const ReactionTableView view = table.View();
        const test::TemperatureRange start = test::RealTemperatures(nest, view);
        const ConservedTotals initial = ComputeConservedTotals(nest, table, HOT_LEVEL);
        CostSummary cost;
        std::string cycles;
        uint32_t overshoots = 0;
        for (uint64_t tick = 0; tick < HOT_TICKS; ++tick) {
            StepNest(nest, table, test::CONDUCTION_SEED, tick, options);
            cost.Add(nest);
            cycles += std::format("{} ", nest.implicitCost.cycles);
            const test::TemperatureRange range = test::RealTemperatures(nest, view);
            overshoots += range.low < start.low || range.high > start.high ? 1 : 0;
        }

        Expect(ComputeConservedTotals(nest, table, HOT_LEVEL) == initial, "熱い点: 保存量が最初とビット一致");
        Expect(overshoots == 0, "熱い点: 温度が最初の範囲の外に出ない");
        Log(Channel::Sim, Level::Info, "熱い点(レベル 6・Δk {}): 刻みごとの V サイクル {}/ {}",
            HOT_LEVEL - options.subcycleBaseLevel, cycles, cost.Text());
    }

    // --- 鎖の場面 ---

    void CheckChain(const BakedReactionTable& table) {
        constexpr uint64_t CHAIN_TICKS = 60;
        constexpr uint64_t EARLY_TICKS = 12;
        const MultiresStepOptions options = ImplicitOptions(table);
        MultiresNest active = test::MakeChainNest(table, 16);
        MultiresNest full = test::MakeChainNest(table, 16);
        const ReactionTableView view = table.View();
        const test::TemperatureRange start = test::RealTemperatures(active, view);
        const ConservedTotals initial = ComputeConservedTotals(active, table, test::CHAIN_LEVELS);
        CostSummary early;
        CostSummary late;
        std::string cycles;
        uint32_t drifts = 0;
        uint32_t mismatches = 0;
        uint32_t overshoots = 0;
        for (uint64_t tick = 0; tick < CHAIN_TICKS; ++tick) {
            StepActive(active, table, test::CONDUCTION_SEED, tick, options);
            StepNest(full, table, test::CONDUCTION_SEED, tick, options);
            (tick < EARLY_TICKS ? early : late).Add(active);
            cycles += std::format("{} ", active.implicitCost.cycles);
            mismatches += test::SameWorld(active, full) ? 0 : 1;
            if (mismatches == 1 && !test::SameWorld(active, full))
                Log(Channel::Sim, Level::Error, "鎖: 刻み {} で活性だけ刻んだ時と全部刻んだ時が違う", tick);

            if (tick % TOTALS_EVERY == 0 || tick + 1 == CHAIN_TICKS)
                drifts += ComputeConservedTotals(active, table, test::CHAIN_LEVELS) == initial ? 0 : 1;

            const test::TemperatureRange range = test::RealTemperatures(active, view);
            overshoots += range.low < start.low || range.high > start.high ? 1 : 0;
        }

        Expect(drifts == 0, "鎖(陰解法): 「世界 + 帳簿」の保存量が最初とビット一致(4 刻みごと)");
        Expect(mismatches == 0, "鎖(陰解法): 活性だけ刻んでも全部刻んだ時とビット一致");
        Expect(overshoots == 0, "鎖(陰解法): 温度が最初の範囲の外に出ない");
        Expect(active.counters[MR_COUNTER_PAGE_SHORTAGE] == 0, "鎖(陰解法): 頁は足りている");

        const test::TemperatureRange range = test::RealTemperatures(active, view);
        Log(Channel::Sim, Level::Info, "鎖(陰解法): {} 刻みで世界の最高 {} mK・端数のブロック {}・端数の不足 {}",
            CHAIN_TICKS, range.high, test::CountFractionBlocks(active), active.counters[MR_COUNTER_FRACTION_SHORTAGE]);
        Log(Channel::Sim, Level::Info, "鎖(陰解法): 刻みごとの V サイクル {}", cycles);
        Log(Channel::Sim, Level::Info, "鎖(陰解法): 最後の刻みの多重格子の段 [節, 隣の最大] {}", LevelsText(active));
        Log(Channel::Sim, Level::Info, "鎖(陰解法): 初めの {} 刻み {}", EARLY_TICKS, early.Text());
        Log(Channel::Sim, Level::Info, "鎖(陰解法): 後の {} 刻み {}", CHAIN_TICKS - EARLY_TICKS, late.Text());
    }

    // --- たくさんの要求の場面 ---

    uint64_t RunStress(const BakedReactionTable& table, uint64_t ticks, bool check) {
        const MultiresStepOptions options = ImplicitOptions(table);
        MultiresNest active = test::MakeActivityNest(table);
        MultiresNest full = test::MakeActivityNest(table);
        const ConservedTotals initial = ComputeConservedTotals(active, table, test::STRESS_MAX_LEVEL);
        CostSummary cost;
        uint32_t drifts = 0;
        uint32_t mismatches = 0;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const std::vector<MrRequest> requests = test::MakeStressRequests(active, tick);
            test::BeginActivityTick(active, tick, requests);
            test::EndActivityTick(active, table, tick, true, options);
            cost.Add(active);
            if (!check)
                continue;

            test::BeginActivityTick(full, tick, requests);
            test::EndActivityTick(full, table, tick, false, options);
            mismatches += test::SameWorld(active, full) ? 0 : 1;
            if (tick % TOTALS_EVERY == 0 || tick + 1 == ticks)
                drifts += ComputeConservedTotals(active, table, test::STRESS_MAX_LEVEL) == initial ? 0 : 1;

            if (mismatches == 1 && !test::SameWorld(active, full))
                Log(Channel::Sim, Level::Error, "刻み {}: 活性だけ刻んだ時と全部刻んだ時が違う", tick);
        }

        if (check) {
            Expect(drifts == 0, "たくさんの要求(陰解法): 「世界 + 帳簿」の保存量が最初とビット一致(4 刻みごと)");
            Expect(mismatches == 0, "たくさんの要求(陰解法): 活性だけ刻んでも全部刻んだ時とビット一致");
            Log(Channel::Sim, Level::Info,
                "たくさんの要求(陰解法): {} 刻み・端数のブロック {}・端数の不足 {}・頁の不足 {} / {} / 最後の段 {}",
                ticks, test::CountFractionBlocks(active), active.counters[MR_COUNTER_FRACTION_SHORTAGE],
                active.counters[MR_COUNTER_PAGE_SHORTAGE], cost.Text(), LevelsText(active));
        }

        return HashWholeNest(active);
    }

    void CheckStress(const BakedReactionTable& table) {
        constexpr uint64_t STRESS_TICKS = 8;
        const uint64_t first = RunStress(table, STRESS_TICKS, true);
        const uint64_t second = RunStress(table, STRESS_TICKS, false);
        Expect(first == second, "たくさんの要求(陰解法): 2 回の実行で全部が一致");
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "multires_implicit_tree_test: FAILED(表を作れない)");
            return 1;
        }

        MeasureStick(*table);
        CheckHotPoint(*table);
        CheckChain(*table);
        CheckStress(*table);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_implicit_tree_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_implicit_tree_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
