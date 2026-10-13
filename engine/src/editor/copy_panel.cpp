// copy_panel.cpp — 「写しの早送り」のパネル(T-0412)。使い方は copy_panel.h。
#include "editor/copy_panel.h"

#include <algorithm>
#include <cfloat>
#include <format>
#include <vector>

#include <imgui.h>

namespace bicameral::editor {

    namespace {

        constexpr ImVec4 ERROR_COLOR = {1.0f, 0.4f, 0.4f, 1.0f};
        constexpr ImVec4 GOOD_COLOR = {0.5f, 0.9f, 0.5f, 1.0f};
        constexpr ImVec4 NOTE_COLOR = {0.7f, 0.7f, 0.7f, 1.0f};
        constexpr uint32_t MILLI = 1000;

        // 刻み → 「分 秒」(世界の時間)
        std::string WorldTime(uint64_t ticks, uint64_t ticksPerSecond) {
            const uint64_t seconds = ticks / std::max<uint64_t>(1, ticksPerSecond);
            return std::format("{} 分 {} 秒", seconds / 60, seconds % 60);
        }

    }  // namespace

    void CopyPanel::Build(const CopyProgress& progress, uint64_t ticksPerSecond) {
        ImGui::SetNextWindowPos({420.0f, 360.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({460.0f, 360.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("写しの早送り")) {
            ImGui::End();
            return;
        }

        ImGui::TextColored(NOTE_COLOR, "今の世界の写しを別に早送りして、条件に着く時間を測る(世界は変わらない)");
        ImGui::TextColored(NOTE_COLOR,
                           "写しにはコマンドを当てない(今から何もしなければ)。本物の刻みの余りの時間で回す");
        ImGui::Separator();
        BuildCondition();

        ImGui::BeginDisabled(progress.running);
        if (ImGui::Button("今の世界の写しで早送り")) {
            m_request = CopyRequest{
                .maxTicks = static_cast<uint64_t>(std::max(m_maxSeconds, 1)) * ticksPerSecond,
                .sampleIntervalTicks = static_cast<uint32_t>(std::max(m_intervalTicks, 1)),
                .condition = static_cast<CopyCondition>(m_condition),
                .temperatureMillikelvin = static_cast<uint32_t>(std::max(m_temperatureKelvin, 0)) * MILLI,
                .species = static_cast<uint32_t>(m_speciesIndex + 1),
                .amountMicromoles = static_cast<uint64_t>(std::max(m_amountMillimoles, 0)) * MILLI};
        }

        ImGui::EndDisabled();
        if (progress.running) {
            ImGui::SameLine();
            if (ImGui::Button("止める"))
                m_cancel = true;
        }

        ImGui::Separator();
        BuildResult(progress, ticksPerSecond);
        ImGui::End();
    }

    void CopyPanel::BuildCondition() {
        const char* conditions[] = {"上限まで(最高温度を測る)", "最高温度がしきい値より下になるまで",
                                    "物質の量がしきい値以下になるまで"};
        ImGui::Combo("止める条件", &m_condition, conditions, IM_ARRAYSIZE(conditions));
        ImGui::InputInt("上限 (秒)", &m_maxSeconds);
        ImGui::InputInt("測る間隔 (刻み)", &m_intervalTicks);
        if (m_condition == static_cast<int>(CopyCondition::BelowTemperature))
            ImGui::InputInt("しきい値 (K)", &m_temperatureKelvin);

        if (m_condition != static_cast<int>(CopyCondition::SpeciesAtOrBelow))
            return;

        const std::vector<std::string>* names = m_table ? &m_table->table.speciesNames : nullptr;
        const int count = names != nullptr ? static_cast<int>(names->size()) - 1 : 0;
        if (count <= 0) {
            ImGui::TextColored(ERROR_COLOR, "今の表に物質が無い");
            return;
        }

        m_speciesIndex = std::clamp(m_speciesIndex, 0, count - 1);
        if (ImGui::BeginCombo("物質", (*names)[static_cast<size_t>(m_speciesIndex) + 1].c_str())) {
            for (int index = 0; index < count; ++index) {
                if (ImGui::Selectable((*names)[static_cast<size_t>(index) + 1].c_str(), index == m_speciesIndex))
                    m_speciesIndex = index;
            }

            ImGui::EndCombo();
        }

        ImGui::InputInt("しきい値 (mmol)", &m_amountMillimoles);
    }

    void CopyPanel::BuildResult(const CopyProgress& progress, uint64_t ticksPerSecond) const {
        if (!progress.message.empty())
            ImGui::TextColored(progress.failed ? ERROR_COLOR : NOTE_COLOR, "%s", progress.message.c_str());

        if (progress.running) {
            const uint64_t done = progress.tick - progress.startTick;
            ImGui::Text("早送り中: 刻み %llu → %llu(%s / %s)", static_cast<unsigned long long>(progress.startTick),
                        static_cast<unsigned long long>(progress.tick), WorldTime(done, ticksPerSecond).c_str(),
                        WorldTime(progress.maxTicks, ticksPerSecond).c_str());
            ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(std::max<uint64_t>(1, progress.maxTicks)));
        } else if (progress.finished && !progress.failed) {
            const uint64_t elapsed = progress.endTick - progress.startTick;
            ImGui::TextColored(progress.reached ? GOOD_COLOR : NOTE_COLOR, "%s: 刻み %llu から %llu 刻み = %s",
                               progress.reached ? "条件に着いた" : "上限まで着かなかった",
                               static_cast<unsigned long long>(progress.startTick),
                               static_cast<unsigned long long>(elapsed), WorldTime(elapsed, ticksPerSecond).c_str());
        }

        if (progress.vramBytes > 0) {
            ImGui::TextColored(NOTE_COLOR, "写しの VRAM %llu MiB・1 刻み %.1f µs・予算の余りを待ったフレーム %llu",
                               static_cast<unsigned long long>(progress.vramBytes >> 20), progress.tickGpuMicroseconds,
                               static_cast<unsigned long long>(progress.waitedFrames));
        }

        if (progress.samples.empty())
            return;

        // 測った値の道のり(表示だけ。浮動小数点はエディタの表示に使ってよい)
        std::vector<float> values;
        values.reserve(progress.samples.size());
        for (const auto& [tick, value] : progress.samples)
            values.push_back(static_cast<float>(value) / 1000.0f);  // mK → K・µmol → mmol

        const auto& [lastTick, lastValue] = progress.samples.back();
        const std::string label = std::format("{} {:.1f} {}(刻み {})", progress.measuresAmount ? "量" : "最高温度",
                                              static_cast<double>(lastValue) / 1000.0,
                                              progress.measuresAmount ? "mmol" : "K", lastTick);
        ImGui::PlotLines("##copy", values.data(), static_cast<int>(values.size()), 0, label.c_str(), FLT_MAX, FLT_MAX,
                         {440.0f, 120.0f});
    }

}  // namespace bicameral::editor
