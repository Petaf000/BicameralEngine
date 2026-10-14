// gpu_implicit_box_bench.cpp — 刻みの中で広い範囲に伝える重さ(T-0494。D-467・docs/design/19 §4)を測る。
// 1 レベルの一様な箱(一辺 16・32・64・128 セル)の熱を陰解法(方式②。sim::GpuImplicit。ADR-0019)で解き、伝わる速さ
// (×1・×4^5 ≈ ×10³・×4^10 ≈ ×10⁶)ごとに、1 刻みの GPU 時間・V サイクルの回数・
// 箱の温度の幅(その刻みでどこまで坂を下りたか)を測る。系は CPU の BuildImplicitGrid で作って写す(GPU で作る段は T-0129。値は同じ)。
// 場面: 箱の真ん中の 2³ セルが 1500 K・ほかは 300 K(鋭い熱い点。V サイクルが最も多く要る形)。物質は試験の表の空気(400 K の値)、
// 伝わる速さはセルのレベルで変える: 基準(面の係数が熱容量の上限で頭打ちにならない最後のレベル。MrSubcycleBaseLevel)から k 段細かくすると、
// 熱容量に対する面の係数(1 刻みに隣へ伝わる割合)が 4^k 倍になる(伝導率を 4^k 倍にするのと同じ式。空気の伝導率 65525 mW/K は ×10⁶ で 32bit を超える)。
// 記録の形は 2 つ: 既定(段ごとの Dispatch を上限まで積む)と、前の刻みの数から選ぶ形(ShapeFrom。T-0154・T-0179 の固定費を削る形。
// 伝導の段 GpuMultiresImplicit と同じ使い方)。保存量(エネルギー + 端数)が毎刻みビット一致しなければ失敗。
//
// ctest には登録しない(時間がかかり、時間は機械しだい)。
// 走らせ方: `job.py run -Preset release -Exe gpu_implicit_box_bench -- --no-gbv [--max-edge 64]`。
// 結果は Markdown の表の行としてログに出す(docs/perf.md・docs/tickets/T-0494-wide-propagation-cost.md に貼る)。
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
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
#include "reaction_test_table.h"
#include "sim/gpu_implicit.h"
#include "sim/implicit_conduction.h"

using namespace bicameral;
using namespace bicameral::multires;

namespace {

    constexpr uint32_t TOLERANCE_MILLIKELVIN = 1;  // ADR-0019
    constexpr uint32_t MAX_CYCLES = 64;            // 下りきるまでの回数を見るため、ふだんの上限 16 より大きく
    constexpr uint32_t TICKS = 8;                  // 測る刻みの数
    constexpr uint32_t SMALL_WARMUP_STEPS = 64;    // 暖機(一辺 32 以下)
    constexpr uint32_t LARGE_WARMUP_STEPS = 8;     // 暖機(それより大きい箱。1 本のリストが長くなりすぎないように)
    constexpr size_t SMALL_BOX_CELLS = 32 * 32 * 32;

    constexpr int64_t AIR_SAMPLE_MILLIKELVIN = 400000;
    constexpr int64_t COLD_MILLIKELVIN = 300000;
    constexpr int64_t HOT_MILLIKELVIN = 1500000;
    constexpr uint32_t TEMPERATURE_FRACTION_BITS = 16;  // ImplicitTemperature は mK × 2^16

    constexpr std::array<uint32_t, 4> EDGES = {16, 32, 64, 128};
    constexpr std::array<int32_t, 3> SPEED_LEVEL_GAPS = {0, 5, 10};  // 伝わる速さ ×4^k(×1・×1024・×1048576)

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

    // --- 場面 ---

    // 一辺 edge の箱。真ん中の 2³ セルだけ熱い
    std::vector<sim::ImplicitCell> BoxCells(const test::Material& material, int32_t level, uint32_t edge) {
        const int64_t hotLow = (static_cast<int64_t>(edge) / 2) - 1;
        const int64_t hotHigh = static_cast<int64_t>(edge) / 2;
        const auto isHot = [&](int64_t axis) {
            return axis >= hotLow && axis <= hotHigh;
        };

        std::vector<sim::ImplicitCell> cells;
        cells.reserve(static_cast<size_t>(edge) * edge * edge);
        for (int64_t z = 0; z < edge; ++z) {
            for (int64_t y = 0; y < edge; ++y) {
                for (int64_t x = 0; x < edge; ++x) {
                    const bool hot = isHot(x) && isHot(y) && isHot(z);
                    cells.push_back(test::MakeCell(material, level, x, y, z, hot ? HOT_MILLIKELVIN : COLD_MILLIKELVIN));
                }
            }
        }

        return cells;
    }

    // 箱の温度の幅(最高 − 最低。mK)
    int64_t SpreadMillikelvin(const sim::ImplicitGrid& grid) {
        int64_t lowest = std::numeric_limits<int64_t>::max();
        int64_t highest = std::numeric_limits<int64_t>::min();
        for (const sim::ImplicitCell& cell : grid.cells) {
            const int64_t temperature = sim::ImplicitTemperature(cell);
            lowest = std::min(lowest, temperature);
            highest = std::max(highest, temperature);
        }

        return (highest - lowest) >> TEMPERATURE_FRACTION_BITS;
    }

    bool SameTotal(const fx::FxU128& a, const fx::FxU128& b) {
        return a.hi == b.hi && a.lo == b.lo;
    }

    // --- 計測 ---

    struct BoxTiming {
        std::vector<double> stepMs;
        std::vector<uint32_t> cycles;
        std::vector<uint32_t> limitedCells;
        std::vector<int64_t> spreadMillikelvin;
        uint32_t levelCount = 0;
    };

    // 暖機の後、grid を写して TICKS 刻みを 1 刻みずつ印を打って測る。shapeFromPrevious なら 2 刻み目から前の刻みの数で記録の形を選ぶ
    std::expected<BoxTiming, std::string> MeasureBox(ID3D12Device5* device, gpu::ImmediateQueue& queue,
                                                     gpu::DebugRing& ring, uint64_t frequency,
                                                     const sim::ImplicitGrid& grid, bool shapeFromPrevious) {
        auto gpu = sim::GpuImplicit::Create(device, grid);
        if (!gpu)
            return std::unexpected(gpu.error());

        // --- 暖機(同じ場面。写し直すので測る刻みの初めは同じ)---
        const uint32_t warmupSteps = grid.cells.size() > SMALL_BOX_CELLS ? LARGE_WARMUP_STEPS : SMALL_WARMUP_STEPS;
        const sim::ImplicitOptions warmOptions = {.method = sim::ImplicitMethod::Multigrid, .cycles = 1};
        const auto warm = [&](ID3D12GraphicsCommandList10* list) {
            bool recorded = gpu->RecordUpload(list, grid);
            for (uint32_t i = 0; i < warmupSteps && recorded; ++i)
                recorded = gpu->RecordStep(list, ring.GpuAddress(), warmOptions, 0);

            return recorded;
        };
        if (auto executed = Execute(queue, ring, *gpu, warm); !executed)
            return std::unexpected(executed.error());

        // --- 測る ---
        const sim::ImplicitOptions options = {.method = sim::ImplicitMethod::Multigrid,
                                              .cycles = MAX_CYCLES,
                                              .toleranceMillikelvin = TOLERANCE_MILLIKELVIN};
        const fx::FxU128 initial = sim::ImplicitConservedTotal(grid);
        sim::ImplicitGrid read = grid;
        sim::GpuImplicitCost previous;
        BoxTiming timing;
        for (uint32_t tick = 0; tick < TICKS; ++tick) {
            const sim::GpuImplicitRecordShape shape = shapeFromPrevious && tick != 0
                                                          ? sim::GpuImplicit::ShapeFrom(previous)
                                                          : sim::GpuImplicitRecordShape{};
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                bool recorded = tick != 0 || gpu->RecordUpload(list, grid);
                gpu->RecordTimestamp(list, 0);
                recorded = recorded && gpu->RecordStep(list, ring.GpuAddress(), options,
                                                       sim::GpuImplicit::DEFAULT_MAX_LIMIT_ROUNDS, shape);
                gpu->RecordTimestamp(list, 1);

                return recorded;
            };
            if (auto executed = Execute(queue, ring, *gpu, record); !executed)
                return std::unexpected(executed.error());

            sim::GpuImplicitCost cost;
            const std::vector<uint64_t> stamps = gpu->ReadTimestamps(2);
            if (stamps.size() != 2 || !gpu->Read(read, cost))
                return std::unexpected("読み戻せない");

            if (!SameTotal(sim::ImplicitConservedTotal(read), initial))
                return std::unexpected(std::format("保存量がずれた(刻み {})", tick));

            timing.stepMs.push_back(static_cast<double>(stamps[1] - stamps[0]) * 1000.0 /
                                    static_cast<double>(frequency));
            timing.cycles.push_back(cost.cycles);
            timing.limitedCells.push_back(cost.limitedCells);
            timing.spreadMillikelvin.push_back(SpreadMillikelvin(read));
            timing.levelCount = cost.levelCount;
            previous = cost;
        }

        return timing;
    }

    double Mean(std::span<const double> values) {
        double sum = 0;
        for (const double value : values)
            sum += value;

        return values.empty() ? 0 : sum / static_cast<double>(values.size());
    }

    std::string Series(const BoxTiming& timing) {
        std::string text;
        for (size_t i = 0; i < timing.stepMs.size(); ++i)
            text += timing.limitedCells[i] == 0
                        ? std::format("{}{:.2f}({})", i == 0 ? "" : " ", timing.stepMs[i], timing.cycles[i])
                        : std::format("{}{:.2f}({}・戻した {})", i == 0 ? "" : " ", timing.stepMs[i], timing.cycles[i],
                                      timing.limitedCells[i]);

        return text;
    }

    // perf.md の表の 1 行(条件 | 結果)
    void LogRow(uint32_t edge, size_t cellCount, int32_t gap, bool shapeFromPrevious, const BoxTiming& timing) {
        const std::span<const double> later = std::span(timing.stepMs).subspan(1);
        const uint32_t maxCycles = *std::ranges::max_element(timing.cycles);
        Log(Channel::Sim, Level::Info,
            "| 一辺 {}({} セル・段 {})・速さ ×4^{}・記録の形 {} | 刻み 0 {:.2f} ms(V {})/ 後 {} 刻みの平均 {:.2f} ms・"
            "V 最大 {} / 温度の幅 刻み 0 の後 {} mK → 最後 {} mK |",
            edge, cellCount, timing.levelCount, gap, shapeFromPrevious ? "前の刻みから" : "既定", timing.stepMs[0],
            timing.cycles[0], later.size(), Mean(later), maxCycles, timing.spreadMillikelvin.front(),
            timing.spreadMillikelvin.back());
        Log(Channel::Sim, Level::Info, "  刻みごと ms(V): {}", Series(timing));
    }

    std::expected<void, std::string> MeasureAll(ID3D12Device5* device, gpu::ImmediateQueue& queue, gpu::DebugRing& ring,
                                                const sim::BakedReactionTable& table, uint32_t maxEdge) {
        uint64_t frequency = 0;
        if (FAILED(queue.Native()->GetTimestampFrequency(&frequency)))
            return std::unexpected("タイムスタンプの周波数を読めない");

        const MrThermal thermal = MrCellThermal(
            table.View(), test::MakeConductionAir(table, static_cast<int32_t>(AIR_SAMPLE_MILLIKELVIN)));
        const int32_t level = std::max(MrSubcycleBaseLevel(thermal), 0);
        const test::Material air = test::AirAt(table, static_cast<int32_t>(AIR_SAMPLE_MILLIKELVIN));
        Log(Channel::Sim, Level::Info, "空気(400 K): 伝導率 {} mW/K・熱容量 {}・レベル {}(基準 {})", air.conductance,
            air.heatCapacity, level, MrSubcycleBaseLevel(thermal));

        for (const uint32_t edge : EDGES) {
            if (edge > maxEdge)
                continue;

            for (const int32_t gap : SPEED_LEVEL_GAPS) {
                const sim::ImplicitGrid grid = sim::BuildImplicitGrid(BoxCells(air, level + gap, edge));
                for (const bool shapeFromPrevious : {false, true}) {
                    const auto timing = MeasureBox(device, queue, ring, frequency, grid, shapeFromPrevious);
                    if (!timing)
                        return std::unexpected(std::format("一辺 {}・×4^{}: {}", edge, gap, timing.error()));

                    LogRow(edge, grid.cells.size(), gap, shapeFromPrevious, *timing);
                }
            }
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        uint32_t maxEdge = EDGES.back();
        for (size_t index = 0; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--max-edge" && index + 1 < arguments.size())
                maxEdge = static_cast<uint32_t>(std::stoul(arguments[++index]));
            else
                rest.push_back(arguments[index]);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_implicit_box_bench [--warp] [--queue direct|compute] [--no-gbv] [--max-edge N]");
            return 2;
        }

        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_implicit_box_bench: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error, "gpu_implicit_box_bench: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        if (auto measured = MeasureAll(device->Get(), *queue, *ring, *table, maxEdge); !measured) {
            Log(Channel::Gpu, Level::Error, "gpu_implicit_box_bench: FAILED({})", measured.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_implicit_box_bench"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_implicit_box_bench: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
