// work_graph_stats.h — Work Graphs のノードごとのカウンタ(shaders/common/work_graph_stats.hlsli)を CPU 側で持つ(T-0008、16 §1)。
//
// データの流れ: ノードが WgCountLaunch などで u1 space1 のバッファに数える → RecordReadbackAndReset() が slot の読み戻しのバッファへ写して
//   0 に戻す → その slot のリストが終わった後に Read() → Report() がログ(ADR-0006)へ 1 行の要約(Trace)と、上限の Warning を出す。
// どのノードが何番か・宣言した上限はいくつかは、グラフを作る側が GraphStatsLayout に書く(HLSL の番号と同じ順)。
//
// 1 本のコマンドリストの中での使い方:
//   stats.RecordBegin(list);                                                     // COMMON → UAV
//   list->SetComputeRootUnorderedAccessView(layout.GraphStatsIndex(), stats.GpuAddress());
//   ... DispatchGraph / Dispatch ...
//   stats.RecordReadbackAndReset(list, slot);
//   (slot のリストが終わったら)auto snapshot = stats.Read(slot);  stats.Report(*snapshot);
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "common/work_graph_stats.hlsli"
#include "core/aliases.h"
#include "gpu/readback_ring.h"

namespace bicameral::gpu {

    // --- グラフを作る側が書く上限(HLSL の宣言と同じ値)---

    struct GraphNodeLimits {
        std::string name;
        uint32_t maxOutputRecords = 0;  // 1 回の起動で出せる数(宣言した MaxRecords の合計。0 = 出さない)
        uint32_t warnOutputRecords =
            0;  // 1 回の起動の要求がこれ以上なら「上限に近い」(0 = 見ない。出す数が構造で決まるノード)
        uint32_t maxRecursionDepth = 0;  // NodeMaxRecursionDepth(0 = 再帰しない)
    };

    struct GraphGaugeLimits {
        std::string name;
        uint32_t capacity = 0;
        uint32_t warnPercent = 75;  // 使った量の最大が容量のこの割合以上なら「近い」(100 なら満杯のときだけ)
    };

    struct GraphStatsLayout {
        std::string name;                      // ログに出すグラフの名前
        std::vector<GraphNodeLimits> nodes;    // WG_STATS_MAX_NODES 個まで。番号 = 並び順
        std::vector<GraphGaugeLimits> gauges;  // WG_STATS_MAX_GAUGES 個まで
    };

    // --- 読み戻したもの ---

    struct GraphNodeCounters {
        uint32_t launches = 0;
        uint32_t inputRecords = 0;
        uint32_t outputRecords = 0;
        uint32_t refusedOutputs = 0;
        uint32_t peakRequestedOutputs = 0;
        uint32_t deepestRecursion = 0;
        uint32_t refusedRecursions = 0;
        friend bool operator==(const GraphNodeCounters&, const GraphNodeCounters&) = default;
    };

    struct GraphStatsSnapshot {
        std::vector<GraphNodeCounters> nodes;  // layout.nodes と同じ数・順
        std::vector<uint32_t> gaugePeaks;      // layout.gauges と同じ数・順
        friend bool operator==(const GraphStatsSnapshot&, const GraphStatsSnapshot&) = default;
    };

    // 何フレームかの分をまとめる(数は足し、最大は最大)。total が空なら frame の形にする。frame が空(読めなかった)なら何もしない
    void AccumulateGraphStats(GraphStatsSnapshot& total, const GraphStatsSnapshot& frame);

    // 1 行の要約(「グラフ: ノード 起動 n・入力 n・出力 n | … | 計器 最大 n/容量」)
    [[nodiscard]] std::string FormatGraphStats(const GraphStatsLayout& layout, const GraphStatsSnapshot& snapshot);

    // --- 上限の検出 ---

    enum class GraphFindingKind : uint8_t {
        OutputsRefused,      // 出力の上限を超える要求を止めた(結果が変わっている)
        OutputsNearLimit,    // 1 回の起動の出力の要求が warnOutputRecords 以上
        RecursionRefused,    // 再帰の深さの上限で止めた(結果が変わっている)
        RecursionNearLimit,  // 再帰の深さが宣言の 3/4 以上
        GaugeOverCapacity,   // 容量を超えて使おうとした(溢れた分は落ちている)
        GaugeNearCapacity,   // 容量の warnPercent 以上
        Count,
    };

    struct GraphFinding {
        GraphFindingKind kind = GraphFindingKind::Count;
        uint32_t index = 0;  // ノードか計器の番号
        std::string text;    // ログに出す 1 行
    };

    // 上限に当たった・近づいたものを並べる(ノードの順 → 計器の順)。GPU なしで確かめられる
    [[nodiscard]] std::vector<GraphFinding> EvaluateGraphStats(const GraphStatsLayout& layout,
                                                               const GraphStatsSnapshot& snapshot);

    class WorkGraphStats {
    public:
        // 同じ Warning(種類 × 番号)をもう一度ログに出すのは、この回数の Report() の後(毎フレームの Warning でログを埋めない)
        static constexpr uint64_t WARNING_REPEAT_REPORTS = 600;

        [[nodiscard]] static expected<WorkGraphStats, std::string> Create(ID3D12Device* device, GraphStatsLayout layout,
                                                                          uint32_t slotCount = 1);

        // ルートの UAV(u1 space1)に渡すアドレス
        [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const { return m_ring.GpuAddress(); }
        [[nodiscard]] const GraphStatsLayout& Layout() const { return m_layout; }

        // シェーダーが数える前に: COMMON → UNORDERED_ACCESS
        void RecordBegin(ID3D12GraphicsCommandList* list) const;

        // 数えた後に: slot の読み戻しのバッファへ写し、全部を 0 に戻す。最後は COMMON に戻す
        void RecordReadbackAndReset(ID3D12GraphicsCommandList* list, uint32_t slot = 0) const;

        // slot のリストを GPU が終えた後に呼ぶ
        [[nodiscard]] expected<GraphStatsSnapshot, std::string> Read(uint32_t slot = 0) const;

        // 要約を Trace で、見つかった上限を Warning でログへ(同じものは WARNING_REPEAT_REPORTS 回に 1 回)。見つかったものを全部返す
        std::vector<GraphFinding> Report(const GraphStatsSnapshot& snapshot);

    private:
        WorkGraphStats(ReadbackRing&& ring, GraphStatsLayout&& layout);

        ReadbackRing m_ring;
        GraphStatsLayout m_layout;
        uint64_t m_reportCount = 0;
        std::vector<uint64_t>
            m_lastWarnedReport;  // (種類 × 番号)ごとに、最後にログに出した Report の番号 + 1(0 = まだ)
    };

}  // namespace bicameral::gpu
