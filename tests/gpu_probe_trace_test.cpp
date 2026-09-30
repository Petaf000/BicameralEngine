// gpu_probe_trace_test.cpp — 伝導の連鎖のトレース(T-0087、16 §1.3・§3)を確かめる。
//
// 確かめること:
//   - 同じ入力で 2 回走らせても、フレームへの分け方を変えても、並べたトレースが同じ(atomic の順によらない。ADR-0003)
//   - GPU のトレース(重なりを除いたもの)が CPU リファレンスの予想(sim/probe_trace の AppendExpectedProbeTrace)と一致する。
//     範囲(刻み・セルの箱)を狭めても一致する。容量を越えたら落とした数が出る
//   - 刻みごとの木としてファイルに書け、つつき(同じセルに 2 回 = 一覧に 2 度)と「前の刻みで変わった」根が読める
//   - わざと CPU リファレンスのつつきを 1 セルずらすと、ハッシュ列から最初に食い違った状態の刻みが、その刻みの GPU と CPU の
//     全部のセルから最初のブロックとセルが、トレースからずれの元の刻み(つつき)が出る
//   - debug layer のエラーが 0 件
// 引数: gpu_test_options.h(--warp)。キューは compute。
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/device.h"
#include "gpu/graph_trace.h"
#include "gpu/immediate_queue.h"
#include "gpu/queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "sim/probe_sim.h"
#include "sim/probe_trace.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)
using gpu::GraphTraceFilter;
using gpu::GraphTraceRecord;

namespace {

    constexpr uint64_t TOTAL_TICKS = 16;
    constexpr uint32_t TRACE_CAPACITY = 1u << 16;

    // わざとずらす試験: 刻み SHIFT_TICK のつつき(33, 32, 32)を、CPU リファレンスだけ(34, 32, 32)にする(同じブロックの隣のセル)
    constexpr uint64_t SHIFT_TICK = 9;
    constexpr std::array<uint32_t, 3> SHIFT_CELL_GPU = {33, 32, 32};
    constexpr std::array<uint32_t, 3> SHIFT_CELL_CPU = {34, 32, 32};

    // 同じセルに 2 回(一覧に同じブロックが 2 度入る)・ブロックの境目・格子の角・ずらす所
    struct PokeSpec {
        uint64_t tick;
        std::array<uint32_t, 3> cell;
    };

    constexpr std::array<PokeSpec, 5> POKES = {{
        {.tick = 0, .cell = {10, 10, 10}},
        {.tick = 0, .cell = {10, 10, 10}},
        {.tick = 3, .cell = {3, 4, 3}},
        {.tick = 6, .cell = {0, 0, 0}},
        {.tick = SHIFT_TICK, .cell = SHIFT_CELL_GPU},
    }};

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    // shifted なら SHIFT_TICK のつつきを SHIFT_CELL_CPU にする(CPU リファレンスだけに渡す)
    std::vector<ProbeCommand> MakeCommands(bool shifted) {
        std::vector<ProbeCommand> commands;
        uint32_t sequence = 0;
        for (const PokeSpec& poke : POKES) {
            const std::array<uint32_t, 3> cell = shifted && poke.tick == SHIFT_TICK ? SHIFT_CELL_CPU : poke.cell;
            commands.push_back(MakePokeCommand(poke.tick, sequence++, cell[0], cell[1], cell[2]));
        }

        return commands;
    }

    GraphTraceFilter WholeFilter() {
        return ProbeTraceFilterForCells(0, TOTAL_TICKS, {0, 0, 0}, {PROBE_GRID_SIZE, PROBE_GRID_SIZE, PROBE_GRID_SIZE},
                                        TRACE_CAPACITY);
    }

    // --- CPU リファレンス ---

    struct CpuRun {
        std::vector<ProbeTickHash> hashes;    // S(1)〜S(ticks)
        std::vector<GraphTraceRecord> trace;  // 予想(並べて重なりなし)
        std::vector<uint32_t> cells;          // S(ticks) の全部のセル
    };

    CpuRun RunReference(std::span<const ProbeCommand> commands, const GraphTraceFilter& filter, uint64_t ticks) {
        ProbeReference reference;
        CpuRun result;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const std::vector<uint8_t> changedBefore(reference.ChangedBlocks().begin(),
                                                     reference.ChangedBlocks().end());
            reference.Advance(tick, commands);
            AppendExpectedProbeTrace(filter, tick, commands, changedBefore, reference.ChangedBlocks(), result.trace);

            const std::span<const uint32_t> state = reference.State(tick + 1);
            result.hashes.push_back({.tick = tick + 1,
                                     .hash = ProbeStateHash(state),
                                     .heat = ProbeHeatSum(state),
                                     .scheduledBlocks = reference.ScheduledBlocks()});
        }

        result.trace = UniqueProbeTrace(std::move(result.trace));
        const std::span<const uint32_t> last = reference.State(ticks);
        result.cells.assign(last.begin(), last.end());

        return result;
    }

    // --- GPU ---

    struct GpuRun {
        bool ok = false;
        std::vector<ProbeTickHash> hashes;
        std::vector<GraphTraceRecord> trace;  // 並べたもの(重なりは残す)
        uint32_t droppedTraceCount = 0;
        std::vector<uint32_t> cells;  // 最後の抽出のセル(最後のフレームが刻みの境界で終わるとき S(刻みの数))
    };

    // 抽出(COMMON)のセルの部分を読み戻す
    std::vector<uint32_t> ReadExtractionCells(ID3D12Device5* device, ID3D12Resource* extraction) {
        std::vector<uint32_t> cells(PROBE_CELL_COUNT);
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, uint64_t{PROBE_CELL_COUNT} * 4,
                                                                  gpu::BufferKind::Readback);
        if (!queue || !readback)
            return {};

        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr)
            return {};

        list->CopyBufferRegion(readback.Get(), 0, extraction, 0, uint64_t{PROBE_CELL_COUNT} * 4);
        if (!queue->ExecuteAndWait() || !gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(std::span(cells))))
            return {};

        return cells;
    }

    // unitsPerFrame の分け方で走らせる(コマンドは最初のフレームで全部足す。キューの中で自分の刻みまで待つ)
    GpuRun RunGpu(ID3D12Device5* device, const GraphTraceFilter& filter, std::span<const uint32_t> unitsPerFrame,
                  std::span<const ProbeCommand> commands) {
        GpuRun result;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"TraceTestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, {.trace = filter});
        if (!queue || !simulation) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error());
            return result;
        }

        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        uint64_t unitPosition = 0;
        uint32_t extractionTarget = 0;
        for (size_t frame = 0; frame < unitsPerFrame.size(); ++frame) {
            const auto slot = static_cast<uint32_t>(frame % ProbeSim::FRAME_SLOT_COUNT);
            extractionTarget = static_cast<uint32_t>(frame % PROBE_EXTRACTION_COUNT);
            ID3D12CommandList* list = simulation->RecordFrame(
                slot, {.firstTick = unitPosition / unitsPerTick,
                       .firstUnit = static_cast<uint32_t>(unitPosition % unitsPerTick),
                       .unitCount = unitsPerFrame[frame],
                       .extract = true,
                       .extractionTarget = extractionTarget,
                       .commands = frame == 0 ? commands : std::span<const ProbeCommand>()});

            if (list == nullptr || !queue->WaitCpu(queue->Submit(list)))
                return result;

            ProbeFrameReadback readback = simulation->ReadFrame(slot);
            result.hashes.insert(result.hashes.end(), readback.hashes.begin(), readback.hashes.end());
            result.trace.insert(result.trace.end(), readback.trace.begin(), readback.trace.end());
            result.droppedTraceCount += readback.droppedTraceCount;
            unitPosition += unitsPerFrame[frame];
        }

        gpu::SortGraphTrace(result.trace);
        result.cells = ReadExtractionCells(device, simulation->Extraction(extractionTarget));
        result.ok = unitPosition % unitsPerTick == 0 && result.cells.size() == PROBE_CELL_COUNT;

        return result;
    }

    // --- 分け方 ---

    std::vector<uint32_t> TickFrames(uint64_t ticks) {
        return std::vector<uint32_t>(ticks, PROBE_FIXED_UNITS_PER_TICK);
    }

    // 刻みの途中で切れるばらばらの分け方(合計は TOTAL_TICKS 刻みぶん)
    std::vector<uint32_t> MixedFrames() {
        constexpr std::array<uint32_t, 5> PATTERN = {1, 5, 2, 7, 3};
        const auto total = static_cast<uint32_t>(TOTAL_TICKS * PROBE_FIXED_UNITS_PER_TICK);
        std::vector<uint32_t> sizes;
        uint32_t sum = 0;
        for (size_t index = 0; sum < total; ++index) {
            sizes.push_back(std::min(PATTERN[index % PATTERN.size()], total - sum));
            sum += sizes.back();
        }

        return sizes;
    }

    bool HashesMatch(std::span<const ProbeTickHash> gpu, std::span<const ProbeTickHash> cpu) {
        return gpu.size() == cpu.size() && !FirstDivergentTick(gpu, cpu).has_value();
    }

    // GPU(重なりを除く)と CPU の予想が一致するか。違えば最初の食い違いをログへ
    bool TraceMatchesReference(const GpuRun& gpu, const CpuRun& cpu) {
        const std::vector<GraphTraceRecord> unique = UniqueProbeTrace(gpu.trace);
        const auto mismatch = FirstProbeTraceMismatch(unique, cpu.trace);
        if (mismatch)
            Log(Channel::Sim, Level::Error, "  {}(GPU {} 件 / CPU {} 件)", *mismatch, unique.size(), cpu.trace.size());

        return !mismatch.has_value();
    }

    std::string ReadWholeFile(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    // --- 試験 ---

    // 同じ入力で 2 回・分け方を変えて、トレースが同じ。CPU リファレンスの予想と一致。木としてファイルに書ける
    void TestDeterminism(ID3D12Device5* device, Failures& failures) {
        const std::vector<ProbeCommand> commands = MakeCommands(false);
        const CpuRun expected = RunReference(commands, WholeFilter(), TOTAL_TICKS);

        const GpuRun first = RunGpu(device, WholeFilter(), TickFrames(TOTAL_TICKS), commands);
        const GpuRun second = RunGpu(device, WholeFilter(), TickFrames(TOTAL_TICKS), commands);
        const GpuRun mixed = RunGpu(device, WholeFilter(), MixedFrames(), commands);
        Log(Channel::Sim, Level::Info,
            "トレース: 1 回目 {} 件・2 回目 {} 件・ばらばら {} 件(CPU の予想 {} 件、重なりなし)", first.trace.size(),
            second.trace.size(), mixed.trace.size(), expected.trace.size());

        failures.Check(first.ok && second.ok && mixed.ok, "走らせられた");
        failures.Check(HashesMatch(first.hashes, expected.hashes) && HashesMatch(mixed.hashes, expected.hashes),
                       "ハッシュ列が CPU リファレンスと一致");
        failures.Check(!first.trace.empty() && first.droppedTraceCount == 0 && second.droppedTraceCount == 0 &&
                           mixed.droppedTraceCount == 0,
                       "トレースが書けて、落とした記録が無い");
        failures.Check(first.trace == second.trace, "同じ入力で 2 回走らせてトレースが同じ");
        failures.Check(first.trace == mixed.trace, "フレームの分け方を変えてもトレースが同じ");
        failures.Check(TraceMatchesReference(first, expected), "トレースが CPU リファレンスの予想と一致");

        // --- 刻みごとの木のファイル ---
        const fs::path path = fs::temp_directory_path() / "bicameral_probe_trace_test.txt";
        const auto written = WriteProbeTraceFile(path, first.trace, WholeFilter(), first.droppedTraceCount);
        const std::string text = written ? ReadWholeFile(path) : std::string();
        const size_t firstTickEnd = text.find("刻み 1:");
        Log(Channel::Sim, Level::Info, "木の始め:\n{}", text.substr(0, std::min<size_t>(firstTickEnd, 1200)));

        failures.Check(written.has_value() && text.starts_with("# 伝導の連鎖のトレース"), "木をファイルに書けた");
        failures.Check(
            text.contains("刻み 0: 根 1・") && text.contains("つつき セル (10, 10, 10) セル (10, 10, 10) ×2"),
            "木: 刻み 0 の根は同じセルを 2 回つついたブロック 1 つ(一覧に 2 度)");
        failures.Check(text.contains("刻み 0 で変わった") && text.contains("変わらない"),
                       "木: 変わったブロックが次の刻みの根になり、変わらない子も読める");
        fs::remove(path);
    }

    // 範囲を狭める(刻み・セルの箱)・容量を越える
    void TestFilters(ID3D12Device5* device, Failures& failures) {
        const std::vector<ProbeCommand> commands = MakeCommands(false);
        const GraphTraceFilter narrow = ProbeTraceFilterForCells(2, 8, {0, 0, 0}, {12, 12, 12}, TRACE_CAPACITY);
        const CpuRun expected = RunReference(commands, narrow, 10);
        const GpuRun run = RunGpu(device, narrow, TickFrames(10), commands);
        const bool inTicks = rng::all_of(
            run.trace, [](const GraphTraceRecord& record) { return record.tick >= 2 && record.tick < 8; });

        Log(Channel::Sim, Level::Info, "狭い範囲(刻み [2, 8)・セル [0, 12)³ = ブロック [0, 3)³): {} 件",
            run.trace.size());
        failures.Check(run.ok && !run.trace.empty() && inTicks && run.droppedTraceCount == 0,
                       "狭い範囲: 範囲の刻みだけが書かれた");
        failures.Check(TraceMatchesReference(run, expected), "狭い範囲: CPU リファレンスの予想と一致");

        // 容量 8 件 / フレーム: 書けた分は 8 件まで、残りは落とした数へ
        GraphTraceFilter tiny = WholeFilter();
        tiny.capacity = 8;
        const GpuRun overflow = RunGpu(device, tiny, TickFrames(2), commands);
        Log(Channel::Sim, Level::Info, "容量 8: 書けた {} 件・落とした {} 件", overflow.trace.size(),
            overflow.droppedTraceCount);
        failures.Check(overflow.ok && overflow.trace.size() == 16 && overflow.droppedTraceCount > 0,
                       "容量を越えたら 1 フレーム 8 件だけ書き、落とした数を数える");
    }

    // わざと CPU リファレンスのつつきを 1 セルずらす → 最初の刻み・ブロック・セル
    void TestDivergence(ID3D12Device5* device, Failures& failures) {
        const std::vector<ProbeCommand> gpuCommands = MakeCommands(false);
        const std::vector<ProbeCommand> cpuCommands = MakeCommands(true);
        const CpuRun shifted = RunReference(cpuCommands, WholeFilter(), TOTAL_TICKS);
        const GpuRun run = RunGpu(device, WholeFilter(), TickFrames(TOTAL_TICKS), gpuCommands);

        // (1) ハッシュ列 → 最初に食い違った状態 S(t)
        const auto tick = FirstDivergentTick(run.hashes, shifted.hashes);
        failures.Check(tick == SHIFT_TICK + 1, std::format("最初に食い違った状態は S({})", SHIFT_TICK + 1));
        if (!tick)
            return;

        // (2) その刻みまで GPU を走らせ直し(決定的なので同じ)、全部のセルを CPU と比べる → 最初のブロックとセル
        const GpuRun upTo = RunGpu(device, WholeFilter(), TickFrames(*tick), gpuCommands);
        const CpuRun cpuUpTo = RunReference(cpuCommands, WholeFilter(), *tick);
        const auto divergence = FindCellDivergence(*tick, upTo.cells, cpuUpTo.cells);
        if (divergence)
            Log(Channel::Sim, Level::Info, "{}", FormatProbeDivergence(*divergence));

        // GPU は (33, 32, 32) の熱が z − 1 の (33, 32, 31) へ流れ、CPU には流れない。ブロック (8, 8, 7) が番号で最初
        const uint32_t expectedBlock = ProbeBlockOfCell(33, 32, 31);
        failures.Check(upTo.ok && divergence.has_value() && divergence->block == expectedBlock &&
                           divergence->cell == std::array<uint32_t, 3>{33, 32, 31} &&
                           divergence->gpuValue == (PROBE_POKE_AMOUNT >> PROBE_CONDUCT_SHIFT) &&
                           divergence->cpuValue == 0,
                       "最初に食い違ったブロックとセル(8, 8, 7)の (33, 32, 31)");

        // (3) トレース → ずれの元(刻み 9 のつつきのセルが違う)
        const auto mismatch = FirstProbeTraceMismatch(UniqueProbeTrace(run.trace), shifted.trace);
        if (mismatch)
            Log(Channel::Sim, Level::Info, "{}", *mismatch);

        failures.Check(mismatch.has_value() && mismatch->contains(std::format("刻み {} つつき", SHIFT_TICK)),
                       "トレースの最初の食い違いは、ずらした刻みのつつき");
    }

}  // namespace

int main(int argc, char** argv) {
    const auto options = test::ParseGpuTestOptions(std::span(argv, static_cast<size_t>(argc)));
    if (!options) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_trace_test [--warp]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter);
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Failures failures;
    TestDeterminism(device->Get(), failures);
    TestFilters(device->Get(), failures);
    TestDivergence(device->Get(), failures);

    const bool passesValidation = test::PassesValidation(*device, "gpu_probe_trace_test");
    const bool passed = failures.count == 0 && passesValidation;
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_trace_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
