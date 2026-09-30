// gpu_debug_ring_test.cpp — シェーダーの printf / assert のリング(T-0003、docs/design/16-debug-test.md §1)を確かめる。
//   1. DecodeDebugRecord(GPU なし): 4 つの型の引数・書式の指定({:#x})・知らない書式の番号・書式に足りない引数
//   2. compute(shaders/sim/debug_ring_probe.hlsl): 64 スレッドの print と assert 1 件が、CPU で作った文字列と一致するか
//   3. 溢れ: 容量より多く書かせて、落とした数を正しく数えるか
//   4. 空に戻るか: 3 の後にもう一度 2 を走らせて、前の分が残っていないか
//   5. Work Graphs(shaders/sim/debug_ring_graph_probe.hlsl): Leaf ノードの print が 1〜256 を 1 回ずつ出すか
// 引数は gpu_test_options.h。
#include <algorithm>
#include <array>
#include <initializer_list>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu/work_graph.h"
#include "gpu_test_options.h"

using namespace bicameral;

namespace {

    constexpr uint32_t THREADS_PER_GROUP = 64;     // debug_ring_probe.hlsl の numthreads
    constexpr uint32_t ASSERT_THREAD = 5;          // debug_ring_probe.hlsl で assert するスレッド
    constexpr uint32_t GRAPH_LEAF_COUNT = 4 * 64;  // debug_ring_graph_probe.hlsl の ROOT_GROUP_COUNT × numthreads
    constexpr uint32_t PROBE_THREAD_COUNT = 64;
    constexpr uint32_t OVERFLOW_THREAD_COUNT = DEBUG_RING_CAPACITY + 64;
    constexpr uint32_t MAX_LOGGED_MESSAGES = 4;
    constexpr gpu::RootSignatureLayout COMPUTE_LAYOUT{.rootConstantCount = 1, .debugRing = true};
    constexpr gpu::RootSignatureLayout GRAPH_LAYOUT{.debugRing = true};

    struct Failures {
        int count = 0;

        void Check(bool condition, string_view what) {
            if (condition)
                return;

            Log(Channel::Gpu, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    // debug_ring_probe.hlsl と同じ式で、スレッドの print の文字列を作る
    std::string ExpectedProbeText(uint32_t thread) {
        const int32_t negative = -static_cast<int32_t>(thread) - 1;
        const uint64_t large = (uint64_t{thread} << 40) | 0xABCDu;
        const int64_t signed64 = -(static_cast<int64_t>(thread) << 33) - 7;

        return format("thread={} negative={} large={:#x} signed64={}", thread, negative, large, signed64);
    }

    // --- 1. デコード(GPU なし)---

    struct TestArgument {
        uint64_t bits;
        uint32_t kind;
    };

    std::array<uint32_t, DEBUG_RECORD_WORDS> MakeRecord(uint32_t formatIndex, uint32_t kind,
                                                        std::initializer_list<TestArgument> arguments) {
        std::array<uint32_t, DEBUG_RECORD_WORDS> record{};
        uint32_t argKinds = 0;
        uint32_t index = 0;

        for (const TestArgument& argument : arguments) {
            record[DEBUG_RECORD_ARG_WORD + index * 2] = static_cast<uint32_t>(argument.bits);
            record[DEBUG_RECORD_ARG_WORD + index * 2 + 1] = static_cast<uint32_t>(argument.bits >> 32);
            argKinds |= argument.kind << (index * 2);
            ++index;
        }

        record[0] = formatIndex;
        record[1] = kind | (index << 4) | (argKinds << 8);
        record[2] = 42;

        return record;
    }

    void TestDecode(Failures& failures) {
        // GPU は 32bit の符号つきの値を 64bit に符号を広げて書く(debug_ring.hlsli の DebugArgWords)
        const auto probe =
            MakeRecord(static_cast<uint32_t>(DebugFormat::DebugRingProbe), DEBUG_KIND_PRINT,
                       {{.bits = 3, .kind = DEBUG_ARG_U32},
                        {.bits = static_cast<uint64_t>(int64_t{-4}), .kind = DEBUG_ARG_I32},
                        {.bits = (uint64_t{3} << 40) | 0xABCDu, .kind = DEBUG_ARG_U64},
                        {.bits = static_cast<uint64_t>(-(int64_t{3} << 33) - 7), .kind = DEBUG_ARG_I64}});

        const gpu::DebugMessage message = gpu::DecodeDebugRecord(probe);
        failures.Check(message.text == ExpectedProbeText(3), "デコード: 型ごとの引数と {:#x} → " + message.text);

        failures.Check(message.format == DebugFormat::DebugRingProbe && !message.isAssert && message.line == 42 &&
                           message.where == "debug_ring_probe/Main",
                       "デコード: 書式の番号・種類・行・場所");

        const auto unknown = MakeRecord(9999, DEBUG_KIND_ASSERT, {{.bits = 7, .kind = DEBUG_ARG_U32}});
        const gpu::DebugMessage unknownMessage = gpu::DecodeDebugRecord(unknown);

        failures.Check(unknownMessage.format == DebugFormat::Count && unknownMessage.isAssert &&
                           unknownMessage.text.contains("9999") && unknownMessage.text.contains("[7]"),
                       "デコード: 知らない書式の番号 → " + unknownMessage.text);

        const auto shortRecord = MakeRecord(static_cast<uint32_t>(DebugFormat::DebugRingProbe), DEBUG_KIND_PRINT,
                                            {{.bits = 1, .kind = DEBUG_ARG_U32}});
        const gpu::DebugMessage shortMessage = gpu::DecodeDebugRecord(shortRecord);
        failures.Check(shortMessage.text.contains("当てはまらない") && shortMessage.text.contains("[1]"),
                       "デコード: 引数が足りない → " + shortMessage.text);
    }

    // --- GPU の準備 ---

    struct ProbeContext {
        gpu::ImmediateQueue* queue;
        ComPtr<ID3D12RootSignature> computeRootSignature;
        ComPtr<ID3D12PipelineState> computePipeline;
        ComPtr<ID3D12RootSignature> graphRootSignature;
        gpu::WorkGraph graph;
        gpu::DebugRing ring;
    };

    expected<ProbeContext, std::string> CreateContext(ID3D12Device5* device, gpu::ImmediateQueue& queue) {
        const auto computeShader = gpu::LoadShader("sim/debug_ring_probe.cso");
        const auto graphLibrary = gpu::LoadShader("sim/debug_ring_graph_probe.cso");
        if (!computeShader)
            return unexpected(computeShader.error());

        if (!graphLibrary)
            return unexpected(graphLibrary.error());

        ComPtr<ID3D12RootSignature> computeRootSignature = gpu::CreateRootSignature(device, COMPUTE_LAYOUT);
        ComPtr<ID3D12RootSignature> graphRootSignature = gpu::CreateRootSignature(device, GRAPH_LAYOUT);
        if (!computeRootSignature || !graphRootSignature)
            return unexpected("ルート署名を作れない");

        ComPtr<ID3D12PipelineState> computePipeline =
            gpu::CreateComputePipeline(device, computeRootSignature.Get(), *computeShader);
        if (!computePipeline)
            return unexpected("パイプラインを作れない");

        auto graph = gpu::WorkGraph::Create(device, graphRootSignature.Get(), *graphLibrary, L"DebugRingProbe");
        if (!graph)
            return unexpected(graph.error());

        auto ring = gpu::DebugRing::Create(device);
        if (!ring)
            return unexpected(ring.error());

        return ProbeContext{.queue = &queue,
                            .computeRootSignature = std::move(computeRootSignature),
                            .computePipeline = std::move(computePipeline),
                            .graphRootSignature = std::move(graphRootSignature),
                            .graph = std::move(*graph),
                            .ring = std::move(*ring)};
    }

    // --- 2〜4. compute から ---

    expected<gpu::DebugRingContents, std::string> RunCompute(ProbeContext& context, uint32_t printThreadCount) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return unexpected("コマンドリストを始められない");

        context.ring.RecordBegin(list);
        list->SetComputeRootSignature(context.computeRootSignature.Get());
        list->SetPipelineState(context.computePipeline.Get());
        list->SetComputeRoot32BitConstant(COMPUTE_LAYOUT.RootConstantIndex(), printThreadCount, 0);
        list->SetComputeRootUnorderedAccessView(COMPUTE_LAYOUT.DebugRingIndex(), context.ring.GpuAddress());
        list->Dispatch((printThreadCount + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP, 1, 1);
        context.ring.RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return unexpected("GPU での実行に失敗");

        return context.ring.Drain(MAX_LOGGED_MESSAGES);
    }

    // 数(書こうとした・落とした・読めた・assert)を確かめる
    void CheckComputeCounts(const gpu::DebugRingContents& contents, uint32_t printThreadCount, Failures& failures) {
        const uint32_t assertCount = printThreadCount > ASSERT_THREAD ? 1 : 0;
        const uint32_t requested = printThreadCount + assertCount;
        const uint32_t stored = std::min(requested, DEBUG_RING_CAPACITY);
        Log(Channel::Gpu, Level::Info, "compute {} スレッド: 書こうとした {}(期待 {})、落とした {}(期待 {})",
            printThreadCount, contents.requestedCount, requested, contents.droppedCount, requested - stored);
        failures.Check(contents.requestedCount == requested, "書こうとした数");
        failures.Check(contents.droppedCount == requested - stored, "落とした数");
        failures.Check(contents.messages.size() == stored, "読めた数");
    }

    // 溢れていないとき: スレッドごとの print が 1 回ずつ期待どおりの文字列で、assert がスレッド 5 の 1 件だけか
    void CheckComputeMessages(const gpu::DebugRingContents& contents, uint32_t printThreadCount, Failures& failures) {
        std::vector<int> printCounts(printThreadCount, 0);
        uint32_t assertCount = 0;
        for (const gpu::DebugMessage& message : contents.messages) {
            if (message.isAssert) {
                ++assertCount;

                failures.Check(message.format == DebugFormat::DebugRingProbeAssert &&
                                   message.text == format("thread={} で assert", ASSERT_THREAD),
                               "assert の中身 → " + message.text);

                continue;
            }

            const auto thread = static_cast<uint32_t>(std::stoul(message.text.substr(std::strlen("thread="))));
            const bool known = thread < printThreadCount && message.text == ExpectedProbeText(thread);
            failures.Check(known, "print の中身 → " + message.text);
            if (known)
                ++printCounts[thread];
        }

        failures.Check(assertCount == 1 && contents.assertCount == 1, "assert の数");
        failures.Check(rng::all_of(printCounts, [](int count) { return count == 1; }),
                       "どのスレッドの print もちょうど 1 回");
    }

    // --- 5. Work Graphs のノードから ---

    expected<gpu::DebugRingContents, std::string> RunGraph(ProbeContext& context) {
        const uint32_t entrypoint = context.graph.EntrypointIndex(L"Root");
        if (entrypoint == UINT32_MAX)
            return unexpected("入口のノード Root が無い");

        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return unexpected("コマンドリストを始められない");

        context.ring.RecordBegin(list);
        list->SetComputeRootSignature(context.graphRootSignature.Get());
        context.graph.SetProgram(list, true);
        list->SetComputeRootUnorderedAccessView(GRAPH_LAYOUT.DebugRingIndex(), context.ring.GpuAddress());
        const uint32_t rootRecord = 0;
        gpu::WorkGraph::DispatchFromCpu(list, entrypoint, &rootRecord, 1, sizeof(rootRecord));
        context.ring.RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return unexpected("GPU での実行に失敗");

        return context.ring.Drain(MAX_LOGGED_MESSAGES);
    }

    void CheckGraphMessages(const gpu::DebugRingContents& contents, Failures& failures) {
        Log(Channel::WorkGraph, Level::Info, "Work Graph: 書こうとした {}(期待 {})", contents.requestedCount,
            GRAPH_LEAF_COUNT);
        failures.Check(contents.requestedCount == GRAPH_LEAF_COUNT && contents.messages.size() == GRAPH_LEAF_COUNT,
                       "Work Graph: 数");
        std::vector<std::string> texts;

        for (const gpu::DebugMessage& message : contents.messages) {
            failures.Check(message.format == DebugFormat::DebugRingGraphLeaf && message.channel == Channel::WorkGraph &&
                               message.where == "debug_ring_graph_probe/Leaf",
                           "Work Graph: 書式とチャンネル");

            texts.push_back(message.text);
        }

        std::vector<std::string> expected;
        for (uint32_t value = 1; value <= GRAPH_LEAF_COUNT; ++value)
            expected.push_back(format("value={}", value));

        rng::sort(texts);
        rng::sort(expected);
        failures.Check(texts == expected, "Work Graph: 1〜256 が 1 回ずつ");
    }

    // --- 全体 ---

    expected<void, std::string> RunGpuTests(ProbeContext& context, Failures& failures) {
        for (const uint32_t printThreadCount : {PROBE_THREAD_COUNT, OVERFLOW_THREAD_COUNT, PROBE_THREAD_COUNT}) {
            const auto contents = RunCompute(context, printThreadCount);
            if (!contents)
                return unexpected(contents.error());

            CheckComputeCounts(*contents, printThreadCount, failures);
            if (contents->droppedCount == 0)
                CheckComputeMessages(*contents, printThreadCount, failures);
        }

        const auto graphContents = RunGraph(context);
        if (!graphContents)
            return unexpected(graphContents.error());

        CheckGraphMessages(*graphContents, failures);

        return {};
    }

    int Run(span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_debug_ring_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_debug_ring_test: adapter {}, queue {}(assert の Error はわざと出している)",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));

        Failures failures;
        TestDecode(failures);

        const auto device = gpu::Device::Create(options->adapter);
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_debug_ring_test: FAILED ({})", device.error());
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto context = queue ? CreateContext(device->Get(), *queue) : unexpected(queue.error());
        const auto result = context ? RunGpuTests(*context, failures) : unexpected(context.error());

        if (!result) {
            Log(Channel::Gpu, Level::Error, "gpu_debug_ring_test: FAILED ({})", result.error());
            return 1;
        }

        if (failures.count > 0) {
            Log(Channel::Gpu, Level::Error, "gpu_debug_ring_test: FAILED({} 件)", failures.count);
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_debug_ring_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_debug_ring_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();  // ログを閉じる

    return exitCode;
}
