// gpu_multires_implicit_test.cpp — 細かいレベルの熱の陰解法(方式②。ADR-0019)の GPU 版(sim/gpu_implicit・shaders/sim/implicit_conduct.hlsl)が
// CPU リファレンス(sim/implicit_conduction の StepImplicit)と毎刻みビット一致するかを確かめる(T-0117)。比べるもの: 全部のセルのエネルギーと端数・
// 回した V サイクルの数・安全網で戻したセルの数と回数・範囲を超えた最大。場面(tests/multires_implicit_scene.h):
//   - 物差しの山(Δk = 2・6・8。なめらか。V 1〜15 回)
//   - 違うレベルの面を含む熱い点(差 3 と 6。鋭い。V 12〜16 回)を V 適応(1 mK・上限 16)と V 2 回(安全網が働く)で
//   - 熱い点を 2 回走らせて一致(決定性)
// 計測(release のハードウェアだけ。--measure-only なら計測だけ): 1 刻みの GPU 時間・V サイクル 1 回・空の V サイクル(止めた後の回)・
// 安全網 1 回の空の費用、鎖の場面(tests/multires_conduction_scene.h。方式① の T-0111 と同じ世界)を方式②だけで解いた時と、
// 細かい所(基準 + 3 段より細かい)だけを方式②で解いた時の ms/刻み。結果は docs/perf.md・T-0117 のチケットへ。
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/multires_conduction.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_conduction_scene.h"
#include "multires_implicit_scene.h"
#include "sim/gpu_implicit.h"
#include "sim/implicit_conduction.h"
#include "sim/multires_nest.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;

namespace {

    constexpr uint32_t ADAPTIVE_TOLERANCE = 1;  // mK(ADR-0019)
    constexpr uint32_t ADAPTIVE_MAX_CYCLES = 16;

    sim::ImplicitOptions AdaptiveOptions() {
        return {.method = sim::ImplicitMethod::Multigrid,
                .cycles = ADAPTIVE_MAX_CYCLES,
                .toleranceMillikelvin = ADAPTIVE_TOLERANCE};
    }

    sim::ImplicitOptions FixedOptions(uint32_t cycles) {
        return {.method = sim::ImplicitMethod::Multigrid, .cycles = cycles};
    }

    using Record = std::function<bool(ID3D12GraphicsCommandList10*)>;

    // 1 本のリストを記録して投げ、読み戻す
    std::expected<void, std::string> Execute(gpu::ImmediateQueue& queue, gpu::DebugRing& ring, sim::GpuImplicit& gpu,
                                             const Record& record) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        ring.RecordBegin(list);
        if (!record(list))
            return std::unexpected("記録できない");

        gpu.RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = ring.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        return {};
    }

    // --- 比べる ---

    std::expected<void, std::string> CompareTick(const sim::ImplicitGrid& cpu, const sim::ImplicitCost& cpuCost,
                                                 const sim::ImplicitGrid& read, const sim::GpuImplicitCost& gpuCost,
                                                 uint64_t tick) {
        for (size_t i = 0; i < cpu.cells.size(); ++i) {
            if (cpu.cells[i].energy != read.cells[i].energy || cpu.cells[i].fraction != read.cells[i].fraction) {
                return std::unexpected(std::format(
                    "刻み {} のセル {}(レベル {})が食い違う: CPU {} + {} / GPU {} + {}", tick, i, cpu.cells[i].level,
                    cpu.cells[i].energy, cpu.cells[i].fraction, read.cells[i].energy, read.cells[i].fraction));
            }
        }

        if (!gpuCost.limitFinished)
            return std::unexpected(std::format("刻み {}: 安全網が記録した回数の中で止まらない", tick));

        if (cpuCost.cycles != gpuCost.cycles || cpuCost.limitedCells != gpuCost.limitedCells ||
            cpuCost.limitRounds != gpuCost.limitRounds ||
            cpuCost.worstExcessMillikelvin != gpuCost.worstExcessMillikelvin) {
            return std::unexpected(std::format(
                "刻み {} の数が食い違う: V サイクル {} / {}・戻したセル {} / {}・安全網の回数 {} / {}・超えた最大 {} / "
                "{} mK",
                tick, cpuCost.cycles, gpuCost.cycles, cpuCost.limitedCells, gpuCost.limitedCells, cpuCost.limitRounds,
                gpuCost.limitRounds, cpuCost.worstExcessMillikelvin, gpuCost.worstExcessMillikelvin));
        }

        return {};
    }

    struct SceneSummary {
        uint32_t maxCycles = 0;
        uint32_t limitedCells = 0;
        std::vector<sim::ImplicitCell> last;
    };

    // CPU と GPU を ticks 刻み進め、毎刻み比べる
    std::expected<SceneSummary, std::string> RunScene(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                      gpu::DebugRing& ring, std::vector<sim::ImplicitCell> cells,
                                                      const sim::ImplicitOptions& options, uint64_t ticks) {
        sim::ImplicitGrid cpu = sim::BuildImplicitGrid(std::move(cells));
        sim::ImplicitGrid read = cpu;
        auto gpu = sim::GpuImplicit::Create(device, cpu);
        if (!gpu)
            return std::unexpected(gpu.error());

        SceneSummary summary;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                return (tick != 0 || gpu->RecordUpload(list, cpu)) && gpu->RecordStep(list, ring.GpuAddress(), options);
            };
            if (auto executed = Execute(queue, ring, *gpu, record); !executed)
                return std::unexpected(executed.error());

            const sim::ImplicitCost cpuCost = sim::StepImplicit(cpu, options);
            sim::GpuImplicitCost gpuCost;
            if (!gpu->Read(read, gpuCost))
                return std::unexpected("読み戻せない");

            if (auto compared = CompareTick(cpu, cpuCost, read, gpuCost, tick); !compared)
                return std::unexpected(compared.error());

            summary.maxCycles = std::max(summary.maxCycles, gpuCost.cycles);
            summary.limitedCells += gpuCost.limitedCells;
        }

        summary.last = read.cells;

        return summary;
    }

    bool SameCells(const std::vector<sim::ImplicitCell>& a, const std::vector<sim::ImplicitCell>& b) {
        return std::ranges::equal(a, b, [](const sim::ImplicitCell& x, const sim::ImplicitCell& y) {
            return x.energy == y.energy && x.fraction == y.fraction;
        });
    }

    std::expected<void, std::string> RunAll(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                            const sim::BakedReactionTable& table) {
        const test::Material air = test::AirAt(table, static_cast<int32_t>(test::STICK_MEAN));
        const int32_t base = MrSubcycleBaseLevel(
            MrCellThermal(table.View(), test::MakeConductionAir(table, static_cast<int32_t>(test::STICK_MEAN))));
        constexpr uint32_t STICK_LENGTH = 8;  // 基準のレベルの山の長さ(T-0110 の物差し)
        constexpr uint64_t STICK_TICKS = 4;
        for (const int32_t gap : {2, 6, 8}) {
            const auto summary = RunScene(device, queue, ring,
                                          test::MakeWaveCells(air, base + gap, STICK_LENGTH << gap), AdaptiveOptions(),
                                          STICK_TICKS);
            if (!summary)
                return std::unexpected(std::format("物差しの山 Δk {}: {}", gap, summary.error()));

            Log(Channel::Sim, Level::Info, "物差しの山 Δk {}: {} 刻み一致(V サイクル最大 {} 回)", gap, STICK_TICKS,
                summary->maxCycles);
        }

        constexpr uint64_t COMPOSITE_TICKS = 6;
        const auto adaptive = RunScene(device, queue, ring, test::MakeCompositeCells(air, base), AdaptiveOptions(),
                                       COMPOSITE_TICKS);
        const auto again = RunScene(device, queue, ring, test::MakeCompositeCells(air, base), AdaptiveOptions(),
                                    COMPOSITE_TICKS);
        if (!adaptive || !again)
            return std::unexpected(std::format("熱い点(V 適応): {}", !adaptive ? adaptive.error() : again.error()));

        if (!SameCells(adaptive->last, again->last))
            return std::unexpected("熱い点(V 適応)の 2 回の実行が食い違う");

        Log(Channel::Sim, Level::Info, "熱い点(V 適応): {} 刻み一致・2 回一致(V サイクル最大 {} 回・安全網 {} セル)",
            COMPOSITE_TICKS, adaptive->maxCycles, adaptive->limitedCells);

        const auto fixed = RunScene(device, queue, ring, test::MakeCompositeCells(air, base), FixedOptions(2), 4);
        if (!fixed)
            return std::unexpected(std::format("熱い点(V 2 回): {}", fixed.error()));

        Log(Channel::Sim, Level::Info, "熱い点(V 2 回・安全網): 4 刻み一致(安全網で戻したセル 合計 {})",
            fixed->limitedCells);
        if (fixed->limitedCells == 0)
            return std::unexpected("熱い点(V 2 回)で安全網が働かない(場面が試験になっていない)");

        return {};
    }

    // --- 計測 ---

    constexpr uint32_t WARMUP_STEPS = 300;

    // 鎖の場面の本物の葉のセル(minLevel 以上のレベルだけ)を陰解法の試作のセルに
    std::vector<sim::ImplicitCell> ChainCells(const sim::BakedReactionTable& table, int32_t minLevel) {
        const sim::MultiresNest nest = test::MakeChainNest(table, 16);
        std::vector<sim::ImplicitCell> cells;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL || block.level < minLevel)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrIsSteppedCell(block, index))
                    continue;

                const MrThermal thermal = MrCellThermal(table.View(), sim::LoadNestCell(nest, slot, index));
                const test::Material material = {.heatCapacity = 8 * thermal.capacityLimit,
                                                 .conductance = thermal.conductance};
                cells.push_back(test::MakeCell(material, block.level, block.originX + MrCellX(index),
                                               block.originY + MrCellY(index), block.originZ + MrCellZ(index),
                                               thermal.temperature));
            }
        }

        return cells;
    }

    // 段ごとの節の数と、隣の数の最大(1 スレッドが順に回す長さ。費用の見当)
    std::string GridShape(const sim::ImplicitGrid& grid) {
        std::string text;
        for (const sim::ImplicitGridLevel& level : grid.levels) {
            uint32_t longest = 0;
            for (size_t i = 0; i + 1 < level.rowStarts.size(); ++i)
                longest = std::max(longest, level.rowStarts[i + 1] - level.rowStarts[i]);

            text += std::format("{}{}節・隣 最大 {}", text.empty() ? "" : " / ", level.levels.size(), longest);
        }

        return text;
    }

    struct Timing {
        std::vector<double> stepMs;  // 刻みごと
        std::vector<uint32_t> cycles;
        std::vector<uint32_t> limitedCells;
        uint32_t gridLevels = 0;          // 多重格子の段の数
        uint32_t dispatchesPerCycle = 0;  // V サイクル 1 回の段(Dispatch)の数
        std::string shape;
    };

    // 暖機(別の場面の刻みを WARMUP_STEPS 回)の後、cells を写して ticks 刻みを 1 刻みずつ印を打って測る
    std::expected<Timing, std::string> MeasureSteps(
        ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring, uint64_t frequency,
        std::vector<sim::ImplicitCell> cells, const sim::ImplicitOptions& options, uint32_t ticks,
        uint32_t maxLimitRounds = sim::GpuImplicit::DEFAULT_MAX_LIMIT_ROUNDS, bool predication = true) {
        const sim::ImplicitGrid grid = sim::BuildImplicitGrid(std::move(cells));
        auto gpu = sim::GpuImplicit::Create(device, grid);
        if (!gpu)
            return std::unexpected(gpu.error());

        gpu->UsePredication(predication);

        // --- 暖機(同じ場面。写し直すので測る刻みの初めは同じ)---
        const auto warm = [&](ID3D12GraphicsCommandList10* list) {
            bool recorded = gpu->RecordUpload(list, grid);
            for (uint32_t i = 0; i < WARMUP_STEPS && recorded; ++i)
                recorded = gpu->RecordStep(list, ring.GpuAddress(), FixedOptions(1), 0);

            return recorded;
        };
        if (auto executed = Execute(queue, ring, *gpu, warm); !executed)
            return std::unexpected(executed.error());

        Timing timing;
        timing.gridLevels = static_cast<uint32_t>(grid.levels.size());
        timing.dispatchesPerCycle = gpu->DispatchesPerCycle(options);
        timing.shape = GridShape(grid);
        sim::ImplicitGrid read = grid;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                bool recorded = tick != 0 || gpu->RecordUpload(list, grid);
                gpu->RecordTimestamp(list, 0);
                recorded = recorded && gpu->RecordStep(list, ring.GpuAddress(), options, maxLimitRounds);
                gpu->RecordTimestamp(list, 1);

                return recorded;
            };
            if (auto executed = Execute(queue, ring, *gpu, record); !executed)
                return std::unexpected(executed.error());

            sim::GpuImplicitCost cost;
            const std::vector<uint64_t> stamps = gpu->ReadTimestamps(2);
            if (stamps.size() != 2 || !gpu->Read(read, cost))
                return std::unexpected("読み戻せない");

            timing.stepMs.push_back(static_cast<double>(stamps[1] - stamps[0]) * 1000.0 /
                                    static_cast<double>(frequency));
            timing.cycles.push_back(cost.cycles);
            timing.limitedCells.push_back(cost.limitedCells);
        }

        return timing;
    }

    double Mean(std::span<const double> values) {
        double sum = 0;
        for (const double value : values)
            sum += value;

        return values.empty() ? 0 : sum / static_cast<double>(values.size());
    }

    std::string Series(const Timing& timing) {
        std::string text;
        for (size_t i = 0; i < timing.stepMs.size(); ++i)
            text += timing.limitedCells[i] == 0
                        ? std::format("{}{:.2f}({})", i == 0 ? "" : " ", timing.stepMs[i], timing.cycles[i])
                        : std::format("{}{:.2f}({}・戻した {})", i == 0 ? "" : " ", timing.stepMs[i], timing.cycles[i],
                                      timing.limitedCells[i]);

        return text;
    }

    constexpr uint32_t TICKS = 12;  // 計測の刻みの数

    int32_t StickBase(const sim::BakedReactionTable& table) {
        return MrSubcycleBaseLevel(
            MrCellThermal(table.View(), test::MakeConductionAir(table, static_cast<int32_t>(test::STICK_MEAN))));
    }

    std::expected<void, std::string> MeasureSticks(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                   gpu::DebugRing& ring, uint64_t frequency,
                                                   const sim::BakedReactionTable& table) {
        const test::Material air = test::AirAt(table, static_cast<int32_t>(test::STICK_MEAN));
        const int32_t base = StickBase(table);

        // --- 物差しの山(なめらか)Δk 3・6: V 適応 / V 1・2・4 回(固定)/ 安全網の上限 0 ---
        for (const int32_t gap : {3, 6}) {
            const auto cells = test::MakeWaveCells(air, base + gap, 8u << gap);
            const auto adaptive = MeasureSteps(device, queue, ring, frequency, cells, AdaptiveOptions(), TICKS);
            const auto one = MeasureSteps(device, queue, ring, frequency, cells, FixedOptions(1), TICKS);
            const auto two = MeasureSteps(device, queue, ring, frequency, cells, FixedOptions(2), TICKS);
            const auto four = MeasureSteps(device, queue, ring, frequency, cells, FixedOptions(4), TICKS);
            const auto noLimit = MeasureSteps(device, queue, ring, frequency, cells, FixedOptions(1), TICKS, 0);
            if (!adaptive || !one || !two || !four || !noLimit)
                return std::unexpected("物差しの山を測れない");

            const double perCycle = (Mean(four->stepMs) - Mean(one->stepMs)) / 3.0;
            const double perLimitRound = (Mean(one->stepMs) - Mean(noLimit->stepMs)) /
                                         sim::GpuImplicit::DEFAULT_MAX_LIMIT_ROUNDS;
            Log(Channel::Sim, Level::Info,
                "計測 物差しの山 Δk {}({} セル): V 適応 ms/刻み(回数) {} / V 1・2・4 回 {:.3f}・{:.3f}・{:.3f} ms → V "
                "1 回 {:.3f} ms / "
                "空の安全網 1 回 {:.1f} µs",
                gap, cells.size(), Series(*adaptive), Mean(one->stepMs), Mean(two->stepMs), Mean(four->stepMs),
                perCycle, perLimitRound * 1000.0);
        }

        return {};
    }

    std::expected<void, std::string> MeasureEmptyCycles(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                        gpu::DebugRing& ring, uint64_t frequency,
                                                        const sim::BakedReactionTable& table) {
        const test::Material air = test::AirAt(table, static_cast<int32_t>(test::STICK_MEAN));
        const int32_t base = StickBase(table);

        // --- 空の V サイクル: なめらかな山 Δk 3 で、上限 16 と、要った回数ちょうどの上限。述語で飛ばす / 段が空で抜けるだけ ---
        for (const bool predication : {true, false}) {
            const auto cells = test::MakeWaveCells(air, base + 3, 8u << 3);
            const auto capped = MeasureSteps(device, queue, ring, frequency, cells, AdaptiveOptions(), TICKS,
                                             sim::GpuImplicit::DEFAULT_MAX_LIMIT_ROUNDS, predication);
            if (!capped)
                return std::unexpected("空の V サイクルを測れない");

            const uint32_t needed = *std::ranges::max_element(capped->cycles);
            sim::ImplicitOptions exact = AdaptiveOptions();
            exact.cycles = needed;
            const auto tight = MeasureSteps(device, queue, ring, frequency, cells, exact, TICKS,
                                            sim::GpuImplicit::DEFAULT_MAX_LIMIT_ROUNDS, predication);
            if (!tight)
                return std::unexpected("空の V サイクルを測れない");

            const double emptyCycleMs = (Mean(capped->stepMs) - Mean(tight->stepMs)) / (ADAPTIVE_MAX_CYCLES - needed);
            Log(Channel::Sim, Level::Info,
                "計測 空の V サイクル({}・Δk 3・要った {} 回): 上限 16 {:.3f} ms・上限 {} {:.3f} ms → 空の V 1 回 "
                "{:.1f} µs"
                "({} Dispatch = {:.2f} µs/段)",
                predication ? "述語で飛ばす" : "空で抜ける", needed, Mean(capped->stepMs), needed, Mean(tight->stepMs),
                emptyCycleMs * 1000.0, tight->dispatchesPerCycle, emptyCycleMs * 1000.0 / tight->dispatchesPerCycle);
        }

        return {};
    }

    std::expected<void, std::string> MeasureHotPoint(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                     gpu::DebugRing& ring, uint64_t frequency,
                                                     const sim::BakedReactionTable& table) {
        const test::Material air = test::AirAt(table, static_cast<int32_t>(test::STICK_MEAN));
        const int32_t base = StickBase(table);

        // --- 熱い点(鋭い)。V サイクルの上限 16 / 8 / 6(上限で打ち切ると安全網が働く。判断待ちの (B) の費用)---
        for (const uint32_t maxCycles : {ADAPTIVE_MAX_CYCLES, 8u, 6u}) {
            sim::ImplicitOptions options = AdaptiveOptions();
            options.cycles = maxCycles;
            const auto timing = MeasureSteps(device, queue, ring, frequency, test::MakeCompositeCells(air, base),
                                             options, TICKS);
            if (!timing)
                return std::unexpected("熱い点を測れない");

            Log(Channel::Sim, Level::Info, "計測 熱い点(V 適応・上限 {}・V 1 回 {} Dispatch)ms/刻み(回数): {} / 段: {}",
                maxCycles, timing->dispatchesPerCycle, Series(*timing), timing->shape);
        }

        return {};
    }

    std::expected<void, std::string> MeasureChain(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                  gpu::DebugRing& ring, uint64_t frequency,
                                                  const sim::BakedReactionTable& table) {
        // --- 鎖(T-0111 の方式① と同じ世界。基準 2)。全部を方式② / 基準より細かい所だけ(案 a)/ 基準 + 3 段より細かい所だけ(案 b)。
        //     一部だけの時、外(粗い側)との面は断熱(組み込みは T-0119。見積もり)。V サイクルの上限 16 と 6(上限で打ち切る時の費用)---
        const sim::MultiresStepOptions subcycle = test::SubcycleTestOptions(table, 3);
        const int32_t chainBase = subcycle.subcycleBaseLevel;
        struct ChainCase {
            int32_t minLevel;
            uint32_t maxCycles;
            bool predication;
        };
        const std::array<ChainCase, 5> chainCases = {
            ChainCase{.minLevel = 0, .maxCycles = ADAPTIVE_MAX_CYCLES, .predication = true},
            ChainCase{.minLevel = chainBase + 1, .maxCycles = ADAPTIVE_MAX_CYCLES, .predication = true},
            ChainCase{.minLevel = chainBase + 3, .maxCycles = ADAPTIVE_MAX_CYCLES, .predication = true},
            ChainCase{.minLevel = chainBase + 1, .maxCycles = 6, .predication = true},
            ChainCase{.minLevel = chainBase + 1, .maxCycles = ADAPTIVE_MAX_CYCLES, .predication = false}};
        for (const ChainCase& chainCase : chainCases) {
            const std::vector<sim::ImplicitCell> cells = ChainCells(table, chainCase.minLevel);
            sim::ImplicitOptions options = AdaptiveOptions();
            options.cycles = chainCase.maxCycles;
            const auto timing = MeasureSteps(device, queue, ring, frequency, cells, options, 2 * TICKS,
                                             sim::GpuImplicit::DEFAULT_MAX_LIMIT_ROUNDS, chainCase.predication);
            if (!timing)
                return std::unexpected("鎖を測れない");

            const std::span<const double> later = std::span(timing->stepMs).subspan(TICKS);
            Log(Channel::Sim, Level::Info,
                "計測 鎖(レベル {} 以上・{} セル・基準 {}・段 {}・V 1 回 {} Dispatch・上限 {}{})ms/刻み(回数): {} / "
                "後半 "
                "{} 刻みの平均 "
                "{:.3f} ms / 段: {}",
                chainCase.minLevel, cells.size(), chainBase, timing->gridLevels, timing->dispatchesPerCycle,
                chainCase.maxCycles, chainCase.predication ? "" : "・述語なし", Series(*timing), TICKS, Mean(later),
                timing->shape);
        }

        return {};
    }

    std::expected<void, std::string> MeasureAll(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                const sim::BakedReactionTable& table) {
        uint64_t frequency = 0;
        if (FAILED(queue.Native()->GetTimestampFrequency(&frequency)))
            return std::unexpected("タイムスタンプの周波数を読めない");

        for (const auto& measure : {MeasureSticks, MeasureEmptyCycles, MeasureHotPoint, MeasureChain}) {
            if (auto measured = measure(device, queue, ring, frequency, table); !measured)
                return measured;
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        bool measureOnly = false;
        for (char* argument : arguments) {
            if (std::string_view(argument) == "--measure-only")
                measureOnly = true;
            else
                rest.push_back(argument);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_multires_implicit_test [--warp] [--queue direct|compute] [--measure-only]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_implicit_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

#ifdef NDEBUG
        constexpr bool RELEASE = true;
#else
        constexpr bool RELEASE = false;
#endif
        if (RELEASE && options->adapter != gpu::AdapterKind::Warp) {
            if (auto measured = MeasureAll(device->Get(), *queue, *ring, *table); !measured) {
                Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_test: FAILED(計測: {})", measured.error());
                return 1;
            }
        }

        if (measureOnly)
            return 0;

        if (auto result = RunAll(device->Get(), *queue, *ring, *table); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_test: FAILED ({})", result.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_implicit_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_multires_implicit_test: OK(陰解法の GPU と CPU が毎刻みビット一致)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
