// multires_implicit_test.cpp — 細かいレベルの熱の陰解法の試作(T-0110 研究。sim/implicit_conduction)の CPU のテスト。確かめること:
//   - 物差し(T-0108 の tests/multires_subcycle_test.cpp と同じ測り方): x の向きの余弦の温度の山の減り方を連続の熱方程式の解と比べ、
//     「実効の拡散率 ÷ 本当の値」を出す。山の長さは基準のレベルの 8 セル(T-0108 の物差しの山)に固定し、レベル 基準 + Δk では
//     8·2^Δk セルで置く(同じ長さの山なら、本当の減り方はどのレベルでも同じ)。方式ごと・Δk ごとに、費用(1 刻みに 1 セルを何回計算したか・
//     直列の段の数)も出す。目標: 選んだ方式(V サイクルを誤差の見込み 1 mK で止める。ADR-0019)で Δk ≤ 8 の全部が ±5%・
//     安全網が働かない・V サイクル 1 回の費用(回数固定の V 2 回で見る)がレベルの数に比例
//   - 短い山: 8 セルの山をレベル 基準 + 6 に置いた時の 1・2 刻み後の振幅(本当は 1 刻みでほぼ消える。後退 Euler の時間の離散化の限界を見る)
//   - 違うレベルの面(差 3 と 6)を含む場面: 熱い細かい所が冷める。保存量がビット一致・温度が最初の範囲の外に出ない(どの方式も安全網で)・
//     2 回の実行で一致。安全網を外した時の行き過ぎと速さも方式ごとに記録する(収束の具合。T-0118 の比べの元)
// どの方式も、保存量(全部のセルのエネルギー + 端数)は毎刻み最初とビット一致すること(近似が効くのは速さだけ)。
#include <algorithm>
#include <array>
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
#include "sim/implicit_conduction.h"
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

    constexpr uint32_t TEMPERATURE_SHIFT = 16;  // ImplicitTemperature は mK × 2^16
    constexpr double TEMPERATURE_SCALE = 65536.0;

    struct Material {
        uint64_t heatCapacity = 0;
        uint32_t conductance = 0;
    };

    Material AirAt(const BakedReactionTable& table, int32_t millikelvin) {
        const MrThermal thermal = MrCellThermal(table.View(), test::MakeConductionAir(table, millikelvin));

        return {8 * thermal.capacityLimit, thermal.conductance};
    }

    ImplicitCell MakeCell(const Material& material, int32_t level, int64_t x, int64_t y, int64_t z,
                          int64_t millikelvin) {
        return {.level = level,
                .x = x,
                .y = y,
                .z = z,
                .heatCapacity = material.heatCapacity,
                .conductance = material.conductance,
                .energy = ImplicitEnergyFor(material.heatCapacity, millikelvin)};
    }

    struct Method {
        std::string_view name;
        ImplicitOptions options;
        int32_t highestGap = 0;  // これより大きい Δk では測らない(debug で重い)
        bool galerkin = false;   // 多重格子の親の係数を子の合計のままに(BuildImplicitGrid)
    };

    constexpr uint32_t ADAPTIVE_TOLERANCE = 1;  // mK
    constexpr uint32_t ADAPTIVE_MAX_CYCLES = 16;

    const std::array<Method, 7> METHODS = {{
        {"赤黒 8 回", {.method = ImplicitMethod::RedBlack, .sweeps = 8}, 8},
        {"赤黒 32 回", {.method = ImplicitMethod::RedBlack, .sweeps = 32}, 8},
        {"V 1 回", {.method = ImplicitMethod::Multigrid, .cycles = 1}, 8},
        {"V 2 回", {.method = ImplicitMethod::Multigrid, .cycles = 2}, 8},
        {"V 8 回", {.method = ImplicitMethod::Multigrid, .cycles = 8}, 8},
        {"V 適応(誤差 1 mK・上限 16 回)",
         {.method = ImplicitMethod::Multigrid,
          .cycles = ADAPTIVE_MAX_CYCLES,
          .toleranceMillikelvin = ADAPTIVE_TOLERANCE},
         8},
        {"RKL2", {.method = ImplicitMethod::Rkl2}, 6},
    }};

    constexpr size_t CHOSEN_METHOD = 5;  // 合格の基準を課す方式(V 適応)
    constexpr size_t FIXED_METHOD = 3;   // 費用の比例を見る方式(V 2 回)

    // 違うレベルの場面で安全網を外して収束の具合を見る組み合わせ(記録だけ)
    const std::array<Method, 9> COMPOSITE_PROBES = {{
        {"V 2 回", {.method = ImplicitMethod::Multigrid, .cycles = 2}, 0},
        {"V 4 回", {.method = ImplicitMethod::Multigrid, .cycles = 4}, 0},
        {"V 8 回", {.method = ImplicitMethod::Multigrid, .cycles = 8}, 0},
        {"V 12 回", {.method = ImplicitMethod::Multigrid, .cycles = 12}, 0},
        {"V 4 回・直し 0.75 倍", {.method = ImplicitMethod::Multigrid, .cycles = 4, .correctionScale = 192}, 0},
        {"V 4 回・Galerkin", {.method = ImplicitMethod::Multigrid, .cycles = 4}, 0, true},
        {"V(3,3) 8 回", {.method = ImplicitMethod::Multigrid, .cycles = 8, .preSmooth = 3, .postSmooth = 3}, 0},
        {"V(3,3) 適応(誤差 1 mK・上限 16 回)",
         {.method = ImplicitMethod::Multigrid,
          .cycles = ADAPTIVE_MAX_CYCLES,
          .toleranceMillikelvin = ADAPTIVE_TOLERANCE,
          .preSmooth = 3,
          .postSmooth = 3},
         0},
        {"RKL2", {.method = ImplicitMethod::Rkl2}, 0},
    }};

    // --- 刻んで見張る(保存・行き過ぎ・費用)---

    // 行き過ぎの判定の余裕(mK): 安全網の余裕 1 mK + エネルギーと温度の切り捨ての 1 mK
    constexpr int64_t OVERSHOOT_TOLERANCE = 2;

    struct Watch {
        fx::FxU128 initial{};
        int64_t lowest = 0;  // mK
        int64_t highest = 0;
        uint32_t drifts = 0;
        uint32_t overshoots = 0;
        uint64_t limitedCells = 0;  // 安全網で陽解法に戻したセル(全部の刻みの合計)
        int64_t worstExcess = 0;    // 安全網の前に範囲を超えた最大(mK)
        uint32_t maxCycles = 0;     // 1 刻みに回した V サイクルの最大
        ImplicitCost cost;          // 1 刻みあたり(最後の刻み)
    };

    Watch StartWatch(const ImplicitGrid& grid) {
        Watch watch;
        watch.initial = ImplicitConservedTotal(grid);
        watch.lowest = INT64_MAX;
        watch.highest = INT64_MIN;
        for (const ImplicitCell& cell : grid.cells) {
            const int64_t temperature = ImplicitTemperature(cell) >> TEMPERATURE_SHIFT;
            watch.lowest = std::min(watch.lowest, temperature - OVERSHOOT_TOLERANCE);
            watch.highest = std::max(watch.highest, temperature + 1 + OVERSHOOT_TOLERANCE);
        }

        return watch;
    }

    void StepWatched(ImplicitGrid& grid, const ImplicitOptions& options, Watch& watch) {
        watch.cost = StepImplicit(grid, options);
        watch.limitedCells += watch.cost.limitedCells;
        watch.worstExcess = std::max(watch.worstExcess, watch.cost.worstExcessMillikelvin);
        watch.maxCycles = std::max(watch.maxCycles, watch.cost.cycles);
        const fx::FxU128 total = ImplicitConservedTotal(grid);
        watch.drifts += total.hi == watch.initial.hi && total.lo == watch.initial.lo ? 0 : 1;
        for (const ImplicitCell& cell : grid.cells) {
            const int64_t temperature = ImplicitTemperature(cell) >> TEMPERATURE_SHIFT;
            if (temperature < watch.lowest || temperature > watch.highest) {
                watch.overshoots += 1;
                break;
            }
        }
    }

    // --- 物差し ---

    constexpr int64_t STICK_MEAN = 400000;      // mK
    constexpr int64_t STICK_AMPLITUDE = 40000;  // mK
    constexpr uint32_t STICK_CROSS = 2;         // y・z のセルの数(3 次元の面と縮約を通す)
    constexpr double STICK_DECAY = 0.5;         // 解析解で山が exp(−0.5) になるまで刻む
    constexpr uint64_t STICK_MAX_TICKS = 3000;
    constexpr double STICK_TOLERANCE = 0.05;
    constexpr std::array<int32_t, 8> STICK_GAPS = {0, 1, 2, 3, 4, 5, 6, 8};
    constexpr int32_t SHORT_WAVE_GAP = 6;

    double WaveWeight(int64_t x, uint32_t length) {
        return std::cos(std::numbers::pi * (static_cast<double>(x) + 0.5) / static_cast<double>(length));
    }

    std::vector<ImplicitCell> MakeWaveCells(const Material& air, int32_t level, uint32_t length) {
        std::vector<ImplicitCell> cells;
        for (uint32_t z = 0; z < STICK_CROSS; ++z) {
            for (uint32_t y = 0; y < STICK_CROSS; ++y) {
                for (uint32_t x = 0; x < length; ++x) {
                    const double wave = static_cast<double>(STICK_AMPLITUDE) * WaveWeight(x, length);
                    const int64_t temperature = STICK_MEAN + std::llround(wave);
                    cells.push_back(MakeCell(air, level, x, y, z, temperature));
                }
            }
        }

        return cells;
    }

    // 余弦の成分の振幅(mK)
    double WaveAmplitude(const ImplicitGrid& grid, uint32_t length) {
        double sum = 0.0;
        for (const ImplicitCell& cell : grid.cells) {
            const double temperature = static_cast<double>(ImplicitTemperature(cell)) / TEMPERATURE_SCALE;
            sum += (temperature - static_cast<double>(STICK_MEAN)) * WaveWeight(cell.x, length);
        }

        return sum * 2.0 / static_cast<double>(grid.cells.size());
    }

    // 連続の解の 1 刻みの減り方(山の長さ length セル、レベル level)
    double WaveRate(const Material& air, int32_t level, uint32_t length) {
        const double coefficient = static_cast<double>(air.conductance) *
                                   static_cast<double>(HC_LIMIT_PER_CONDUCTANCE) * std::pow(4.0, level);
        const double wave = std::numbers::pi / static_cast<double>(length);

        return coefficient / static_cast<double>(air.heatCapacity) * wave * wave;
    }

    struct StickResult {
        double ratio = 0.0;
        uint64_t ticks = 0;
        size_t gridLevels = 0;
        Watch watch;
    };

    StickResult MeasureStick(const Material& air, int32_t level, uint32_t length, const ImplicitOptions& options) {
        ImplicitGrid grid = BuildImplicitGrid(MakeWaveCells(air, level, length));
        const double rate = WaveRate(air, level, length);
        StickResult result;
        result.ticks = std::clamp<uint64_t>(static_cast<uint64_t>(std::ceil(STICK_DECAY / rate)), 2, STICK_MAX_TICKS);
        result.gridLevels = grid.levels.size();
        result.watch = StartWatch(grid);
        const double start = WaveAmplitude(grid, length);
        for (uint64_t tick = 0; tick < result.ticks; ++tick)
            StepWatched(grid, options, result.watch);

        const double end = WaveAmplitude(grid, length);
        result.ratio = std::log(start / end) / (static_cast<double>(result.ticks) * rate);

        return result;
    }

    void LogStick(const Method& method, int32_t gap, int32_t level, uint32_t length, const StickResult& result) {
        const double cells = static_cast<double>(length * STICK_CROSS * STICK_CROSS);
        Log(Channel::Sim, Level::Info,
            "物差し: Δk {}(レベル {}・山 {} セル){}: 実効 ÷ 本当 = {:.4f}({} 刻み)・多重格子 {} 段・RKL2 {} 段・"
            "1 刻みに 1 セルを {:.1f} 回・直列 {} 段・V 最大 {} 回・安全網 {} セル(超えた最大 {} mK)・行き過ぎ "
            "{}・保存のずれ {}",
            gap, level, length, method.name, result.ratio, result.ticks, result.gridLevels, result.watch.cost.stages,
            static_cast<double>(result.watch.cost.cellUpdates) / cells, result.watch.cost.passes,
            result.watch.maxCycles, result.watch.limitedCells, result.watch.worstExcess, result.watch.overshoots,
            result.watch.drifts);
    }

    void CheckStick(const Material& air, int32_t base) {
        std::array<uint64_t, STICK_GAPS.size()> chosenPasses{};
        std::array<uint64_t, STICK_GAPS.size()> fixedPasses{};  // V 2 回(回数が一定の時の 1 サイクルの費用)
        for (size_t g = 0; g < STICK_GAPS.size(); ++g) {
            const int32_t gap = STICK_GAPS[g];
            const int32_t level = base + gap;
            const uint32_t length = 8u << gap;
            for (size_t m = 0; m < METHODS.size(); ++m) {
                const Method& method = METHODS[m];
                if (gap > method.highestGap)
                    continue;

                const StickResult result = MeasureStick(air, level, length, method.options);
                LogStick(method, gap, level, length, result);
                Expect(result.watch.drifts == 0,
                       std::format("物差し: Δk {} の {} で保存量が毎刻み最初とビット一致", gap, method.name));
                if (m == FIXED_METHOD)
                    fixedPasses[g] = result.watch.cost.passes;

                if (m != CHOSEN_METHOD)
                    continue;

                chosenPasses[g] = result.watch.cost.passes;
                Expect(std::abs(result.ratio - 1.0) <= STICK_TOLERANCE,
                       std::format("物差し: Δk {} の {} で実効の拡散率が本当の値の ±5% の中", gap, method.name));
                Expect(result.watch.overshoots == 0,
                       std::format("物差し: Δk {} の {} で温度が最初の範囲の外に出ない", gap, method.name));
                Expect(result.watch.limitedCells == 0,
                       std::format("物差し: Δk {} の {} で安全網が働かない(近似が十分)", gap, method.name));
                Expect(result.gridLevels <= static_cast<size_t>(gap) + 1,
                       std::format("物差し: Δk {} の多重格子は基準のレベルまでの {} 段以下", gap, gap + 1));
            }
        }

        // V サイクル 1 回の費用はレベルの数に比例する(回数が一定の V 2 回の直列の段: Δk 8 が Δk 4 の 2.5 倍以下)。
        // 適応では要る回数も Δk とともに増える(残差が D/C ≈ 4^Δk 倍になって新しい温度に出るため)ので、記録だけ
        Log(Channel::Sim, Level::Info, "物差し: 直列の段 Δk 4 → Δk 8: {} は {} → {}・{} は {} → {}",
            METHODS[FIXED_METHOD].name, fixedPasses[4], fixedPasses[7], METHODS[CHOSEN_METHOD].name, chosenPasses[4],
            chosenPasses[7]);
        Expect(fixedPasses[7] * 2 <= fixedPasses[4] * 5, "物差し: V サイクル 1 回の直列の段がレベルの差に比例する");
    }

    // 短い山(8 セル)を細かいレベルに: 本当は 1 刻みで exp(−rate) に減る
    void LogShortWave(const Material& air, int32_t base) {
        const int32_t level = base + SHORT_WAVE_GAP;
        constexpr uint32_t LENGTH = 8;
        const double rate = WaveRate(air, level, LENGTH);
        for (const Method& method : METHODS) {
            ImplicitGrid grid = BuildImplicitGrid(MakeWaveCells(air, level, LENGTH));
            Watch watch = StartWatch(grid);
            StepWatched(grid, method.options, watch);
            const double first = WaveAmplitude(grid, LENGTH);
            StepWatched(grid, method.options, watch);
            const double second = WaveAmplitude(grid, LENGTH);
            Log(Channel::Sim, Level::Info,
                "短い山: Δk {}・{}: 振幅 {} mK → 1 刻み {:.3f} mK・2 刻み {:.3f} mK(本当 {:.3g}・{:.3g})・安全網 {} "
                "セル・行き過ぎ {}",
                SHORT_WAVE_GAP, method.name, STICK_AMPLITUDE, first, second,
                static_cast<double>(STICK_AMPLITUDE) * std::exp(-rate),
                static_cast<double>(STICK_AMPLITUDE) * std::exp(-2.0 * rate), watch.limitedCells, watch.overshoots);
            Expect(watch.overshoots == 0,
                   std::format("短い山: {} で温度が最初の範囲の外に出ない(安全網)", method.name));
            Expect(watch.drifts == 0, std::format("短い山: {} で保存量がビット一致", method.name));
        }
    }

    // --- 違うレベルの面を含む場面 ---
    // レベル 基準 の 4 セルの列(x = 0〜3)の x = 1 をレベル 基準 + 3 の 8³ セルに細かくし、そのうち (8, 3, 3) をさらに
    // レベル 基準 + 6 の 8³ セルに細かくする。最も細かい所の −x の面はレベル 基準 のセル (0, 0, 0) に面する(差 6)。
    // 最も細かい所だけ 1500 K、ほかは 300 K。

    constexpr int64_t COMPOSITE_COLD = 300000;
    constexpr int64_t COMPOSITE_HOT = 1500000;
    constexpr uint64_t COMPOSITE_TICKS = 30;
    constexpr int32_t COMPOSITE_STEP = 3;  // 細かくする 1 回のレベルの差
    constexpr int64_t COMPOSITE_EDGE = 8;
    constexpr uint32_t COMPOSITE_NO_LIMIT_SLACK = 1000000;  // mK

    std::vector<ImplicitCell> MakeCompositeCells(const Material& air, int32_t base) {
        std::vector<ImplicitCell> cells;
        for (int64_t x : {0, 2, 3})
            cells.push_back(MakeCell(air, base, x, 0, 0, COMPOSITE_COLD));

        const int32_t middle = base + COMPOSITE_STEP;
        const int32_t finest = middle + COMPOSITE_STEP;
        for (int64_t z = 0; z < COMPOSITE_EDGE; ++z) {
            for (int64_t y = 0; y < COMPOSITE_EDGE; ++y) {
                for (int64_t x = COMPOSITE_EDGE; x < 2 * COMPOSITE_EDGE; ++x) {
                    if (x != COMPOSITE_EDGE || y != 3 || z != 3)
                        cells.push_back(MakeCell(air, middle, x, y, z, COMPOSITE_COLD));

                    const int64_t fineX = (COMPOSITE_EDGE * COMPOSITE_EDGE) + x - COMPOSITE_EDGE;
                    cells.push_back(MakeCell(air, finest, fineX, (3 * COMPOSITE_EDGE) + y, (3 * COMPOSITE_EDGE) + z,
                                             COMPOSITE_HOT));
                }
            }
        }

        return cells;
    }

    double FinestMean(const ImplicitGrid& grid, int32_t finest) {
        double sum = 0.0;
        uint32_t count = 0;
        for (const ImplicitCell& cell : grid.cells) {
            if (cell.level != finest)
                continue;

            sum += static_cast<double>(ImplicitTemperature(cell)) / TEMPERATURE_SCALE;
            count += 1;
        }

        return sum / static_cast<double>(count);
    }

    struct CompositeRun {
        std::array<double, 3> means{};  // 最も細かい所の平均の温度(1・5・30 刻み後)
        uint32_t fractionCells = 0;
        Watch watch;
        std::vector<ImplicitCell> cells;
    };

    CompositeRun RunComposite(const Material& air, int32_t base, const ImplicitOptions& options,
                              bool galerkin = false) {
        ImplicitGrid grid = BuildImplicitGrid(MakeCompositeCells(air, base), galerkin);
        const int32_t finest = base + (2 * COMPOSITE_STEP);
        CompositeRun run;
        run.watch = StartWatch(grid);
        for (uint64_t tick = 1; tick <= COMPOSITE_TICKS; ++tick) {
            StepWatched(grid, options, run.watch);
            if (tick == 1)
                run.means[0] = FinestMean(grid, finest);
            else if (tick == 5)
                run.means[1] = FinestMean(grid, finest);
        }

        run.means[2] = FinestMean(grid, finest);
        run.fractionCells = static_cast<uint32_t>(
            std::ranges::count_if(grid.cells, [](const ImplicitCell& cell) { return cell.fraction != 0; }));
        run.cells = grid.cells;

        return run;
    }

    bool SameCells(const std::vector<ImplicitCell>& a, const std::vector<ImplicitCell>& b) {
        return std::ranges::equal(a, b, [](const ImplicitCell& left, const ImplicitCell& right) {
            return left.energy == right.energy && left.fraction == right.fraction;
        });
    }

    void CheckComposite(const Material& air, int32_t base) {
        const ImplicitGrid grid = BuildImplicitGrid(MakeCompositeCells(air, base));
        const auto deepFaces = std::ranges::count_if(grid.faces,
                                                     [](const ImplicitFace& face) { return face.gap == 6; });
        const auto stepFaces = std::ranges::count_if(grid.faces,
                                                     [](const ImplicitFace& face) { return face.gap == 3; });
        Log(Channel::Sim, Level::Info, "違うレベル: セル {}・面 {}(差 3 が {}・差 6 が {})・多重格子 {} 段",
            grid.cells.size(), grid.faces.size(), stepFaces, deepFaces, grid.levels.size());
        Expect(deepFaces == COMPOSITE_EDGE * COMPOSITE_EDGE, "違うレベル: 差 6 の面が 64 枚");

        for (size_t m = 0; m < METHODS.size(); ++m) {
            const Method& method = METHODS[m];
            const CompositeRun run = RunComposite(air, base, method.options);
            Log(Channel::Sim, Level::Info,
                "違うレベル: {}: 最も細かい所の平均 1 刻み {:.1f} K・5 刻み {:.1f} K・30 刻み {:.1f} K・端数を持つセル "
                "{}・"
                "安全網 {} セル(超えた最大 {} mK)・行き過ぎ {}・保存のずれ {}・1 刻みの計算 {} 回・直列 {} 段・V 最大 "
                "{} 回",
                method.name, run.means[0] / 1000.0, run.means[1] / 1000.0, run.means[2] / 1000.0, run.fractionCells,
                run.watch.limitedCells, run.watch.worstExcess, run.watch.overshoots, run.watch.drifts,
                run.watch.cost.cellUpdates, run.watch.cost.passes, run.watch.maxCycles);
            Expect(run.watch.drifts == 0, std::format("違うレベル: {} で保存量が毎刻み最初とビット一致", method.name));
            Expect(run.watch.overshoots == 0,
                   std::format("違うレベル: {} で温度が最初の範囲の外に出ない(安全網)", method.name));
            if (m != CHOSEN_METHOD)
                continue;

            Expect(run.fractionCells != 0, "違うレベル: 粗い側が端数で受けている");
            const CompositeRun again = RunComposite(air, base, method.options);
            Expect(SameCells(run.cells, again.cells), "違うレベル: 2 回の実行で全部が一致");
        }

        // 安全網を外した時(余裕を 1000 K に)の振る舞い: どれだけ範囲の外に出るか・速さはどうか(記録だけ)
        for (const Method& probe : COMPOSITE_PROBES) {
            ImplicitOptions options = probe.options;
            options.limitSlackMillikelvin = COMPOSITE_NO_LIMIT_SLACK;
            const CompositeRun run = RunComposite(air, base, options, probe.galerkin);
            Log(Channel::Sim, Level::Info,
                "違うレベル(安全網なし): {}: 最も細かい所の平均 1 刻み {:.1f} K・5 刻み {:.1f} K・30 刻み {:.1f} K・"
                "超えた最大 {} mK・保存のずれ {}・1 刻みの計算 {} 回・直列 {} 段・V 最大 {} 回",
                probe.name, run.means[0] / 1000.0, run.means[1] / 1000.0, run.means[2] / 1000.0, run.watch.worstExcess,
                run.watch.drifts, run.watch.cost.cellUpdates, run.watch.cost.passes, run.watch.maxCycles);
            Expect(run.watch.drifts == 0, "違うレベル(安全網なし): 保存量が毎刻み最初とビット一致");
        }
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "multires_implicit_test: FAILED(表を作れない)");
            return 1;
        }

        const Material air = AirAt(*table, static_cast<int32_t>(STICK_MEAN));
        const int32_t base = MrSubcycleBaseLevel(
            MrCellThermal(table->View(), test::MakeConductionAir(*table, static_cast<int32_t>(STICK_MEAN))));
        Log(Channel::Sim, Level::Info, "陰解法: 空気の頭打ちにならない最後のレベル(基準){}・熱容量 {}・G {}", base,
            air.heatCapacity, air.conductance);

        CheckStick(air, base);
        LogShortWave(air, base);
        CheckComposite(air, base);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_implicit_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_implicit_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
