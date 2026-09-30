// gpu_work_graph_stats_test.cpp — Work Graphs のノードのカウンタと上限の検出(T-0008、docs/design/16-debug-test.md §1)を確かめる。
//   1. 判定(GPU なし): EvaluateGraphStats が上限に当たった・近づいたものを見つけ、何も無ければ空。AccumulateGraphStats の足し方
//   2. GPU(shaders/sim/work_graph_limits_probe.hlsl): Spawn へレコードを渡し、場面ごとにカウンタが CPU の予想と一致するか
//      - ふつう: 何も見つからない
//      - 近い: Fan の出力の要求が上限ちょうど・Chain の再帰が深さの 3/4 → 「近い」の Warning
//      - 越える: Fan が上限を越える数を求め・Chain が深さの上限を越えて再帰したがり・計器が容量を越える
//        → 越えた分は出さずに数え(未定義の動作を起こさない)、Warning と、ノードの printf(場所 = ノード名・行つき)が出る
//      - たくさん: 64 レコードの値がばらばら(ウェーブでまとめて数える所の確かめ)
//   3. 空に戻るか: 「越える」の後の場面に、前の分が残っていない(場面ごとに読み戻して 0 に戻す)
// 引数は gpu_test_options.h。わざと Warning を出す。
#include <algorithm>
#include <array>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu/work_graph.h"
#include "gpu/work_graph_stats.h"
#include "gpu_test_options.h"

using namespace bicameral;
using Microsoft::WRL::ComPtr;

namespace {

    // work_graph_limits_probe.hlsl の番号と上限
    constexpr uint32_t NODE_SPAWN = 0;
    constexpr uint32_t NODE_FAN = 1;
    constexpr uint32_t NODE_LEAF = 2;
    constexpr uint32_t NODE_CHAIN = 3;
    constexpr uint32_t GAUGE_FAN_REQUEST = 0;
    constexpr uint32_t FAN_MAX_RECORDS = 4;
    constexpr uint32_t CHAIN_MAX_DEPTH = 8;
    constexpr uint32_t FAN_REQUEST_CAPACITY = 8;  // 計器の容量(試験のために決めた値)

    constexpr gpu::RootSignatureLayout GRAPH_LAYOUT{.debugRing = true, .graphStats = true};
    constexpr uint32_t MANY_RECORDS = 64;

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition) return;
            Log(Channel::WorkGraph, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    gpu::GraphStatsLayout MakeLayout() {
        return {.name = "上限の試験",
                .nodes = {{.name = "Spawn", .maxOutputRecords = 2},
                          {.name = "Fan", .maxOutputRecords = FAN_MAX_RECORDS, .warnOutputRecords = FAN_MAX_RECORDS},
                          {.name = "Leaf"},
                          {.name = "Chain", .maxOutputRecords = 1, .maxRecursionDepth = CHAIN_MAX_DEPTH}},
                .gauges = {{.name = "Fan の要求", .capacity = FAN_REQUEST_CAPACITY}}};
    }

    std::vector<gpu::GraphFindingKind> Kinds(const std::vector<gpu::GraphFinding>& findings) {
        std::vector<gpu::GraphFindingKind> kinds;
        kinds.reserve(findings.size());
        for (const gpu::GraphFinding& finding : findings) {
            kinds.push_back(finding.kind);
        }
        return kinds;
    }

    // --- 1. 判定(GPU なし)---

    void TestEvaluate(Failures& failures) {
        const gpu::GraphStatsLayout layout = MakeLayout();
        gpu::GraphStatsSnapshot snapshot{.nodes = std::vector<gpu::GraphNodeCounters>(4), .gaugePeaks = {0}};
        failures.Check(gpu::EvaluateGraphStats(layout, snapshot).empty(), "判定: 全部 0 なら何も無い");

        snapshot.nodes[NODE_FAN].peakRequestedOutputs = 3;
        snapshot.nodes[NODE_CHAIN].deepestRecursion = 5;  // 5 × 4 < 8 × 3
        snapshot.gaugePeaks[GAUGE_FAN_REQUEST] = 5;       // 5 × 100 < 8 × 75
        failures.Check(gpu::EvaluateGraphStats(layout, snapshot).empty(), "判定: 上限の手前(3/4 未満)なら何も無い");

        snapshot.nodes[NODE_FAN].peakRequestedOutputs = 4;
        snapshot.nodes[NODE_CHAIN].deepestRecursion = 6;
        snapshot.gaugePeaks[GAUGE_FAN_REQUEST] = 6;
        const std::vector<gpu::GraphFindingKind> nearKinds = Kinds(gpu::EvaluateGraphStats(layout, snapshot));
        failures.Check(
            nearKinds == std::vector{gpu::GraphFindingKind::OutputsNearLimit, gpu::GraphFindingKind::RecursionNearLimit,
                                     gpu::GraphFindingKind::GaugeNearCapacity},
            "判定: 近い 3 つ(ノードの順 → 計器)");

        snapshot.nodes[NODE_FAN].refusedOutputs = 2;
        snapshot.nodes[NODE_CHAIN].refusedRecursions = 1;
        snapshot.gaugePeaks[GAUGE_FAN_REQUEST] = 9;
        const std::vector<gpu::GraphFinding> over = gpu::EvaluateGraphStats(layout, snapshot);
        failures.Check(
            Kinds(over) == std::vector{gpu::GraphFindingKind::OutputsRefused, gpu::GraphFindingKind::RecursionRefused,
                                       gpu::GraphFindingKind::GaugeOverCapacity},
            "判定: 止めたものは「近い」より先(近いは出さない)");
        failures.Check(!over.empty() && over[0].index == NODE_FAN && over[0].text.contains("Fan"), "判定: 番号と名前");

        gpu::GraphStatsSnapshot total;
        gpu::AccumulateGraphStats(total, snapshot);
        gpu::AccumulateGraphStats(total, snapshot);
        gpu::AccumulateGraphStats(total, {});  // 読めなかったフレームは足さない
        failures.Check(total.nodes.size() == 4 && total.nodes[NODE_FAN].refusedOutputs == 4 &&
                           total.nodes[NODE_FAN].peakRequestedOutputs == 4 && total.gaugePeaks[0] == 9,
                       "足し方: 数は足し、最大は最大");
    }

    // --- 2. GPU ---

    struct SpawnRecord {
        uint32_t id;
        uint32_t fanRequest;
        uint32_t chainRequest;
    };

    struct Scenario {
        const char* name;
        std::vector<SpawnRecord> records;
        std::vector<gpu::GraphFindingKind> expectedFindings;
        uint32_t expectedPrints;  // ノードの printf(止めたとき)の数
    };

    // work_graph_limits_probe.hlsl を CPU で真似たカウンタ
    gpu::GraphStatsSnapshot ExpectedStats(const std::vector<SpawnRecord>& records) {
        gpu::GraphStatsSnapshot expected{.nodes = std::vector<gpu::GraphNodeCounters>(4), .gaugePeaks = {0}};
        for (const SpawnRecord& record : records) {
            gpu::GraphNodeCounters& spawn = expected.nodes[NODE_SPAWN];
            ++spawn.launches;
            ++spawn.inputRecords;
            spawn.outputRecords += 2;
            spawn.peakRequestedOutputs = 2;

            const uint32_t granted = std::min(record.fanRequest, FAN_MAX_RECORDS);
            gpu::GraphNodeCounters& fan = expected.nodes[NODE_FAN];
            ++fan.launches;
            ++fan.inputRecords;
            fan.outputRecords += granted;
            fan.refusedOutputs += record.fanRequest - granted;
            fan.peakRequestedOutputs = std::max(fan.peakRequestedOutputs, record.fanRequest);
            expected.nodes[NODE_LEAF].launches += granted;
            expected.nodes[NODE_LEAF].inputRecords += granted;
            expected.gaugePeaks[GAUGE_FAN_REQUEST] =
                std::max(expected.gaugePeaks[GAUGE_FAN_REQUEST], record.fanRequest);

            const uint32_t depth = std::min(record.chainRequest, CHAIN_MAX_DEPTH);
            gpu::GraphNodeCounters& chain = expected.nodes[NODE_CHAIN];
            chain.launches += depth + 1;
            chain.inputRecords += depth + 1;
            chain.outputRecords += depth;
            chain.peakRequestedOutputs = std::max(chain.peakRequestedOutputs, depth > 0 ? 1u : 0u);
            chain.deepestRecursion = std::max(chain.deepestRecursion, depth);
            chain.refusedRecursions += record.chainRequest > CHAIN_MAX_DEPTH ? 1 : 0;
        }
        return expected;
    }

    std::vector<Scenario> MakeScenarios() {
        std::vector<SpawnRecord> many;
        many.reserve(MANY_RECORDS);
        for (uint32_t index = 0; index < MANY_RECORDS; ++index) {
            many.push_back({.id = 100 + index, .fanRequest = index % 4, .chainRequest = index % 5});
        }
        using enum gpu::GraphFindingKind;
        return {
            {.name = "ふつう",
             .records = {{.id = 1, .fanRequest = 2, .chainRequest = 3}},
             .expectedFindings = {},
             .expectedPrints = 0},
            {.name = "近い",
             .records = {{.id = 2, .fanRequest = FAN_MAX_RECORDS, .chainRequest = 6}},
             .expectedFindings = {OutputsNearLimit, RecursionNearLimit},
             .expectedPrints = 0},
            {.name = "越える",
             .records = {{.id = 3, .fanRequest = 9, .chainRequest = 20}},
             .expectedFindings = {OutputsRefused, RecursionRefused, GaugeOverCapacity},
             .expectedPrints = 2},
            {.name = "たくさん", .records = many, .expectedFindings = {}, .expectedPrints = 0},
        };
    }

    struct ProbeContext {
        gpu::ImmediateQueue* queue;
        ComPtr<ID3D12RootSignature> rootSignature;
        gpu::WorkGraph graph;
        gpu::WorkGraphStats stats;
        gpu::DebugRing ring;
        bool initialized = false;
    };

    std::expected<ProbeContext, std::string> CreateContext(ID3D12Device5* device, gpu::ImmediateQueue& queue) {
        const auto library = gpu::LoadShader("sim/work_graph_limits_probe.cso");
        if (!library) return std::unexpected(library.error());
        ComPtr<ID3D12RootSignature> rootSignature = gpu::CreateRootSignature(device, GRAPH_LAYOUT);
        if (!rootSignature) return std::unexpected("ルート署名を作れない");
        auto graph = gpu::WorkGraph::Create(device, rootSignature.Get(), *library, L"WorkGraphLimitsProbe");
        if (!graph) return std::unexpected(graph.error());
        auto stats = gpu::WorkGraphStats::Create(device, MakeLayout());
        if (!stats) return std::unexpected(stats.error());
        auto ring = gpu::DebugRing::Create(device);
        if (!ring) return std::unexpected(ring.error());
        return ProbeContext{.queue = &queue,
                            .rootSignature = std::move(rootSignature),
                            .graph = std::move(*graph),
                            .stats = std::move(*stats),
                            .ring = std::move(*ring)};
    }

    struct RunOutput {
        gpu::GraphStatsSnapshot stats;
        gpu::DebugRingContents messages;
    };

    std::expected<RunOutput, std::string> RunScenario(ProbeContext& context, const Scenario& scenario) {
        const uint32_t entrypoint = context.graph.EntrypointIndex(L"Spawn");
        if (entrypoint == UINT32_MAX) return std::unexpected("入口のノード Spawn が無い");
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr) return std::unexpected("コマンドリストを始められない");
        context.ring.RecordBegin(list);
        context.stats.RecordBegin(list);
        list->SetComputeRootSignature(context.rootSignature.Get());
        context.graph.SetProgram(list, !context.initialized);
        context.initialized = true;
        list->SetComputeRootUnorderedAccessView(GRAPH_LAYOUT.DebugRingIndex(), context.ring.GpuAddress());
        list->SetComputeRootUnorderedAccessView(GRAPH_LAYOUT.GraphStatsIndex(), context.stats.GpuAddress());
        gpu::WorkGraph::DispatchFromCpu(list, entrypoint, scenario.records.data(),
                                        static_cast<uint32_t>(scenario.records.size()), sizeof(SpawnRecord));
        context.stats.RecordReadbackAndReset(list);
        context.ring.RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait()) return std::unexpected("GPU での実行に失敗");
        auto stats = context.stats.Read();
        if (!stats) return std::unexpected(stats.error());
        return RunOutput{.stats = std::move(*stats), .messages = context.ring.Drain()};
    }

    void LogMismatch(const gpu::GraphStatsLayout& layout, const gpu::GraphStatsSnapshot& actual,
                     const gpu::GraphStatsSnapshot& expected) {
        Log(Channel::WorkGraph, Level::Error, "  GPU: {}", gpu::FormatGraphStats(layout, actual));
        Log(Channel::WorkGraph, Level::Error, "  CPU: {}", gpu::FormatGraphStats(layout, expected));
        for (size_t node = 0; node < actual.nodes.size() && node < expected.nodes.size(); ++node) {
            const gpu::GraphNodeCounters& a = actual.nodes[node];
            const gpu::GraphNodeCounters& e = expected.nodes[node];
            if (a == e) continue;
            Log(Channel::WorkGraph, Level::Error,
                "  {}: 最大の要求 {}/{} 深さ {}/{} 止めた出力 {}/{} 止めた再帰 {}/{}(GPU/CPU)", layout.nodes[node].name,
                a.peakRequestedOutputs, e.peakRequestedOutputs, a.deepestRecursion, e.deepestRecursion,
                a.refusedOutputs, e.refusedOutputs, a.refusedRecursions, e.refusedRecursions);
        }
    }

    // 止めたときの printf: 場所がノード名で、行が付き、チャンネルが WorkGraph
    void CheckPrints(const Scenario& scenario, const gpu::DebugRingContents& messages, Failures& failures) {
        failures.Check(messages.messages.size() == scenario.expectedPrints && messages.assertCount == 0,
                       std::format("{}: ノードの printf の数 {}(期待 {})", scenario.name, messages.messages.size(),
                                   scenario.expectedPrints));
        for (const gpu::DebugMessage& message : messages.messages) {
            Log(Channel::WorkGraph, Level::Info, "  printf: {}:{} {}", message.where, message.line, message.text);
            const bool fan = message.format == DebugFormat::WgLimitsFanRefused &&
                             message.where == "work_graph_limits_probe/Fan" &&
                             message.text == "レコード 3: 出力を 9 件求めた(上限 4。越えた分は出さない)";
            const bool chain = message.format == DebugFormat::WgLimitsChainRefused &&
                               message.where == "work_graph_limits_probe/Chain" &&
                               message.text == "レコード 3: 深さ 8 で再帰の上限(自分へ出さない)";
            failures.Check((fan || chain) && message.line > 0 && message.channel == Channel::WorkGraph,
                           std::format("{}: printf の中身と場所 → {}", scenario.name, message.text));
        }
    }

    std::expected<void, std::string> RunGpuTests(ProbeContext& context, Failures& failures) {
        // 「越える」の後にもう一度「ふつう」: 前の分が残っていないか
        std::vector<Scenario> scenarios = MakeScenarios();
        scenarios.push_back(scenarios.front());
        for (const Scenario& scenario : scenarios) {
            const auto output = RunScenario(context, scenario);
            if (!output) return std::unexpected(output.error());
            const gpu::GraphStatsSnapshot expected = ExpectedStats(scenario.records);
            Log(Channel::WorkGraph, Level::Info, "{}: {}", scenario.name,
                gpu::FormatGraphStats(context.stats.Layout(), output->stats));
            const bool match = output->stats == expected;
            if (!match) LogMismatch(context.stats.Layout(), output->stats, expected);
            failures.Check(match, std::format("{}: カウンタが CPU の予想と一致", scenario.name));
            // Report は Warning をログへ出す(わざと)。見つかったものは毎回全部返る
            const std::vector<gpu::GraphFinding> findings = context.stats.Report(output->stats);
            failures.Check(Kinds(findings) == scenario.expectedFindings,
                           std::format("{}: 上限の検出({} 件、期待 {} 件)", scenario.name, findings.size(),
                                       scenario.expectedFindings.size()));
            CheckPrints(scenario, output->messages, failures);
        }
        return {};
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_work_graph_stats_test [--warp] [--queue direct|compute]");
            return 2;
        }
        Log(Channel::WorkGraph, Level::Info,
            "gpu_work_graph_stats_test: adapter {}, queue {}(上限の Warning はわざと出している)",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));

        Failures failures;
        TestEvaluate(failures);

        const auto device = gpu::Device::Create(options->adapter);
        if (!device) {
            Log(Channel::WorkGraph, Level::Error, "gpu_work_graph_stats_test: FAILED ({})", device.error());
            return 1;
        }
        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto context = queue ? CreateContext(device->Get(), *queue) : std::unexpected(queue.error());
        const auto result = context ? RunGpuTests(*context, failures) : std::unexpected(context.error());
        if (!result) {
            Log(Channel::WorkGraph, Level::Error, "gpu_work_graph_stats_test: FAILED ({})", result.error());
            return 1;
        }
        if (failures.count > 0) {
            Log(Channel::WorkGraph, Level::Error, "gpu_work_graph_stats_test: FAILED({} 件)", failures.count);
            return 1;
        }
        if (!test::PassesValidation(*device, "gpu_work_graph_stats_test")) return 1;
        Log(Channel::WorkGraph, Level::Info, "gpu_work_graph_stats_test: OK");
        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();  // ログを閉じる
    return exitCode;
}
