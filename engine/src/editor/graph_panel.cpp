// graph_panel.cpp — 「Work Graphs と性能」のパネル(T-0143)。考え方は graph_panel.h。
#include "editor/graph_panel.h"

#include <algorithm>
#include <format>
#include <string_view>

#include <imgui.h>

#include "core/aliases.h"

namespace bicameral::editor {
    namespace {

        constexpr float PANEL_X_PIXELS = 12.0f;
        constexpr float PANEL_Y_PIXELS = 420.0f;
        constexpr ImVec4 WARNING_COLOR = {1.0f, 0.55f, 0.35f, 1.0f};

        void Cell(std::string_view text) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(text.data(), text.data() + text.size());
        }

        double PerTick(uint64_t count, uint64_t ticks) {
            return ticks > 0 ? static_cast<double>(count) / static_cast<double>(ticks) : 0.0;
        }

        // --- 単位ごとの GPU 時間 ---
        void BuildUnitTable(const GraphPanelStatus& status) {
            double total = 0.0;
            for (const UnitTime& unit : status.units)
                total += unit.millisecondsPerTick;

            if (!ImGui::BeginTable("units", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
                return;

            ImGui::TableSetupColumn("単位");
            ImGui::TableSetupColumn("ms/刻み");
            ImGui::TableSetupColumn("割合");
            ImGui::TableHeadersRow();
            for (const UnitTime& unit : status.units) {
                Cell(unit.workGraph ? std::format("{}(Work Graph)", unit.name) : unit.name);
                Cell(std::format("{:.3f}", unit.millisecondsPerTick));
                ImGui::TableNextColumn();
                const float fraction = total > 0.0 ? static_cast<float>(unit.millisecondsPerTick / total) : 0.0f;
                ImGui::ProgressBar(fraction, {120.0f, 0.0f});
            }

            ImGui::EndTable();
            ImGui::TextUnformatted(std::format("合計 {:.3f} ms/刻み", total).c_str());
        }

        // --- ノードごとの数と上限の余裕(T-0008 のカウンタ)---
        void BuildNodeTable(const gpu::GraphStatsLayout& layout, const GraphPanelStatus& status) {
            const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit;
            if (!ImGui::BeginTable("nodes", 6, flags))
                return;

            ImGui::TableSetupColumn("ノード");
            ImGui::TableSetupColumn("起動/刻み");
            ImGui::TableSetupColumn("入力/刻み");
            ImGui::TableSetupColumn("出力/刻み");
            ImGui::TableSetupColumn("1 回の出力 最大/上限");
            ImGui::TableSetupColumn("再帰 最大/上限");
            ImGui::TableHeadersRow();
            const size_t count = std::min(layout.nodes.size(), status.stats.nodes.size());
            for (size_t index = 0; index < count; ++index) {
                const gpu::GraphNodeLimits& limits = layout.nodes[index];
                const gpu::GraphNodeCounters& counters = status.stats.nodes[index];
                Cell(limits.name);
                Cell(std::format("{:.1f}", PerTick(counters.launches, status.ticks)));
                Cell(std::format("{:.1f}", PerTick(counters.inputRecords, status.ticks)));
                Cell(std::format("{:.1f}", PerTick(counters.outputRecords, status.ticks)));
                Cell(limits.maxOutputRecords > 0
                         ? std::format("{} / {}", counters.peakRequestedOutputs, limits.maxOutputRecords)
                         : std::string("-"));
                Cell(limits.maxRecursionDepth > 0
                         ? std::format("{} / {}", counters.deepestRecursion, limits.maxRecursionDepth)
                         : std::string("-"));
            }

            ImGui::EndTable();
        }

        // --- 計器(一覧・キューの使った量の最大と容量)---
        void BuildGauges(const gpu::GraphStatsLayout& layout, const GraphPanelStatus& status) {
            const size_t count = std::min(layout.gauges.size(), status.stats.gaugePeaks.size());
            for (size_t index = 0; index < count; ++index) {
                const gpu::GraphGaugeLimits& gauge = layout.gauges[index];
                const uint32_t peak = status.stats.gaugePeaks[index];
                const float fraction = gauge.capacity > 0
                                           ? static_cast<float>(peak) / static_cast<float>(gauge.capacity)
                                           : 0.0f;
                ImGui::ProgressBar(std::min(fraction, 1.0f), {160.0f, 0.0f},
                                   std::format("{} / {}", peak, gauge.capacity).c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(gauge.name.c_str());
            }
        }

        // --- 上限に当たった・近づいたもの(ログの Warning と同じ判定)---
        void BuildFindings(const gpu::GraphStatsLayout& layout, const GraphPanelStatus& status) {
            const std::vector<gpu::GraphFinding> findings = gpu::EvaluateGraphStats(layout, status.stats);
            if (findings.empty()) {
                ImGui::TextUnformatted("上限: 余裕あり");
                return;
            }

            for (const gpu::GraphFinding& finding : findings)
                ImGui::TextColored(WARNING_COLOR, "%s", finding.text.c_str());
        }

    }  // namespace

    void BuildGraphPanel(const GraphPanelStatus& status) {
        ImGui::SetNextWindowPos({PANEL_X_PIXELS, PANEL_Y_PIXELS}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Work Graphs と性能", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextUnformatted(std::format("直近 1 秒: {} 刻み", status.ticks).c_str());

        ImGui::SeparatorText("刻みの単位の GPU 時間");
        BuildUnitTable(status);

        if (status.layout != nullptr && !status.stats.nodes.empty()) {
            ImGui::SeparatorText(std::format("{} のノード", status.layout->name).c_str());
            BuildNodeTable(*status.layout, status);
            BuildGauges(*status.layout, status);
            BuildFindings(*status.layout, status);
        }

        ImGui::TextDisabled("ノードごとの時間は測れない(タイムスタンプは DispatchGraph の前後だけ)");
        ImGui::End();
    }

}  // namespace bicameral::editor
