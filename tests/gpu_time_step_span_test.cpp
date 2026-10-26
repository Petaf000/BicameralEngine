// gpu_time_step_span_test.cpp — 時間を飛ばす試作(sim/time_step_tree)の列の系を、釣り合う家族の部品の GPU 版で解き、CPU リファレンス
// (sim/equilibrium の SolveEquilibrium)とビット一致で確かめ、1 出来事の時間を測る(T-1111・T-1109。ADR-0465・ADR-0472)。
// 系は time_step_tree_test の span-dump が火事(0.5 m の根だけの木 = 同じレベルの系)の途中から書いたファイル(tests/time_step_span_file.h):
//   e = 0 の系 8 つ(着火の 1〜4000 刻み後)と e > 0 の系(違う e。e > 8 は V の回数が違う)。
// 1. ビット一致: どの系も V 1・V 2・その e の表の V の回数で、δ = 0 から解いた値(節ごと)を CPU と比べる。
//    段 0 は CPU が組んだ系の写しから GPU が仕上げる(sim/gpu_equilibrium_upload)。粗い段の重み・V は部品のまま。
// 2. 費用(--measure): 系ごとに、段 0 + 粗い段の重み(段の重みを作る)・V 1・V 2 の GPU の時刻印(暖機 2 回の後 8 回の最小)。
//    形の写し(段の形・段 0 の写しのアップロード)と読み戻しは含めない。
// 3. 出来事(T-1141。系のファイルが time_step_tree_test event-merge の出来事の番号付き): 出来事ごとに列を組にまとめ(sim::GroupTimeTreeEvent)、
//    まとめた系を GPU で解いた値が列ごとに CPU で解いた値を並べたものとビット一致か。--measure なら 1 出来事の GPU の時間を
//    列ごと(今の口)と組ごとで比べ(どちらも段 0 + 段の重み + その V の回数・記録の形は前の解きから)、--events-total n で火事全体へ伸ばす。
//    V 2 の組は部品を V 2 で作った形(V の回数を作る時に決める)とも比べる。
// 4. --stage-variants: 系ごとに段 0 の段だけの時間を、本物・右辺の項を割らない・読むだけの 3 つの変種で(128bit の割り算の分)。
// 5. --continuous(T-1152・出来事のファイル): 全部の組を 1 本のリストに続けて積んだ時間(GPU の時計が高いまま = 本物の早送りの形)と、
//    組ごとに CPU で待ってから 1 回解いた時間(時計が下がる形)。費用の式を最小二乗で当て、起動時の測り(sim/gpu_time_tree_cost)の
//    式と比べる。V 2 の組は部品を V 2 で作った形とも。
// 6. --clock(T-1140・出来事のファイル): 最も大きい組を、間を空けて・続けて積んで・また間を空けて解き、時刻と時間を並べる。
// 引数: --spans <ファイル> [--measure] [--tile n(費用の系を n 個並べる)] [--events-total n] [--stage-variants]
//       [--continuous] [--clock] と
//       GPU の試験の共通の引数(--warp・--queue・--no-gbv)。
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/equilibrium.h"
#include "sim/gpu_equilibrium_upload.h"
#include "sim/gpu_time_tree_cost.h"
#include "sim/time_step_tree.h"
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
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
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
                                                                     const sim::ImplicitGrid& grid,
                                                                     uint32_t cycles = 1) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        context.ring->RecordBegin(list);
        if (!gpu.RecordUpload(list, grid) || !gpu.RecordSolve(list, context.ring->GpuAddress(), cycles))
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

    // 系を tile 個並べる(離れた所に写す。費用がセルの数でどう伸びるかを見る。値は比べない)
    test::TimeStepSpanRecord Tile(const test::TimeStepSpanRecord& record, uint32_t tile) {
        constexpr int64_t APART = 1 << 20;  // 写しどうしは面でつながらず、粗い段でも混ざらない
        test::TimeStepSpanRecord tiled = record;
        sim::EquilibriumSystem& system = tiled.system;
        const auto count = static_cast<uint32_t>(record.system.nodes.size());
        for (uint32_t copy = 1; copy < tile; ++copy) {
            const uint32_t offset = copy * count;
            for (sim::EquilibriumNode node : record.system.nodes) {
                node.z += APART * copy;
                system.nodes.push_back(node);
            }

            for (sim::EquilibriumFace face : record.system.faces) {
                face.fine += offset;
                face.coarse += offset;
                system.faces.push_back(face);
            }

            for (sim::EquilibriumFixedFace fixed : record.system.fixedFaces) {
                fixed.node += offset;
                system.fixedFaces.push_back(fixed);
            }

            for (sim::EquilibriumSource source : record.system.sources) {
                source.node += offset;
                system.sources.push_back(source);
            }
        }

        return tiled;
    }

    std::expected<void, std::string> Measure(Context& context, const std::vector<test::TimeStepSpanRecord>& source,
                                             uint32_t tile) {
        std::vector<test::TimeStepSpanRecord> records;
        for (const test::TimeStepSpanRecord& record : source)
            records.push_back(tile > 1 ? Tile(record, tile) : record);

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

    // --- 3. 出来事ごとにまとめた組(T-1141)---
    constexpr uint32_t SOLVE_WARMUP = 2;
    constexpr uint32_t SOLVE_ROUNDS = 6;

    // 1 回の解き(段 0 + 段の重み + V × cycles。cycles = 0 なら段 0 + 段の重みだけ)の GPU の時間(暖機の後の最小)
    std::expected<double, std::string> TimeSolve(Context& context, sim::GpuEquilibriumUpload& gpu,
                                                 const sim::ImplicitGrid& grid, uint32_t cycles) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        const uint64_t ring = context.ring->GpuAddress();
        context.ring->RecordBegin(list);
        if (!gpu.RecordUpload(list, grid))
            return std::unexpected("段の形が合わない");

        uint32_t stamp = 0;
        for (uint32_t round = 0; round < SOLVE_WARMUP + SOLVE_ROUNDS; ++round) {
            gpu.RecordTimestamp(list, stamp++);
            if (!gpu.RecordSolve(list, ring, cycles, true))
                return std::unexpected("解きを積めない");

            gpu.RecordTimestamp(list, stamp++);
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

        double best = 0;
        for (uint32_t round = SOLVE_WARMUP; round < SOLVE_WARMUP + SOLVE_ROUNDS; ++round) {
            const double ms = ToMs(context, ticks[2 * round], ticks[(2 * round) + 1]);
            best = round == SOLVE_WARMUP ? ms : std::min(best, ms);
        }

        return best;
    }

    // 系 1 つを本物の形(記録の形は前の解きから)で解く時間
    std::expected<double, std::string> MeasureSystem(Context& context, const sim::EquilibriumSystem& system,
                                                     uint32_t cycles,
                                                     const sim::GpuEquilibriumUploadOptions& options = {}) {
        const CpuSolve cpu = SolveCpu(system, 1);
        auto gpu = sim::GpuEquilibriumUpload::Create(context.device, system, cpu.grid, options);
        if (!gpu)
            return std::unexpected(gpu.error());

        const auto shape = FitShape(context, *gpu, cpu.grid, std::max(options.cyclesPerRecord, 1u));
        if (!shape)
            return std::unexpected(shape.error());

        gpu->SetSolveShape(*shape);

        return TimeSolve(context, *gpu, cpu.grid, cycles);
    }

    // --- 5. 出来事を続けて積む形と間を空けた形(T-1152・T-1140。本物の早送りは出来事を続けて積む = GPU の時計が高いまま)---
    constexpr uint32_t CONTINUOUS_ROUNDS = 5;  // 1 回目は暖機(数えない)
    constexpr uint32_t GAP_MS = 20;            // 間を空けた形の、出来事の間の CPU の待ち

    struct MeasuredGroup {
        const sim::TimeTreeEventGroup* group = nullptr;
        sim::ImplicitGrid grid;
        std::optional<sim::GpuEquilibriumUpload> gpu;
        std::optional<sim::GpuEquilibriumUpload> built;  // V 2 の組: 部品を V 2 で作った形
        double gapMs = 0;
        double minMs = 0;  // 続けて積んだ形の回の最小
        double maxMs = 0;  // 回の最大(揺れ)
        double builtMs = 0;
    };

    std::expected<void, std::string> ReadContinuous(Context& context, std::vector<MeasuredGroup>& measured);
    std::expected<void, std::string> MeasureClockContinuous(Context& context, sim::GpuEquilibriumUpload& gpu,
                                                            const sim::ImplicitGrid& grid, uint32_t cycles);

    double SecondsSince(const Context& context) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - context.start).count();
    }

    std::expected<sim::GpuEquilibriumUpload, std::string> MakeFitted(Context& context,
                                                                     const sim::EquilibriumSystem& system,
                                                                     const sim::ImplicitGrid& grid,
                                                                     uint32_t cyclesPerRecord) {
        auto gpu = sim::GpuEquilibriumUpload::Create(context.device, system, grid,
                                                     {.cyclesPerRecord = cyclesPerRecord});
        if (!gpu)
            return std::unexpected(gpu.error());

        const auto shape = FitShape(context, *gpu, grid, cyclesPerRecord);
        if (!shape)
            return std::unexpected(shape.error());

        gpu->SetSolveShape(*shape);

        return gpu;
    }

    // 1 回だけの解き(前に CPU で待つ = GPU が休む)
    std::expected<double, std::string> TimeSolveOnce(Context& context, sim::GpuEquilibriumUpload& gpu,
                                                     const sim::ImplicitGrid& grid, uint32_t cycles) {
        std::this_thread::sleep_for(std::chrono::milliseconds(GAP_MS));
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        context.ring->RecordBegin(list);
        if (!gpu.RecordUpload(list, grid))
            return std::unexpected("段の形が合わない");

        gpu.RecordTimestamp(list, 0);
        if (!gpu.RecordSolve(list, context.ring->GpuAddress(), cycles, true))
            return std::unexpected("解きを積めない");

        gpu.RecordTimestamp(list, 1);
        gpu.RecordTimestampReadback(list, 2);
        gpu.RecordEnd(list);
        const auto executed = Execute(context, list);
        if (!executed)
            return std::unexpected(executed.error());

        gpu.ReleaseStaging();
        const std::vector<uint64_t> ticks = gpu.ReadTimestamps(2);
        if (ticks.size() != 2)
            return std::unexpected("時間を読めない");

        return ToMs(context, ticks[0], ticks[1]);
    }

    // 全部の組を 1 本のリストに CONTINUOUS_ROUNDS 回続けて積む(組ごとに回の最小と最大)
    std::expected<void, std::string> TimeContinuous(Context& context, std::vector<MeasuredGroup>& measured) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        const uint64_t ring = context.ring->GpuAddress();
        context.ring->RecordBegin(list);
        for (MeasuredGroup& item : measured) {
            if (!item.gpu->RecordUpload(list, item.grid) || (item.built && !item.built->RecordUpload(list, item.grid)))
                return std::unexpected("段の形が合わない");
        }

        for (uint32_t pass = 0; pass < CONTINUOUS_ROUNDS; ++pass) {
            for (MeasuredGroup& item : measured) {
                item.gpu->RecordTimestamp(list, 2 * pass);
                if (!item.gpu->RecordSolve(list, ring, item.group->cycles, true))
                    return std::unexpected("解きを積めない");

                item.gpu->RecordTimestamp(list, (2 * pass) + 1);
                if (!item.built)
                    continue;

                item.built->RecordTimestamp(list, 2 * pass);
                if (!item.built->RecordSolve(list, ring, 2, true))
                    return std::unexpected("解きを積めない");

                item.built->RecordTimestamp(list, (2 * pass) + 1);
            }
        }

        for (MeasuredGroup& item : measured) {
            item.gpu->RecordTimestampReadback(list, 2 * CONTINUOUS_ROUNDS);
            item.gpu->RecordEnd(list);
            if (item.built) {
                item.built->RecordTimestampReadback(list, 2 * CONTINUOUS_ROUNDS);
                item.built->RecordEnd(list);
            }
        }

        const auto executed = Execute(context, list);
        if (!executed)
            return std::unexpected(executed.error());

        return ReadContinuous(context, measured);
    }

    std::expected<void, std::string> ReadContinuous(Context& context, std::vector<MeasuredGroup>& measured) {
        std::array<double, CONTINUOUS_ROUNDS> passMs{};
        for (MeasuredGroup& item : measured) {
            item.gpu->ReleaseStaging();
            const std::vector<uint64_t> ticks = item.gpu->ReadTimestamps(2 * CONTINUOUS_ROUNDS);
            if (ticks.size() != 2 * CONTINUOUS_ROUNDS)
                return std::unexpected("時間を読めない");

            item.minMs = 1.0e9;
            for (uint32_t pass = 0; pass < CONTINUOUS_ROUNDS; ++pass) {
                const double ms = ToMs(context, ticks[2 * pass], ticks[(2 * pass) + 1]);
                passMs[pass] += ms;
                if (pass == 0)
                    continue;

                item.minMs = std::min(item.minMs, ms);
                item.maxMs = std::max(item.maxMs, ms);
            }

            if (!item.built)
                continue;

            item.built->ReleaseStaging();
            const std::vector<uint64_t> built = item.built->ReadTimestamps(2 * CONTINUOUS_ROUNDS);
            if (built.size() != 2 * CONTINUOUS_ROUNDS)
                return std::unexpected("時間を読めない");

            item.builtMs = 1.0e9;
            for (uint32_t pass = 1; pass < CONTINUOUS_ROUNDS; ++pass)
                item.builtMs = std::min(item.builtMs, ToMs(context, built[2 * pass], built[(2 * pass) + 1]));
        }

        std::string passes;
        for (const double ms : passMs)
            passes += std::format(" {:.2f}", ms);

        Log(Channel::Sim, Level::Info, "続けて積んだ形: 回ごとの全部の組の和(ms。1 回目は暖機){}", passes);

        return {};
    }

    // --- 式の当てはめ(V の回数と段の数ごとに 最小二乗: t = 固定費 + セル × 傾き)---
    struct LineFit {
        uint64_t count = 0;
        double fixedNs = 0;
        double cellPs = 0;
        double meanCells = 0;
        double meanMs = 0;
    };

    LineFit FitLine(const std::vector<const MeasuredGroup*>& items) {
        LineFit fit;
        double sumX = 0;
        double sumY = 0;
        double sumXX = 0;
        double sumXY = 0;
        for (const MeasuredGroup* item : items) {
            const auto x = (double)item->group->system.nodes.size();
            const double y = item->minMs * 1.0e6;
            sumX += x;
            sumY += y;
            sumXX += x * x;
            sumXY += x * y;
        }

        fit.count = items.size();
        const auto n = (double)fit.count;
        fit.meanCells = sumX / n;
        fit.meanMs = sumY / n * 1.0e-6;
        const double spread = (n * sumXX) - (sumX * sumX);
        fit.cellPs = spread > 0 ? ((n * sumXY) - (sumX * sumY)) / spread * 1000.0 : 0.0;
        fit.fixedNs = (sumY - (fit.cellPs / 1000.0 * sumX)) / n;

        return fit;
    }

    void LogFits(const std::vector<MeasuredGroup>& measured) {
        std::map<std::pair<uint32_t, uint32_t>, std::vector<const MeasuredGroup*>> byShape;  // (V・段) → 組
        for (const MeasuredGroup& item : measured)
            byShape[{item.group->cycles, item.group->levels}].push_back(&item);

        for (const auto& [shape, items] : byShape) {
            const LineFit fit = FitLine(items);
            double gapMs = 0;
            for (const MeasuredGroup* item : items)
                gapMs += item->gapMs;

            Log(Channel::Sim, Level::Info,
                "V {}・段 {}: 組 {}・平均 {:.0f} セル・続けて積んだ {:.4f} ms・間を空けた {:.4f} ms → 当てはめ 固定費 "
                "{:.0f} ns・"
                "セル {:.0f} ps",
                shape.first, shape.second, fit.count, fit.meanCells, fit.meanMs, gapMs / (double)fit.count * 1.0,
                fit.fixedNs, fit.cellPs);
        }
    }

    std::string CostText(const sim::TimeTreeSolveCost& cost) {
        return std::format(
            "固定費 {} ns・段 {} ns・V {} ns・段の V {} ns・セル {} ps・セル × V {} ps・選ぶ段 {} ns + 単位 {} ps",
            cost.eventNs, cost.levelNs, cost.cycleNs, cost.levelCycleNs, cost.cellPs, cost.cellCyclePs, cost.chooseNs,
            cost.chooseUnitPs);
    }

    double PredictMs(const sim::TimeTreeSolveCost& cost, const sim::TimeTreeEventGroup& group) {
        const uint64_t cells = group.system.nodes.size();

        return (double)sim::TimeTreeEventNs(cost, cells, cells * group.cycles, group.levels) * 1.0e-6;
    }

    // 式の見積もりと測りの比(全体の和の比・組ごとの比の最小と最大)
    void LogPrediction(std::string_view name, const sim::TimeTreeSolveCost& cost,
                       const std::vector<MeasuredGroup>& measured) {
        double predicted = 0;
        double actual = 0;
        double gap = 0;
        double low = 1.0e9;
        double high = 0;
        for (const MeasuredGroup& item : measured) {
            const double ms = PredictMs(cost, *item.group);
            predicted += ms;
            actual += item.minMs;
            gap += item.gapMs;
            low = std::min(low, ms / item.minMs);
            high = std::max(high, ms / item.minMs);
        }

        Log(Channel::Sim, Level::Info,
            "式 {}({}): 見積もり ÷ 続けて積んだ測り {:.2f}(組ごと "
            "{:.2f}〜{:.2f})・"
            "見積もり ÷ 間を空けた測り {:.2f}・余裕を見込んだ上の値 ÷ 間を空けた測り {:.2f}",
            name, CostText(cost), predicted / actual, low, high, predicted / gap,
            predicted * (double)cost.marginPercent / 100.0 / gap);
    }

    // 出来事のセルの数の段ごとの平均(続けて積んだ・間を空けた・揺れ)
    void LogBuckets(const std::vector<MeasuredGroup>& measured, const sim::TimeTreeSolveCost& cost) {
        constexpr std::array<uint64_t, 6> BOUNDS = {512, 1024, 2048, 4096, 8192, UINT64_MAX};
        for (size_t bucket = 0; bucket < BOUNDS.size(); ++bucket) {
            const uint64_t lower = bucket == 0 ? 0 : BOUNDS[bucket - 1];
            uint64_t count = 0;
            double minMs = 0;
            double gapMs = 0;
            double predicted = 0;
            double wobble = 0;
            for (const MeasuredGroup& item : measured) {
                const uint64_t cells = item.group->system.nodes.size();
                if (cells <= lower || cells > BOUNDS[bucket])
                    continue;

                ++count;
                minMs += item.minMs;
                gapMs += item.gapMs;
                predicted += PredictMs(cost, *item.group);
                wobble = std::max(wobble, item.maxMs / item.minMs);
            }

            if (count == 0)
                continue;

            Log(Channel::Sim, Level::Info,
                "セル {}〜{}: 組 {}・続けて積んだ {:.4f} ms・間を空けた {:.4f} ms(÷ {:.2f})・起動時の式 {:.4f} ms・"
                "回の揺れの最大 {:.2f} 倍",
                lower + 1, BOUNDS[bucket] == UINT64_MAX ? 0 : BOUNDS[bucket], count, minMs / (double)count,
                gapMs / (double)count, gapMs / minMs, predicted / (double)count, wobble);
        }
    }

    std::expected<sim::TimeTreeCostProbe, std::string> RunProbe(Context& context, std::string_view name,
                                                                const sim::TimeTreeCostProbeOptions& options) {
        const double at = SecondsSince(context);
        auto probe = sim::ProbeTimeTreeSolveCost(context.device, *context.queue, *context.ring, context.frequency,
                                                 options);
        if (!probe)
            return std::unexpected(probe.error());

        Log(Channel::Sim, Level::Info,
            "起動時の測り {}(箱 {}・{}・段の多い箱 e {} = 段 {}・{:.1f} 秒の時点): 熱した {} 回 {:.1f} ms・小 V 1 {} "
            "ns・"
            "小 V 2 {} ns・大 V 1 {} ns・大 V 2 {} ns・段の多い大 V 1 {} ns・V 2 {} ns・GPU {:.2f} ms・全体 {:.1f} ms "
            "・選ぶ段 {} 単位 {} ns・{} 単位 {} ns → {}",
            name, options.smallSide, options.largeSide, options.levelExponent, probe->times.levels, at,
            probe->heatBatches, (double)probe->heatNs * 1.0e-6, probe->times.solveNs[0], probe->times.solveNs[1],
            probe->times.solveNs[2], probe->times.solveNs[3], probe->times.solveNs[4], probe->times.solveNs[5],
            (double)probe->gpuNs * 1.0e-6, (SecondsSince(context) - at) * 1000.0, probe->times.chooseUnits[0],
            probe->times.chooseNs[0], probe->times.chooseUnits[1], probe->times.chooseNs[1], CostText(probe->cost));

        return probe;
    }

    std::expected<void, std::string> PrepareGroups(Context& context, const std::vector<sim::TimeTreeEventGroup>& groups,
                                                   std::vector<MeasuredGroup>& measured) {
        for (const sim::TimeTreeEventGroup& group : groups) {
            MeasuredGroup item;
            item.group = &group;
            item.grid = SolveCpu(group.system, 1).grid;
            auto gpu = MakeFitted(context, group.system, item.grid, 1);
            if (!gpu)
                return std::unexpected(gpu.error());

            item.gpu.emplace(std::move(*gpu));
            if (group.cycles == 2) {
                auto built = MakeFitted(context, group.system, item.grid, 2);
                if (!built)
                    return std::unexpected(built.error());

                item.built.emplace(std::move(*built));
            }

            measured.push_back(std::move(item));
        }

        return {};
    }

    void LogTotals(const std::vector<MeasuredGroup>& measured, uint64_t events, uint64_t eventsTotal) {
        double minMs = 0;
        double gapMs = 0;
        std::map<uint32_t, std::array<double, 3>> byCycles;  // V → 数・続けて積んだ・V 2 で作った
        for (const MeasuredGroup& item : measured) {
            minMs += item.minMs;
            gapMs += item.gapMs;
            std::array<double, 3>& entry = byCycles[item.group->cycles];
            entry[0] += 1;
            entry[1] += item.minMs;
            entry[2] += item.builtMs;
        }

        const double total = eventsTotal != 0 ? (double)eventsTotal : (double)events;
        Log(Channel::Sim, Level::Info,
            "1 出来事の GPU の待ち: 続けて積んだ {:.4f} ms・間を空けた {:.4f} ms → 出来事 {:.0f} で 続けて積んだ "
            "{:.2f} 秒・"
            "間を空けた {:.2f} 秒",
            minMs / (double)events, gapMs / (double)events, total, minMs / (double)events * total * 1.0e-3,
            gapMs / (double)events * total * 1.0e-3);
        for (const auto& [cycles, entry] : byCycles) {
            Log(Channel::Sim, Level::Info, "V {} の組 {}: 続けて積んだ {:.4f} ms{}", cycles, entry[0],
                entry[1] / entry[0],
                cycles == 2 ? std::format("・部品を V 2 で作って 1 回 {:.4f} ms", entry[2] / entry[0]) : "");
        }
    }

    using NamedCost = std::pair<std::string, sim::TimeTreeSolveCost>;

    std::expected<void, std::string> MeasureContinuous(Context& context,
                                                       const std::vector<sim::TimeTreeEventGroup>& groups,
                                                       uint64_t events, uint64_t eventsTotal,
                                                       std::vector<NamedCost> probes) {
        std::vector<MeasuredGroup> measured;
        const auto prepared = PrepareGroups(context, groups, measured);
        if (!prepared)
            return prepared;

        // --- 間を空けた形(組ごとに CPU で待ってから 1 回)---
        for (MeasuredGroup& item : measured) {
            const auto ms = TimeSolveOnce(context, *item.gpu, item.grid, item.group->cycles);
            if (!ms)
                return std::unexpected(ms.error());

            item.gapMs = *ms;
        }

        const auto timed = TimeContinuous(context, measured);
        if (!timed)
            return timed;

        // --- 起動時の測りの変種(続けて積んだ直後 = 時計が高い・大きい箱 32^3・長い歩の形)---
        const std::vector<std::pair<std::string, sim::TimeTreeCostProbeOptions>> variants = {
            {"直後・熱さない", {.heatNs = 0}},
            {"直後", {}},
            {"直後・大きい箱 32", {.largeSide = 32}},
            {"直後・段の多い箱 e 14", {.levelExponent = 14}}};
        for (const auto& [name, options] : variants) {
            const auto probe = RunProbe(context, name, options);
            if (!probe)
                return std::unexpected(probe.error());

            probes.emplace_back(name, probe->cost);
        }

        LogFits(measured);
        LogPrediction("既定", sim::TimeTreeSolveCost{}, measured);
        for (const auto& [name, cost] : probes)
            LogPrediction(name, cost, measured);

        LogBuckets(measured, probes.empty() ? sim::TimeTreeSolveCost{} : probes.front().second);
        LogTotals(measured, events, eventsTotal);

        return {};
    }

    // --- 6. GPU の時計の揺れ(T-1140): 同じ系を間を空けて・続けて積んで・また間を空けて ---
    std::expected<void, std::string> MeasureClock(Context& context, const sim::TimeTreeEventGroup& group) {
        const sim::ImplicitGrid grid = SolveCpu(group.system, 1).grid;
        auto gpu = MakeFitted(context, group.system, grid, 1);
        if (!gpu)
            return std::unexpected(gpu.error());

        const auto gapSeries = [&](std::string_view name) -> std::expected<void, std::string> {
            std::string text;
            for (uint32_t index = 0; index < 30; ++index) {
                const auto ms = TimeSolveOnce(context, *gpu, grid, group.cycles);
                if (!ms)
                    return std::unexpected(ms.error());

                if (index % 5 == 0)
                    text += std::format(" {:.1f} 秒 {:.4f}", SecondsSince(context), *ms);
            }

            Log(Channel::Sim, Level::Info, "時計(組 {} セル・{}・{} ms ごと): {}", group.system.nodes.size(), name,
                GAP_MS, text);

            return {};
        };

        const auto before = gapSeries("間を空けた");
        if (!before)
            return before;

        const auto continuous = MeasureClockContinuous(context, *gpu, grid, group.cycles);
        if (!continuous)
            return continuous;

        return gapSeries("続けて積んだ後に間を空けた");
    }

    // 120 回の時刻印付きの解き、その間に 7 回ずつ時刻印なしの解き(GPU は休まない)
    std::expected<void, std::string> MeasureClockContinuous(Context& context, sim::GpuEquilibriumUpload& gpu,
                                                            const sim::ImplicitGrid& grid, uint32_t cycles) {
        constexpr uint32_t TIMED = 120;
        constexpr uint32_t BETWEEN = 7;
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        const uint64_t ring = context.ring->GpuAddress();
        context.ring->RecordBegin(list);
        if (!gpu.RecordUpload(list, grid))
            return std::unexpected("段の形が合わない");

        for (uint32_t index = 0; index < TIMED; ++index) {
            gpu.RecordTimestamp(list, 2 * index);
            bool recorded = gpu.RecordSolve(list, ring, cycles, true);
            gpu.RecordTimestamp(list, (2 * index) + 1);
            for (uint32_t extra = 0; extra < BETWEEN; ++extra)
                recorded = recorded && gpu.RecordSolve(list, ring, cycles, true);

            if (!recorded)
                return std::unexpected("解きを積めない");
        }

        gpu.RecordTimestampReadback(list, 2 * TIMED);
        gpu.RecordEnd(list);
        const auto executed = Execute(context, list);
        if (!executed)
            return std::unexpected(executed.error());

        gpu.ReleaseStaging();
        const std::vector<uint64_t> ticks = gpu.ReadTimestamps(2 * TIMED);
        if (ticks.size() != 2 * TIMED)
            return std::unexpected("時間を読めない");

        std::string text;
        double low = 1.0e9;
        double high = 0;
        for (uint32_t index = 0; index < TIMED; ++index) {
            const double ms = ToMs(context, ticks[2 * index], ticks[(2 * index) + 1]);
            if (index % 15 == 0)
                text += std::format(" {:.1f} ms 目 {:.4f}", ToMs(context, ticks[0], ticks[2 * index]), ms);

            if (index >= TIMED / 4) {
                low = std::min(low, ms);
                high = std::max(high, ms);
            }
        }

        Log(Channel::Sim, Level::Info, "時計(続けて積んだ・{} 回): {}・後ろ 3/4 の揺れ {:.4f}〜{:.4f} ms({:.2f} 倍)",
            TIMED * (BETWEEN + 1), text, low, high, high / low);

        return {};
    }

    struct EventTotals {
        uint64_t events = 0;
        uint64_t spans = 0;
        uint64_t groups = 0;
        uint64_t cells = 0;
        double spanMs = 0;   // 列ごとの系
        double groupMs = 0;  // 組ごと
        uint64_t longGroups = 0;
        double longMs = 0;       // V 2 の組(部品は V 1 で作り 2 回積む)
        double longBuiltMs = 0;  // 同じ組を部品を V 2 で作って 1 回
    };

    std::expected<void, std::string> CompareEvent(Context& context, std::span<const sim::TimeTreeEventSpan> spans,
                                                  const std::vector<sim::TimeTreeEventGroup>& groups) {
        for (const sim::TimeTreeEventGroup& group : groups) {
            std::vector<int64_t> each;
            for (const uint32_t index : group.spans) {
                const CpuSolve cpu = SolveCpu(spans[index].system, spans[index].cycles);
                each.insert(each.end(), cpu.values.begin(), cpu.values.end());
            }

            const CpuSolve merged = SolveCpu(group.system, group.cycles);
            auto gpu = sim::GpuEquilibriumUpload::Create(context.device, group.system, merged.grid);
            if (!gpu)
                return std::unexpected(gpu.error());

            const auto values = SolveGpu(context, *gpu, merged.grid, group.cycles);
            if (!values)
                return std::unexpected(values.error());

            const std::string difference = FirstDifference(each, *values);
            if (!difference.empty())
                return std::unexpected(std::format("組(列 {}・V {}・段 {}): {}", group.spans.size(), group.cycles,
                                                   group.levels, difference));
        }

        return {};
    }

    std::expected<void, std::string> MeasureEvent(Context& context, std::span<const sim::TimeTreeEventSpan> spans,
                                                  const std::vector<sim::TimeTreeEventGroup>& groups,
                                                  EventTotals& totals) {
        for (const sim::TimeTreeEventSpan& span : spans) {
            const auto ms = MeasureSystem(context, span.system, span.cycles);
            if (!ms)
                return std::unexpected(ms.error());

            totals.spanMs += *ms;
        }

        for (const sim::TimeTreeEventGroup& group : groups) {
            const auto ms = MeasureSystem(context, group.system, group.cycles);
            if (!ms)
                return std::unexpected(ms.error());

            totals.groupMs += *ms;
            if (group.cycles != 2)
                continue;

            const auto built = MeasureSystem(context, group.system, 2, {.cyclesPerRecord = 2});
            if (!built)
                return std::unexpected(built.error());

            ++totals.longGroups;
            totals.longMs += *ms;
            totals.longBuiltMs += *built;
        }

        return {};
    }

    std::expected<void, std::string> RunEvents(Context& context, const std::vector<test::TimeStepSpanRecord>& records,
                                               bool measure, uint64_t eventsTotal, bool continuous, bool clock) {
        std::map<uint32_t, std::vector<sim::TimeTreeEventSpan>> events;
        for (const test::TimeStepSpanRecord& record : records) {
            if (record.event != test::TimeStepSpanRecord::NO_EVENT)
                events[record.event].push_back(
                    {.system = record.system, .exponent = record.exponent, .cycles = record.cycles});
        }

        // --- 起動時の測り(プロセスの初め = GPU が休んでいる状態から。T-1152)---
        std::vector<NamedCost> probes;
        const std::vector<std::pair<std::string, sim::TimeTreeCostProbeOptions>> cold = {
            {"初め・熱さない", {.heatNs = 0}}, {"初め", {}}};
        for (const auto& [name, options] : cold) {
            if (!continuous)
                break;

            const auto probe = RunProbe(context, name, options);
            if (!probe)
                return std::unexpected(probe.error());

            probes.emplace_back(name, probe->cost);
        }

        EventTotals totals;
        std::vector<sim::TimeTreeEventGroup> allGroups;  // --continuous・--clock(T-1152)
        for (const auto& [event, spans] : events) {
            const std::vector<sim::TimeTreeEventGroup> groups = sim::GroupTimeTreeEvent(spans);
            const auto compared = CompareEvent(context, spans, groups);
            if (!compared)
                return std::unexpected(std::format("出来事 {}: {}", event, compared.error()));

            ++totals.events;
            totals.spans += spans.size();
            totals.groups += groups.size();
            for (const sim::TimeTreeEventSpan& span : spans)
                totals.cells += span.system.nodes.size();

            if (continuous || clock) {
                allGroups.insert(allGroups.end(), groups.begin(), groups.end());
                continue;
            }

            if (!measure)
                continue;

            const auto measured = MeasureEvent(context, spans, groups, totals);
            if (!measured)
                return std::unexpected(std::format("出来事 {}: {}", event, measured.error()));
        }

        Log(Channel::Sim, Level::Info, "出来事: {}(列 {}・組 {}・セル {})で組の値が列ごとの CPU とビット一致",
            totals.events, totals.spans, totals.groups, totals.cells);
        if (clock && !allGroups.empty()) {
            const auto largest = std::ranges::max_element(
                allGroups, {}, [](const sim::TimeTreeEventGroup& group) { return group.system.nodes.size(); });
            const auto clocked = MeasureClock(context, *largest);
            if (!clocked)
                return clocked;
        }

        if (continuous && !allGroups.empty())
            return MeasureContinuous(context, allGroups, totals.events, eventsTotal, std::move(probes));

        if (!measure || totals.events == 0)
            return {};

        const double sampled = (double)totals.events;
        const double total = eventsTotal != 0 ? (double)eventsTotal : sampled;
        Log(Channel::Sim, Level::Info,
            "出来事の費用: 1 出来事の平均 列ごと {:.4f} ms・組ごと {:.4f} ms(組 ÷ 列 {:.2f})→ 出来事 {:.0f} で 列ごと "
            "{:.2f} 秒・組ごと {:.2f} 秒",
            totals.spanMs / sampled, totals.groupMs / sampled, totals.groupMs / totals.spanMs, total,
            totals.spanMs / sampled * total * 1.0e-3, totals.groupMs / sampled * total * 1.0e-3);
        if (totals.longGroups != 0) {
            Log(Channel::Sim, Level::Info,
                "V 2 の組 {}: 部品を V 1 で作って 2 回積む {:.4f} ms・V 2 で作って 1 回 {:.4f} ms(1 組の平均)",
                totals.longGroups, totals.longMs / (double)totals.longGroups,
                totals.longBuiltMs / (double)totals.longGroups);
        }

        return {};
    }

    // --- 4. 段 0 の段だけの時間を 3 つの変種で(T-1141。128bit の割り算の分)---
    std::expected<void, std::string> StageVariants(Context& context,
                                                   const std::vector<test::TimeStepSpanRecord>& source, uint32_t tile) {
        constexpr std::array<const char*, 3> NAMES = {"本物", "右辺の項を割らない", "読むだけ"};
        for (size_t index = 0; index < source.size(); ++index) {
            const test::TimeStepSpanRecord record = tile > 1 ? Tile(source[index], tile) : source[index];
            std::array<double, 3> ms{};
            for (uint32_t variant = 0; variant < ms.size(); ++variant) {
                const CpuSolve cpu = SolveCpu(record.system, 1);
                auto gpu = sim::GpuEquilibriumUpload::Create(context.device, record.system, cpu.grid,
                                                             {.stageVariant = variant});
                if (!gpu)
                    return std::unexpected(gpu.error());

                // 段 0 の段だけ(e = 0 の系は段 1 つ = 粗い段の重みは無い)。暖機を兼ねて 2 回測った小さい方
                const auto first = TimeSolve(context, *gpu, cpu.grid, 0);
                const auto second = first ? TimeSolve(context, *gpu, cpu.grid, 0) : first;
                if (!second)
                    return std::unexpected(second.error());

                ms[variant] = std::min(*first, *second);
            }

            Log(Channel::Sim, Level::Info,
                "段 0 の段 {}: {} {:.4f} ms・{} {:.4f} ms・{} {:.4f} ms(割り算の分 {:.0f} %)", NameOf(record, index),
                NAMES[0], ms[0], NAMES[1], ms[1], NAMES[2], ms[2], ms[0] > 0 ? 100.0 * (ms[0] - ms[2]) / ms[0] : 0.0);
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::string spansPath;
        bool measure = false;
        uint32_t tile = 1;
        uint64_t eventsTotal = 0;
        bool stageVariants = false;
        bool continuous = false;
        bool clock = false;
        std::vector<char*> rest = {arguments[0]};
        for (size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--spans" && index + 1 < arguments.size())
                spansPath = arguments[++index];
            else if (argument == "--measure")
                measure = true;
            else if (argument == "--tile" && index + 1 < arguments.size())
                tile = std::max<uint32_t>((uint32_t)std::stoul(arguments[++index]), 1);
            else if (argument == "--events-total" && index + 1 < arguments.size())
                eventsTotal = std::stoull(arguments[++index]);
            else if (argument == "--stage-variants")
                stageVariants = true;
            else if (argument == "--continuous")
                continuous = true;
            else if (argument == "--clock")
                clock = true;
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
        const bool eventFile = records->front().event != test::TimeStepSpanRecord::NO_EVENT;
        std::expected<void, std::string> ran;
        if (eventFile)
            ran = RunEvents(context, *records, measure, eventsTotal, continuous, clock);
        else if (stageVariants)
            ran = StageVariants(context, *records, tile);
        else {
            ran = Compare(context, *records);
            if (ran && measure)
                ran = Measure(context, *records, tile);
        }

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
