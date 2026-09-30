// work_graph_stats.cpp — Work Graphs のノードごとのカウンタの読み戻し・要約・上限の検出(T-0008、16 §1)。
#include "gpu/work_graph_stats.h"

#include <algorithm>
#include <format>
#include <span>

#include "core/log.h"

namespace bicameral::gpu {
    namespace {

        constexpr uint32_t KIND_COUNT = static_cast<uint32_t>(GraphFindingKind::Count);
        constexpr uint32_t FINDING_SLOTS_PER_KIND = std::max(WG_STATS_MAX_NODES, WG_STATS_MAX_GAUGES);

        GraphNodeCounters ReadNode(std::span<const uint32_t> words, uint32_t node) {
            const std::span<const uint32_t> entry = words.subspan(size_t{node} * WG_NODE_WORDS, WG_NODE_WORDS);
            return {.launches = entry[WG_NODE_LAUNCHES],
                    .inputRecords = entry[WG_NODE_INPUT_RECORDS],
                    .outputRecords = entry[WG_NODE_OUTPUT_RECORDS],
                    .refusedOutputs = entry[WG_NODE_REFUSED_OUTPUTS],
                    .peakRequestedOutputs = entry[WG_NODE_PEAK_REQUESTED],
                    .deepestRecursion = entry[WG_NODE_DEEPEST_RECURSION],
                    .refusedRecursions = entry[WG_NODE_REFUSED_RECURSIONS]};
        }

        // --- 上限の判定(ノード 1 つ・計器 1 つ)---

        void EvaluateNode(const GraphNodeLimits& limits, const GraphNodeCounters& counters, uint32_t index,
                          std::vector<GraphFinding>& findings) {
            if (counters.refusedOutputs > 0) {
                findings.push_back({.kind = GraphFindingKind::OutputsRefused,
                                    .index = index,
                                    .text = std::format("ノード {}: 出力の上限 {} を超える要求を {} 件止めた"
                                                        "(1 回の起動の要求の最大 {})",
                                                        limits.name, limits.maxOutputRecords, counters.refusedOutputs,
                                                        counters.peakRequestedOutputs)});
            } else if (limits.warnOutputRecords > 0 && counters.peakRequestedOutputs >= limits.warnOutputRecords) {
                findings.push_back(
                    {.kind = GraphFindingKind::OutputsNearLimit,
                     .index = index,
                     .text = std::format("ノード {}: 1 回の起動の出力の要求 {} が上限 {} に近い", limits.name,
                                         counters.peakRequestedOutputs, limits.maxOutputRecords)});
            }
            if (counters.refusedRecursions > 0) {
                findings.push_back({.kind = GraphFindingKind::RecursionRefused,
                                    .index = index,
                                    .text = std::format("ノード {}: 再帰の深さの上限 {} で {} 回止めた", limits.name,
                                                        limits.maxRecursionDepth, counters.refusedRecursions)});
            } else if (limits.maxRecursionDepth > 0 && counters.deepestRecursion > 0 &&
                       uint64_t{counters.deepestRecursion} * 4 >= uint64_t{limits.maxRecursionDepth} * 3) {
                findings.push_back({.kind = GraphFindingKind::RecursionNearLimit,
                                    .index = index,
                                    .text = std::format("ノード {}: 再帰の深さ {} が上限 {} に近い", limits.name,
                                                        counters.deepestRecursion, limits.maxRecursionDepth)});
            }
        }

        void EvaluateGauge(const GraphGaugeLimits& limits, uint32_t peak, uint32_t index,
                           std::vector<GraphFinding>& findings) {
            if (limits.capacity == 0) return;
            if (peak > limits.capacity) {
                findings.push_back({.kind = GraphFindingKind::GaugeOverCapacity,
                                    .index = index,
                                    .text = std::format("{}: 容量 {} を超えて {} まで使おうとした(溢れた分は落ちた)",
                                                        limits.name, limits.capacity, peak)});
            } else if (uint64_t{peak} * 100 >= uint64_t{limits.capacity} * limits.warnPercent) {
                findings.push_back({.kind = GraphFindingKind::GaugeNearCapacity,
                                    .index = index,
                                    .text = std::format("{}: 使った量の最大 {} が容量 {} に近い", limits.name, peak,
                                                        limits.capacity)});
            }
        }

    }  // namespace

    // --- 要約 ---

    void AccumulateGraphStats(GraphStatsSnapshot& total, const GraphStatsSnapshot& frame) {
        if (frame.nodes.empty() && frame.gaugePeaks.empty()) return;  // 読めなかったフレーム
        if (total.nodes.size() != frame.nodes.size() || total.gaugePeaks.size() != frame.gaugePeaks.size()) {
            total = frame;
            return;
        }
        for (size_t index = 0; index < frame.nodes.size(); ++index) {
            GraphNodeCounters& sum = total.nodes[index];
            const GraphNodeCounters& add = frame.nodes[index];
            sum.launches += add.launches;
            sum.inputRecords += add.inputRecords;
            sum.outputRecords += add.outputRecords;
            sum.refusedOutputs += add.refusedOutputs;
            sum.peakRequestedOutputs = std::max(sum.peakRequestedOutputs, add.peakRequestedOutputs);
            sum.deepestRecursion = std::max(sum.deepestRecursion, add.deepestRecursion);
            sum.refusedRecursions += add.refusedRecursions;
        }
        for (size_t index = 0; index < frame.gaugePeaks.size(); ++index) {
            total.gaugePeaks[index] = std::max(total.gaugePeaks[index], frame.gaugePeaks[index]);
        }
    }

    std::string FormatGraphStats(const GraphStatsLayout& layout, const GraphStatsSnapshot& snapshot) {
        std::string text = layout.name + ":";
        const char* separator = " ";
        for (size_t index = 0; index < layout.nodes.size() && index < snapshot.nodes.size(); ++index) {
            const GraphNodeCounters& node = snapshot.nodes[index];
            text += std::format("{}{} 起動 {}・入力 {}・出力 {}", separator, layout.nodes[index].name, node.launches,
                                node.inputRecords, node.outputRecords);
            if (node.refusedOutputs > 0) text += std::format("・止めた出力 {}", node.refusedOutputs);
            if (layout.nodes[index].maxRecursionDepth > 0) {
                text += std::format("・深さ {}/{}", node.deepestRecursion, layout.nodes[index].maxRecursionDepth);
            }
            if (node.refusedRecursions > 0) text += std::format("・止めた再帰 {}", node.refusedRecursions);
            separator = " | ";
        }
        for (size_t index = 0; index < layout.gauges.size() && index < snapshot.gaugePeaks.size(); ++index) {
            text += std::format("{}{} 最大 {}/{}", separator, layout.gauges[index].name, snapshot.gaugePeaks[index],
                                layout.gauges[index].capacity);
            separator = " | ";
        }
        return text;
    }

    std::vector<GraphFinding> EvaluateGraphStats(const GraphStatsLayout& layout, const GraphStatsSnapshot& snapshot) {
        std::vector<GraphFinding> findings;
        for (size_t index = 0; index < layout.nodes.size() && index < snapshot.nodes.size(); ++index) {
            EvaluateNode(layout.nodes[index], snapshot.nodes[index], static_cast<uint32_t>(index), findings);
        }
        for (size_t index = 0; index < layout.gauges.size() && index < snapshot.gaugePeaks.size(); ++index) {
            EvaluateGauge(layout.gauges[index], snapshot.gaugePeaks[index], static_cast<uint32_t>(index), findings);
        }
        return findings;
    }

    // --- GPU のバッファ ---

    WorkGraphStats::WorkGraphStats(ReadbackRing&& ring, GraphStatsLayout&& layout)
        : m_ring(std::move(ring)),
          m_layout(std::move(layout)),
          m_lastWarnedReport(size_t{KIND_COUNT} * FINDING_SLOTS_PER_KIND, 0) {}

    std::expected<WorkGraphStats, std::string> WorkGraphStats::Create(ID3D12Device* device, GraphStatsLayout layout,
                                                                      uint32_t slotCount) {
        if (layout.nodes.size() > WG_STATS_MAX_NODES || layout.gauges.size() > WG_STATS_MAX_GAUGES) {
            return std::unexpected(std::format("{}: ノード {} 個・計器 {} 個は多すぎる(最大 {}・{})", layout.name,
                                               layout.nodes.size(), layout.gauges.size(), WG_STATS_MAX_NODES,
                                               WG_STATS_MAX_GAUGES));
        }
        // 見出し = 全体(読み戻すたびに全部を 0 に戻す)
        auto ring = ReadbackRing::Create(device, WG_STATS_BYTES, WG_STATS_BYTES, slotCount, L"WorkGraphStats");
        if (!ring) return std::unexpected("Work Graphs のカウンタを作れない: " + ring.error());
        return WorkGraphStats(std::move(*ring), std::move(layout));
    }

    void WorkGraphStats::RecordBegin(ID3D12GraphicsCommandList* list) const {
        m_ring.RecordBegin(list);
    }

    void WorkGraphStats::RecordReadbackAndReset(ID3D12GraphicsCommandList* list, uint32_t slot) const {
        m_ring.RecordReadbackAndReset(list, slot);
    }

    std::expected<GraphStatsSnapshot, std::string> WorkGraphStats::Read(uint32_t slot) const {
        std::vector<uint32_t> words(WG_STATS_WORDS);
        if (!m_ring.Read(slot, std::as_writable_bytes(std::span(words)))) {
            return std::unexpected(std::format("{}: カウンタを読み戻せない(slot {})", m_layout.name, slot));
        }
        GraphStatsSnapshot snapshot;
        snapshot.nodes.reserve(m_layout.nodes.size());
        for (uint32_t node = 0; node < m_layout.nodes.size(); ++node) {
            snapshot.nodes.push_back(ReadNode(words, node));
        }
        const std::span<const uint32_t> gauges =
            std::span(words).subspan(size_t{WG_STATS_MAX_NODES} * WG_NODE_WORDS, WG_STATS_MAX_GAUGES);
        snapshot.gaugePeaks.assign(gauges.begin(), gauges.begin() + static_cast<ptrdiff_t>(m_layout.gauges.size()));
        return snapshot;
    }

    std::vector<GraphFinding> WorkGraphStats::Report(const GraphStatsSnapshot& snapshot) {
        ++m_reportCount;
        if (GetLogger().IsEnabled(Level::Trace)) {  // 捨てるなら要約を作らない
            Log(Channel::WorkGraph, Level::Trace, "{}", FormatGraphStats(m_layout, snapshot));
        }
        std::vector<GraphFinding> findings = EvaluateGraphStats(m_layout, snapshot);
        for (const GraphFinding& finding : findings) {
            uint64_t& last = m_lastWarnedReport[size_t{static_cast<uint32_t>(finding.kind)} * FINDING_SLOTS_PER_KIND +
                                                finding.index];
            if (last != 0 && m_reportCount - last < WARNING_REPEAT_REPORTS) continue;
            last = m_reportCount;
            Log(Channel::WorkGraph, Level::Warning, "{}", finding.text);
        }
        return findings;
    }

}  // namespace bicameral::gpu
