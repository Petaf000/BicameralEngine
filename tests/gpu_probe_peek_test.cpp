// gpu_probe_peek_test.cpp — 覗き窓(sim/probe_peek。T-0096)の GPU と CPU リファレンスのビット一致と、「見るだけなら変わらない」。
//
// 確かめること(木箱の壁に火をつけ、フレームごとに数刻み進めて毎フレーム抽出する):
//   - 覗く・刻みの無いフレーム(引き戻すだけ)・別の所へ移る・やめる・もう一度覗く、の各フレームで、
//     GPU の入れ子(世界の写しと影の鎖の見出し・セル・端数・数える欄)と抽出の覗きの欄が CPU リファレンスとビット一致
//   - 覗きながら走らせた世界の刻みごとのハッシュ列が、覗かずに走らせたものと一致(D-403)
//   - 影の鎖が 9 段ある。子どうしが違う家族の数(影の中で細部が動いたか)はログに出すだけ: この場面の点のセルは 4000 K で
//     反応が反応物の量で頭打ちになり(確率的な丸めの差が出ない)、周りは冷たくて反応しないので 0 になる(細部の動きは T-0017 の 600 K で確認済み)
//   - debug layer のエラーとシェーダーの assert が 0 件
// 引数: gpu_test_options.h(--warp は NuGet の WARP。T-0097)
#include <algorithm>
#include <format>
#include <span>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "sim/probe_peek.h"
#include "sim/probe_sim.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::sim;  // probe_sim.hlsli の定数(PROBE_*)

namespace {

    constexpr PeekCell IGNITION_CELL = {.x = 28, .y = 32, .z = 32};  // 木箱の壁(sim/probe_sim.cpp の初めの世界)
    constexpr PeekCell SECOND_CELL = {.x = 29, .y = 31, .z = 30};    // 移る先(同じ 8³ のブロックの別の八分の一)

    // フレームごとの操作
    enum class PeekAction : uint8_t { None, Look, LookSecond, Stop };

    struct FramePlan {
        uint32_t ticks = 0;  // このフレームで進める刻み(0 なら抽出だけ)
        PeekAction action = PeekAction::None;
    };

    // 火をつける前の S(0) から覗き始める(覗き始めの後に世界が大きく変わっても、影が親に従うことを見る)
    constexpr std::array<FramePlan, 15> FRAMES = {{
        {.ticks = 0, .action = PeekAction::Look},  // S(0) を写して細かくする
        {.ticks = 1},                              // 刻む → 引き戻す
        {.ticks = 1},
        {.ticks = 1},
        {.ticks = 2},
        {.ticks = 0},  // 刻みの無いフレーム: 引き戻すだけ
        {.ticks = 1},
        {.ticks = 2, .action = PeekAction::LookSecond},  // 移る(捨てて作り直す)
        {.ticks = 2},
        {.ticks = 2, .action = PeekAction::Stop},  // やめる
        {.ticks = 2},
        {.ticks = 2, .action = PeekAction::Look},  // もう一度
        {.ticks = 3},
        {.ticks = 3},
        {.ticks = 3},
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

    template <typename T>
    std::vector<T> ReadBack(ID3D12Device5* device, ID3D12Resource* source, uint64_t offsetBytes, size_t count) {
        std::vector<T> values(count);
        const uint64_t bytes = count * sizeof(T);
        auto queue = gpu::ImmediateQueue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const ComPtr<ID3D12Resource> readback = gpu::CreateBuffer(device, bytes, gpu::BufferKind::Readback);
        if (!queue || !readback)
            return {};

        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr)
            return {};

        list->CopyBufferRegion(readback.Get(), 0, source, offsetBytes, bytes);
        if (!queue->ExecuteAndWait() || !gpu::ReadBuffer(readback.Get(), std::as_writable_bytes(std::span(values))))
            return {};

        return values;
    }

    void ApplyAction(PeekAction action, ProbePeek* gpu, ProbePeekReference* cpu) {
        const auto apply = [action](auto& peek) {
            if (action == PeekAction::Look)
                peek.Look(IGNITION_CELL);
            else if (action == PeekAction::LookSecond)
                peek.Look(SECOND_CELL);
            else if (action == PeekAction::Stop)
                peek.Stop();
        };

        if (gpu != nullptr)
            apply(*gpu);

        if (cpu != nullptr)
            apply(*cpu);
    }

    // 抽出の覗きの欄のうち、書いたはずの語(段の数・段の見出し・段のセル)が同じか
    bool SameExtraction(std::span<const uint32_t> gpu, std::span<const uint32_t> cpu) {
        if (gpu.size() != cpu.size() || gpu.empty() || gpu[0] != cpu[0])
            return false;

        const uint32_t levelCount = cpu[0];
        const size_t headerWords = PROBE_PEEK_LEVEL_WORDS * size_t{levelCount};
        const size_t cellWords = size_t{levelCount} * PROBE_PEEK_BLOCK_CELLS * PROBE_EXTRACTION_CELL_WORDS;

        return rng::equal(gpu.subspan(PROBE_PEEK_LEVEL_WORDS, headerWords),
                          cpu.subspan(PROBE_PEEK_LEVEL_WORDS, headerWords)) &&
               rng::equal(gpu.subspan(PROBE_PEEK_HEADER_WORDS, cellWords),
                          cpu.subspan(PROBE_PEEK_HEADER_WORDS, cellWords));
    }

    // 影の鎖の家族(親のセル 1 つの子 2³)のうち、子どうしが同じでないものの数(影の中で細部が動いている。
    // 細かくした直後と、反応の無い所では 0。引き戻しは子の比を保つので、違いは影の刻みの反応(確率的な丸め)から生まれる)
    bool FamilyHasDetail(const MultiresNest& nest, uint32_t slot, uint32_t local) {
        const uint64_t eldest = HashReactionCell(LoadNestCell(nest, slot, multires::MrChildCell(local, 0)));
        for (uint32_t j = 1; j < multires::MR_CHILDREN_PER_CELL; ++j) {
            if (HashReactionCell(LoadNestCell(nest, slot, multires::MrChildCell(local, j))) != eldest)
                return true;
        }

        return false;
    }

    uint32_t CountDetailedFamilies(const MultiresNest& nest) {
        uint32_t count = 0;
        for (uint32_t level = 0; level < PEEK_LEVEL_COUNT; ++level) {
            const uint32_t slot = PEEK_FIRST_SHADOW_SLOT + level;
            for (uint32_t local = 0; local < multires::MR_OCTANT_CELLS; ++local)
                count += FamilyHasDetail(nest, slot, local) ? 1 : 0;
        }

        return count;
    }

    struct PeekRun {
        bool ok = false;
        std::vector<ProbeTickHash> hashes;
        std::vector<double> frameMicroseconds;  // フレームのリスト全体の GPU 時間(覗いた分を比べる)
        uint32_t mismatchedFrames = 0;          // 入れ子か抽出が CPU と違ったフレーム
        uint32_t peekedFrames = 0;              // 段が 9 あったフレーム
        uint32_t detailedFamilies = 0;          // 子どうしが違う家族の数の最大
        uint32_t debugAssertCount = 0;
        std::array<uint32_t, multires::MR_COUNTER_COUNT> counters{};
    };

    struct GpuPeekSide {
        ID3D12Device5* device = nullptr;
        ID3D12Resource* extraction = nullptr;  // このフレームの抽出
        const ProbePeek* gpu = nullptr;
    };

    // 1 フレームぶん、GPU の入れ子と抽出の覗きの欄を CPU と比べて run に数える。読み戻せなければ false
    bool CompareFrame(PeekRun& run, const GpuPeekSide& side, const ProbePeekReference& cpuPeek, uint32_t frame,
                      uint64_t tick) {
        MultiresNest gpuNest;
        const std::vector<uint32_t> gpuExtraction = ReadBack<uint32_t>(
            side.device, side.extraction, uint64_t{PROBE_EXTRACTION_PEEK_OFFSET} * 4, PROBE_EXTRACTION_PEEK_WORDS);
        if (!side.gpu->Read(gpuNest) || gpuExtraction.empty())
            return false;

        const bool sameNest = HashWholeNest(gpuNest) == HashWholeNest(cpuPeek.Nest()) &&
                              gpuNest.counters == cpuPeek.Nest().counters;
        const bool sameExtraction = SameExtraction(gpuExtraction, cpuPeek.Extraction());
        run.mismatchedFrames += sameNest && sameExtraction ? 0 : 1;
        const bool peeking = gpuExtraction[0] == PEEK_LEVEL_COUNT;
        run.peekedFrames += peeking ? 1 : 0;
        const uint32_t detailed = peeking ? CountDetailedFamilies(gpuNest) : 0;
        run.detailedFamilies = std::max(run.detailedFamilies, detailed);
        run.counters = gpuNest.counters;
        Log(Channel::Sim, Level::Info,
            "  フレーム {:>2}: 刻み {:>2}  段 {}  入れ子 {:016x}  CPU と {}{}  細部のある家族 {}", frame, tick,
            gpuExtraction[0], HashWholeNest(gpuNest), sameNest ? "一致" : "不一致",
            sameExtraction ? "" : "(抽出が不一致)", detailed);

        return true;
    }

    void AccumulateReadback(PeekRun& run, const ProbeFrameReadback& readback, double usPerTimestamp) {
        run.hashes.insert(run.hashes.end(), readback.hashes.begin(), readback.hashes.end());
        run.debugAssertCount += readback.debugAssertCount;
        run.frameMicroseconds.push_back(static_cast<double>(readback.gpuEndTimestamp - readback.gpuBeginTimestamp) *
                                        usPerTimestamp);
    }

    PeekRun RunPeek(ID3D12Device5* device, const BakedReactionTable& table, bool peek) {
        PeekRun run;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"PeekTestSim");
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table);
        auto gpuPeek = ProbePeek::Create(device, table);
        if (!queue || !simulation || !gpuPeek) {
            Log(Channel::Sim, Level::Error, "作れない: {}{}{}", queue ? "" : queue.error(),
                simulation ? "" : simulation.error(), gpuPeek ? "" : gpuPeek.error());
            return run;
        }

        ProbeReference reference(table);
        ProbePeekReference cpuPeek(table);
        const std::array<ProbeCommand, 1> ignition = {
            MakePokeCommand(0, 0, IGNITION_CELL.x, IGNITION_CELL.y, IGNITION_CELL.z)};
        const uint32_t unitsPerTick = simulation->UnitsPerTick();
        const double usPerTimestamp = 1'000'000.0 / static_cast<double>(queue->TimestampFrequency());
        uint64_t tick = 0;

        // 抽出の後ろに覗き窓と入れ子の読み戻しを(同じリスト)
        const auto peekHook = [&gpuPeek](ID3D12GraphicsCommandList10* list, const ProbeExtractContext& context) {
            gpuPeek->RecordAfterExtract(list, context);
            gpuPeek->RecordReadback(list);
        };

        for (uint32_t frame = 0; frame < FRAMES.size(); ++frame) {
            const FramePlan& plan = FRAMES[frame];
            const auto slot = static_cast<uint32_t>(frame % ProbeSim::FRAME_SLOT_COUNT);
            const auto target = static_cast<uint32_t>(frame % PROBE_EXTRACTION_COUNT);
            if (peek)
                ApplyAction(plan.action, &*gpuPeek, &cpuPeek);

            // --- GPU: 世界を進めて抽出 → 覗き窓 → 入れ子を読み戻す(同じリスト)---
            ProbeFrameInput input{
                .firstTick = tick,
                .unitCount = plan.ticks * unitsPerTick,
                .extract = true,
                .extractionTarget = target,
                .commands = frame == 0 ? std::span<const ProbeCommand>(ignition) : std::span<const ProbeCommand>()};
            if (peek)
                input.afterExtract = peekHook;

            ID3D12CommandList* list = simulation->RecordFrame(slot, input);
            if (list == nullptr || !queue->WaitCpu(queue->Submit(list)))
                return run;

            AccumulateReadback(run, simulation->ReadFrame(slot), usPerTimestamp);

            // --- CPU: 同じ刻みだけ進めて、同じ境界で覗く ---
            for (uint32_t i = 0; i < plan.ticks; ++i, ++tick)
                reference.Advance(
                    tick, tick == 0 ? std::span<const ProbeCommand>(ignition) : std::span<const ProbeCommand>());

            if (!peek)
                continue;

            cpuPeek.Advance(reference.State(tick), tick);

            if (!CompareFrame(run, {.device = device, .extraction = simulation->Extraction(target), .gpu = &*gpuPeek},
                              cpuPeek, frame, tick))
                return run;
        }

        run.ok = run.hashes.size() == tick;

        return run;
    }

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> arguments(argv, argv + argc);
    const auto options = test::ParseGpuTestOptions(std::span(arguments));
    const auto table = BakeReactionTable(MakeCombustionTestTable());
    if (!options || !table) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_probe_peek_test [--warp]");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    Failures failures;
    Log(Channel::Sim, Level::Info, "木箱の壁 ({}, {}, {}) に火をつけ、覗きながら {} フレーム", IGNITION_CELL.x,
        IGNITION_CELL.y, IGNITION_CELL.z, FRAMES.size());
    const PeekRun peeked = RunPeek(device->Get(), *table, true);
    const PeekRun plain = RunPeek(device->Get(), *table, false);

    // 覗いて刻んだフレーム(段が 9 で、細かくした直後でない)の、覗かない時との GPU 時間の差の平均(入れ子の読み戻しを含む)
    double extraMicroseconds = 0.0;
    uint32_t steppedFrames = 0;
    for (size_t frame = 0;
         frame < FRAMES.size() && frame < peeked.frameMicroseconds.size() && frame < plain.frameMicroseconds.size();
         ++frame) {
        const bool stepped = FRAMES[frame].action == PeekAction::None && FRAMES[frame].ticks > 0 &&
                             FRAMES[frame - 1].action != PeekAction::Stop;  // やめた後は覗いていない
        if (!stepped)
            continue;

        extraMicroseconds += peeked.frameMicroseconds[frame] - plain.frameMicroseconds[frame];
        ++steppedFrames;
    }

    Log(Channel::Sim, Level::Info, "覗き窓の GPU 時間(刻む → 引き戻す → 抽出、{} フレームの平均): {:.0f} µs",
        steppedFrames, extraMicroseconds / std::max(1u, steppedFrames));

    const bool sameHashes = peeked.ok && plain.ok &&
                            rng::equal(peeked.hashes, plain.hashes, {}, &ProbeTickHash::hash, &ProbeTickHash::hash);
    Log(Channel::Sim, Level::Info, "覗いた {} 刻み: S = {:016x}  覗かない: {:016x}  数える欄 {} {} {} {}",
        peeked.hashes.size(), peeked.hashes.empty() ? 0 : peeked.hashes.back().hash,
        plain.hashes.empty() ? 0 : plain.hashes.back().hash, peeked.counters[0], peeked.counters[1], peeked.counters[2],
        peeked.counters[3]);

    failures.Check(peeked.ok && plain.ok, "走らせられた");
    failures.Check(peeked.mismatchedFrames == 0, "毎フレーム、入れ子と抽出の覗きの欄が CPU とビット一致");
    failures.Check(sameHashes, "覗いても世界の刻みごとのハッシュ列が覗かない時と同じ");
    failures.Check(peeked.peekedFrames >= 9, std::format("段が 9 あったフレームが 9 以上({})", peeked.peekedFrames));
    failures.Check(peeked.debugAssertCount == 0 && plain.debugAssertCount == 0, "シェーダーの assert が 0 件");

    const bool passed = failures.count == 0 && test::PassesValidation(*device, "gpu_probe_peek_test");
    Log(Channel::Sim, passed ? Level::Info : Level::Error, "gpu_probe_peek_test({}): {}",
        gpu::AdapterKindName(options->adapter), passed ? "OK" : "FAILED");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
