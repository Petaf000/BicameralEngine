// play_metric_panel.cpp — 「遊びの指標」のパネル(T-0432)。使い方は play_metric_panel.h。
#include "editor/play_metric_panel.h"

#include <format>

#include <imgui.h>

namespace bicameral::editor {

    namespace {

        constexpr ImVec4 ERROR_COLOR = {1.0f, 0.4f, 0.4f, 1.0f};
        constexpr ImVec4 CHANGED_COLOR = {1.0f, 0.8f, 0.3f, 1.0f};
        constexpr ImVec4 NOTE_COLOR = {0.7f, 0.7f, 0.7f, 1.0f};

        std::string FormatValue(const sim::PlayMetricValue& value, uint64_t ticksPerSecond) {
            if (!value.measured)
                return "測れない";

            return std::format("{}{}", value.reached ? "" : "上限 ",
                               sim::FormatTicksAsSeconds(value.ticks, ticksPerSecond));
        }

        std::string FormatDelta(const sim::PlayMetricDiff& diff, uint64_t ticksPerSecond) {
            if (!diff.current || !diff.previous)
                return diff.current ? "(初めて)" : "(今の表に無い)";

            if (!diff.changed)
                return "同じ";

            const uint64_t magnitude = static_cast<uint64_t>(diff.deltaTicks < 0 ? -diff.deltaTicks : diff.deltaTicks);
            return std::format("{}{}・量 {:+}", diff.deltaTicks < 0 ? "−" : "+",
                               sim::FormatTicksAsSeconds(magnitude, ticksPerSecond), diff.deltaQuantity);
        }

    }  // namespace

    void PlayMetricPanel::Build(const PlayMetricProgress& progress, uint64_t ticksPerSecond) {
        ImGui::SetNextWindowPos({900.0f, 360.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({480.0f, 300.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("遊びの指標")) {
            ImGui::End();
            return;
        }

        ImGui::TextColored(NOTE_COLOR, "パッケージの play_metrics の場面を写しで早送りして測り、前の版と比べる");
        ImGui::TextColored(NOTE_COLOR, "場面は表の初めの世界(今の世界ではない)。本物の刻みの余りの時間で回す");
        ImGui::Separator();

        ImGui::Checkbox("表を変えたら測る", &m_autoMeasure);
        ImGui::SameLine();
        ImGui::BeginDisabled(progress.running);
        if (ImGui::Button("今の表で測る"))
            m_request = true;

        ImGui::EndDisabled();

        if (progress.running) {
            ImGui::Text("測っている: %zu / %zu  %s  %llu / %llu 刻み(待ったフレーム %llu)", progress.metricIndex + 1,
                        progress.metricCount, progress.metricLabel.c_str(),
                        static_cast<unsigned long long>(progress.tick),
                        static_cast<unsigned long long>(progress.maxTicks),
                        static_cast<unsigned long long>(progress.waitedFrames));
        }

        if (!progress.message.empty())
            ImGui::TextColored(ERROR_COLOR, "%s", progress.message.c_str());

        BuildDiffs(progress, ticksPerSecond);
        ImGui::End();
    }

    void PlayMetricPanel::BuildDiffs(const PlayMetricProgress& progress, uint64_t ticksPerSecond) const {
        if (!progress.hasResult)
            return;

        ImGui::Text("表の版 %016llx(前の版 %016llx)", static_cast<unsigned long long>(progress.tableVersion),
                    static_cast<unsigned long long>(progress.previousTableVersion));
        if (progress.diffs.empty()) {
            ImGui::TextColored(NOTE_COLOR, "登録が無い(パッケージの分類 play_metrics)");
            return;
        }

        if (!ImGui::BeginTable("play_metrics", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            return;

        ImGui::TableSetupColumn("指標");
        ImGui::TableSetupColumn("今");
        ImGui::TableSetupColumn("前の版");
        ImGui::TableSetupColumn("差");
        ImGui::TableHeadersRow();
        for (const sim::PlayMetricDiff& diff : progress.diffs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(diff.label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(diff.current ? FormatValue(*diff.current, ticksPerSecond).c_str() : "-");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(diff.previous ? FormatValue(*diff.previous, ticksPerSecond).c_str() : "-");
            ImGui::TableNextColumn();
            const std::string delta = FormatDelta(diff, ticksPerSecond);
            if (diff.changed && diff.current && diff.previous)
                ImGui::TextColored(CHANGED_COLOR, "%s", delta.c_str());
            else
                ImGui::TextUnformatted(delta.c_str());
        }

        ImGui::EndTable();
    }

}  // namespace bicameral::editor
