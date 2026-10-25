// gpu_time_step_span_test.cpp — 時間を飛ばす試作(sim/time_step_tree)の列の系を、釣り合う家族の部品の GPU 版で解き、CPU リファレンス
// (sim/equilibrium の SolveEquilibrium)とビット一致で確かめ、1 出来事の時間を測る(T-1111・T-1109。ADR-0465・ADR-0472)。
// 系は time_step_tree_test の span-dump が火事(0.5 m の根だけの木 = 同じレベルの系)の途中から書いたファイル(tests/time_step_span_file.h):
//   e = 0 の系 8 つ(着火の 1〜4000 刻み後)と e > 0 の系(違う e。e > 8 は V の回数が違う)。
// 1. ビット一致: どの系も V 1・V 2・その e の表の V の回数で、δ = 0 から解いた値(節ごと)を CPU と比べる。
//    段 0 は CPU が組んだ系の写しから GPU が仕上げる(sim/gpu_equilibrium_upload)。粗い段の重み・V は部品のまま。
// 2. 費用(--measure): 系ごとに、段 0 + 粗い段の重み(段の重みを作る)・V 1・V 2 の GPU の時刻印(暖機 2 回の後 8 回の最小)。
//    形の写し(段の形・段 0 の写しのアップロード)と読み戻しは含めない。
// 引数: --spans <ファイル> [--measure] と GPU の試験の共通の引数(--warp・--queue・--no-gbv)。
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/equilibrium.h"
#include "sim/gpu_equilibrium_upload.h"
#include "time_step_span_file.h"

using namespace bicameral;

namespace {

    constexpr uint32_t MEASURE_WARMUP = 2;
    constexpr uint32_t MEASURE_ROUNDS = 8;

    struct Context {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        uint64_t frequency = 0;
    };

    // --- CPU リファレンス(試作の SolveSpan と同じ形: δ = 0 から・面の値は出さない)---
    struct CpuSolve {
        sim::ImplicitGrid grid;
        std::vector<int64_t> values;
    };

    CpuSolve SolveCpu(const sim::EquilibriumSystem& system, uint32_t cycles) {
        sim::EquilibriumOptions options;
        options.solver.cycles = cycles;
        options.faceValues = false;
        CpuSolve result;
        const sim::EquilibriumObserver observer = [&result](const sim::EquilibriumSolveView& view) {
            result.grid = view.grid;
        };

        sim::EquilibriumState state;
        (void)sim::SolveEquilibrium(system, options, state, &observer);
        result.values = state.values;

        return result;
    }

    std::expected<void, std::string> Execute(Context& context, ID3D12GraphicsCommandList* list) {
        context.ring->RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debug = context.ring->Drain();
        if (debug.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debug.assertCount));

        return {};
    }

    std::expected<std::vector<int64_t>, std::string> SolveGpu(Context& context, sim::GpuEquilibriumUpload& gpu,
                                                              const sim::ImplicitGrid& grid, uint32_t cycles) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        const uint64_t ring = context.ring->GpuAddress();
        context.ring->RecordBegin(list);
        if (!gpu.RecordUpload(list, grid) || !gpu.RecordSolve(list, ring, cycles))
            return std::unexpected("段の形が合わない");

        gpu.RecordValuesReadback(list);
        gpu.RecordEnd(list);
        const auto executed = Execute(context, list);
        if (!executed)
            return std::unexpected(executed.error());

        gpu.ReleaseStaging();
        std::vector<int64_t> values;
        if (!gpu.ReadValues(values))
            return std::unexpected("読み戻せない");

        return values;
    }

    std::string FirstDifference(std::span<const int64_t> cpu, std::span<const int64_t> gpu) {
        if (cpu.size() != gpu.size())
            return std::format("数が違う(CPU {}・GPU {})", cpu.size(), gpu.size());

        for (size_t i = 0; i < cpu.size(); ++i) {
            if (cpu[i] != gpu[i])
                return std::format("節 {}(CPU {}・GPU {})", i, cpu[i], gpu[i]);
        }

        return {};
    }

    std::string NameOf(const test::TimeStepSpanRecord& record, size_t index) {
        return std::format("系 {}(e {}・歩の初め {}・節 {})", index, record.exponent, record.start,
                           record.system.nodes.size());
    }

    // --- 1. ビット一致 ---
    std::expected<void, std::string> Compare(Context& context, const std::vector<test::TimeStepSpanRecord>& records) {
        for (size_t index = 0; index < records.size(); ++index) {
            const test::TimeStepSpanRecord& record = records[index];
            std::vector<uint32_t> cyclesList = {1, 2, record.cycles};
            std::ranges::sort(cyclesList);
            cyclesList.erase(std::unique(cyclesList.begin(), cyclesList.end()), cyclesList.end());
            for (const uint32_t cycles : cyclesList) {
                const CpuSolve cpu = SolveCpu(record.system, cycles);
                auto gpu = sim::GpuEquilibriumUpload::Create(context.device, record.system, cpu.grid);
                if (!gpu)
                    return std::unexpected(std::format("{}: {}", NameOf(record, index), gpu.error()));

                const auto values = SolveGpu(context, *gpu, cpu.grid, cycles);
                if (!values)
                    return std::unexpected(std::format("{}・V {}: {}", NameOf(record, index), cycles, values.error()));

                const std::string difference = FirstDifference(cpu.values, *values);
                if (!difference.empty())
                    return std::unexpected(std::format("{}・V {}: {}", NameOf(record, index), cycles, difference));

                int64_t widest = 0;
                for (const int64_t value : cpu.values)
                    widest = std::max(widest, value < 0 ? -value : value);

                Log(Channel::Sim, Level::Info, "一致 {}・V {}: 段 {}・項 {}・δ の最大 {:.3f} mK", NameOf(record, index),
                    cycles, cpu.grid.levels.size(), gpu->Terms(), (double)widest / 65536.0);
            }
        }

        return {};
    }

    // --- 2. 費用 ---
    struct Cost {
        double weightsMs = 0;              // 段 0 + 粗い段の重み
        std::array<double, 2> cyclesMs{};  // V 1・V 2(重みは使い回し)
    };

    double ToMs(const Context& context, uint64_t begin, uint64_t end) {
        return (double)(end - begin) * 1000.0 / (double)context.frequency;
    }

    std::expected<Cost, std::string> MeasureOne(Context& context, sim::GpuEquilibriumUpload& gpu,
                                                const sim::ImplicitGrid& grid) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        const uint64_t ring = context.ring->GpuAddress();
        context.ring->RecordBegin(list);
        if (!gpu.RecordUpload(list, grid))
            return std::unexpected("段の形が合わない");

        // 回ごとに 3 つの区間(重み・V 1・V 2)
        constexpr uint32_t PARTS = 3;
        uint32_t stamp = 0;
        for (uint32_t round = 0; round < MEASURE_WARMUP + MEASURE_ROUNDS; ++round) {
            for (uint32_t part = 0; part < PARTS; ++part) {
                gpu.RecordTimestamp(list, stamp++);
                const bool recorded = part == 0 ? gpu.RecordSolve(list, ring, 0, true)
                                                : gpu.RecordSolve(list, ring, part, false);
                if (!recorded)
                    return std::unexpected("解きを積めない");

                gpu.RecordTimestamp(list, stamp++);
            }
        }

        gpu.RecordTimestampReadback(list, stamp);
        gpu.RecordEnd(list);
        const auto executed = Execute(context, list);
        if (!executed)
            return std::unexpected(executed.error());

        gpu.ReleaseStaging();
        const std::vector<uint64_t> ticks = gpu.ReadTimestamps(stamp);
        if (ticks.size() != stamp)
            return std::unexpected("時間を読めない");

        Cost cost;
        for (uint32_t round = MEASURE_WARMUP; round < MEASURE_WARMUP + MEASURE_ROUNDS; ++round) {
            const uint32_t base = round * PARTS * 2;
            const bool first = round == MEASURE_WARMUP;
            const double weights = ToMs(context, ticks[base], ticks[base + 1]);
            cost.weightsMs = first ? weights : std::min(cost.weightsMs, weights);
            for (uint32_t v = 0; v < 2; ++v) {
                const double ms = ToMs(context, ticks[base + 2 + (2 * v)], ticks[base + 3 + (2 * v)]);
                cost.cyclesMs[v] = first ? ms : std::min(cost.cyclesMs[v], ms);
            }
        }

        return cost;
    }

    // V の記録の形を前の解きの数から選ぶ(本物の GPU 版と同じ。GpuImplicit::ShapeFrom。値は同じ)
    std::expected<sim::GpuImplicitRecordShape, std::string> FitShape(Context& context, sim::GpuEquilibriumUpload& gpu,
                                                                     const sim::ImplicitGrid& grid) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        context.ring->RecordBegin(list);
        if (!gpu.RecordUpload(list, grid) || !gpu.RecordSolve(list, context.ring->GpuAddress(), 1))
            return std::unexpected("段の形が合わない");

        gpu.RecordCostReadback(list);
        gpu.RecordEnd(list);
        const auto executed = Execute(context, list);
        if (!executed)
            return std::unexpected(executed.error());

        gpu.ReleaseStaging();
        sim::GpuImplicitCost cost;
        if (!gpu.ReadCost(cost))
            return std::unexpected("数を読めない");

        return sim::GpuImplicit::ShapeFrom(cost);
    }

    void LogCost(std::string_view name, std::string_view shape, const sim::GpuEquilibriumUpload& gpu,
                 const sim::ImplicitGrid& grid, const Cost& cost) {
        const double perCell = 1.0e6 / (double)gpu.Cells();
        Log(Channel::Sim, Level::Info,
            "費用 {}・{}: 段 {}・段の重みを作る {:.4f} ms・V 1 {:.4f} ms・V 2 {:.4f} ms → 1 出来事(重みは毎回)V 1 "
            "{:.4f} ms "
            "= {:.0f} ns/セル・V 2 {:.4f} ms = {:.0f} ns/セル・(重みは使い回し)V 1 {:.0f} ns/セル・V 2 {:.0f} ns/セル・"
            "VRAM {:.2f} MB",
            name, shape, grid.levels.size(), cost.weightsMs, cost.cyclesMs[0], cost.cyclesMs[1],
            cost.weightsMs + cost.cyclesMs[0], (cost.weightsMs + cost.cyclesMs[0]) * perCell,
            cost.weightsMs + cost.cyclesMs[1], (cost.weightsMs + cost.cyclesMs[1]) * perCell,
            cost.cyclesMs[0] * perCell, cost.cyclesMs[1] * perCell, (double)gpu.DeviceBytes() / 1.0e6);
    }

    std::expected<void, std::string> Measure(Context& context, const std::vector<test::TimeStepSpanRecord>& records) {
        // 暖機(GPU の時計が上がるまで。最初の系を 3 回)
        for (size_t round = 0; round < 3 + records.size(); ++round) {
            const size_t index = round < 3 ? 0 : round - 3;
            const test::TimeStepSpanRecord& record = records[index];
            const CpuSolve cpu = SolveCpu(record.system, 1);
            auto gpu = sim::GpuEquilibriumUpload::Create(context.device, record.system, cpu.grid);
            if (!gpu)
                return std::unexpected(gpu.error());

            const auto cost = MeasureOne(context, *gpu, cpu.grid);
            if (!cost)
                return std::unexpected(cost.error());

            if (round < 3)
                continue;

            LogCost(NameOf(record, index), "記録の形は既定", *gpu, cpu.grid, *cost);
            const auto shape = FitShape(context, *gpu, cpu.grid);
            if (!shape)
                return std::unexpected(shape.error());

            gpu->SetSolveShape(*shape);
            const auto fitted = MeasureOne(context, *gpu, cpu.grid);
            if (!fitted)
                return std::unexpected(fitted.error());

            LogCost(NameOf(record, index),
                    std::format("記録の形は前の解きから(段 {}・最も粗い段の Dispatch {})", shape->dispatchLevels,
                                shape->coarsestDispatch ? "あり" : "なし"),
                    *gpu, cpu.grid, *fitted);
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::string spansPath;
        bool measure = false;
        std::vector<char*> rest = {arguments[0]};
        for (size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--spans" && index + 1 < arguments.size())
                spansPath = arguments[++index];
            else if (argument == "--measure")
                measure = true;
            else
                rest.push_back(arguments[index]);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options || spansPath.empty()) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_time_step_span_test --spans <ファイル> [--warp] [--queue direct|compute] [--no-gbv] "
                "[--measure]");
            return 2;
        }

        const auto records = test::ReadTimeStepSpans(spansPath);
        if (!records || records->empty()) {
            Log(Channel::Gpu, Level::Error, "gpu_time_step_span_test: FAILED(系のファイルを読めない: {})", spansPath);
            return 1;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_time_step_span_test: FAILED(デバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        uint64_t frequency = 0;
        if (!queue || !ring || FAILED(queue->Native()->GetTimestampFrequency(&frequency))) {
            Log(Channel::Gpu, Level::Error, "gpu_time_step_span_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        Context context{.device = device->Get(), .queue = &*queue, .ring = &*ring, .frequency = frequency};
        auto ran = Compare(context, *records);
        if (ran && measure)
            ran = Measure(context, *records);

        if (!ran) {
            Log(Channel::Gpu, Level::Error, "gpu_time_step_span_test: FAILED({})", ran.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_time_step_span_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_time_step_span_test: OK({} 系)", records->size());

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
