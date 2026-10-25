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
// 引数: --spans <ファイル> [--measure] [--tile n(費用の系を n 個並べる)] [--events-total n] [--stage-variants] と
//       GPU の試験の共通の引数(--warp・--queue・--no-gbv)。
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
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
                                               bool measure, uint64_t eventsTotal) {
        std::map<uint32_t, std::vector<sim::TimeTreeEventSpan>> events;
        for (const test::TimeStepSpanRecord& record : records) {
            if (record.event != test::TimeStepSpanRecord::NO_EVENT)
                events[record.event].push_back(
                    {.system = record.system, .exponent = record.exponent, .cycles = record.cycles});
        }

        EventTotals totals;
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

            if (!measure)
                continue;

            const auto measured = MeasureEvent(context, spans, groups, totals);
            if (!measured)
                return std::unexpected(std::format("出来事 {}: {}", event, measured.error()));
        }

        Log(Channel::Sim, Level::Info, "出来事: {}(列 {}・組 {}・セル {})で組の値が列ごとの CPU とビット一致",
            totals.events, totals.spans, totals.groups, totals.cells);
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
            ran = RunEvents(context, *records, measure, eventsTotal);
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
