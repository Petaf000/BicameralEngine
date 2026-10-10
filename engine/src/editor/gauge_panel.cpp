// gauge_panel.cpp — エディタの「計器と比べる」パネル(gauge_panel.h)。値は sim の整数のまま受け取り、描く時だけ浮動小数点に直す。
#include "editor/gauge_panel.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

#include <imgui.h>

#include "common/units.hlsli"
#include "core/log.h"
#include "editor/lab_panel.h"

namespace bicameral::editor {

    namespace {

        constexpr double MILLIKELVIN_PER_KELVIN = 1e3;
        constexpr double MICROMOLES_PER_MOLE = 1e6;
        constexpr double TICKS_PER_SECOND = fx::TICKS_PER_SECOND;  // 1 刻み = 1/60 s(D-409)
        constexpr ImVec2 GRAPH_SIZE = {460.0f, 150.0f};
        constexpr ImU32 COLOR_A = IM_COL32(255, 170, 60, 255);      // 実験 A(と今の実験)
        constexpr ImU32 COLOR_B = IM_COL32(90, 200, 255, 255);      // 実験 B
        constexpr ImU32 COLOR_MARK = IM_COL32(200, 200, 200, 160);  // 保存点の縦の線
        constexpr int MAX_COMPARE_TICKS = 2000;

        // --auto-lab-compare の実験(木を燃やし、刻み 20 を保存点に隣へ冷たい木を足す。熱い木は 1 刻みで燃え尽きることがある)
        constexpr uint32_t AUTO_IGNITE_TICK = 3;
        constexpr uint32_t AUTO_TICKS = 40;
        constexpr uint64_t AUTO_SAVE_POINT_TICK = 20;
        constexpr uint32_t AUTO_ROOM_MILLIKELVIN = 300000;
        constexpr uint32_t AUTO_IGNITE_MILLIKELVIN = 1500000;
        constexpr sim::LabCellPosition AUTO_CELL = {.x = 3, .y = 3, .z = 3};
        constexpr sim::LabCellPosition AUTO_ADDED_CELL = {.x = 4, .y = 3, .z = 3};

        constexpr std::array<const char*, 6> QUANTITY_NAMES = {"セルの温度 (K)",           "箱の最高温度 (K)",
                                                               "セルの物質量 (mol)",       "箱の物質量 (mol)",
                                                               "セルの反応の速さ (mol/s)", "箱の反応の速さ (mol/s)"};

        bool NeedsSpecies(GaugeQuantity quantity) {
            return quantity != GaugeQuantity::CellTemperature && quantity != GaugeQuantity::MaxTemperature;
        }

        // 1 刻みの値を表示の単位に(物質が範囲の外なら 0)
        float ValueAt(std::span<const sim::LabGaugeSample> samples, size_t index, GaugeQuantity quantity,
                      uint32_t species) {
            const sim::LabGaugeSample& sample = samples[index];
            const auto amountOf = [&](const std::vector<uint64_t>& amounts) {
                return species < amounts.size() ? static_cast<double>(amounts[species]) / MICROMOLES_PER_MOLE : 0.0;
            };
            const auto rateOf = [&](bool wholeBox) {
                return static_cast<double>(sim::LabGaugeRate(samples, index, species, wholeBox)) * TICKS_PER_SECOND /
                       MICROMOLES_PER_MOLE;
            };

            switch (quantity) {
                case GaugeQuantity::CellTemperature:
                    return static_cast<float>(sample.cellTemperatureMilliKelvin / MILLIKELVIN_PER_KELVIN);
                case GaugeQuantity::MaxTemperature:
                    return static_cast<float>(sample.maxTemperatureMilliKelvin / MILLIKELVIN_PER_KELVIN);
                case GaugeQuantity::CellAmount: return static_cast<float>(amountOf(sample.cellAmounts));
                case GaugeQuantity::BoxAmount: return static_cast<float>(amountOf(sample.boxAmounts));
                case GaugeQuantity::CellRate: return static_cast<float>(rateOf(false));
                case GaugeQuantity::BoxRate: return static_cast<float>(rateOf(true));
            }

            return 0.0f;
        }

        std::vector<float> ValuesOf(std::span<const sim::LabGaugeSample> samples, GaugeQuantity quantity,
                                    uint32_t species) {
            std::vector<float> values;
            values.reserve(samples.size());
            for (size_t i = 0; i < samples.size(); ++i)
                values.push_back(ValueAt(samples, i, quantity, species));

            return values;
        }

        // 1 本か 2 本の線を同じ縦軸で重ねて描く。markIndex は縦の線(保存点)。firstTick は左端の刻み
        void DrawSeries(const char* id, std::span<const float> a, std::span<const float> b,
                        std::optional<size_t> markIndex, uint64_t firstTick) {
            const size_t count = std::max(a.size(), b.size());
            float low = std::numeric_limits<float>::max();
            float high = std::numeric_limits<float>::lowest();
            for (const std::span<const float> series : {a, b}) {
                for (const float value : series) {
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
            }

            if (count < 2 || low > high) {
                ImGui::TextDisabled("(2 刻み以上流すと線が出る)");
                return;
            }

            if (high - low < 1e-9f)
                high = low + 1.0f;

            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(id, GRAPH_SIZE);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(origin, {origin.x + GRAPH_SIZE.x, origin.y + GRAPH_SIZE.y}, IM_COL32(20, 20, 24, 255));

            const auto pointOf = [&](size_t index, float value) {
                const float x = origin.x + (GRAPH_SIZE.x * static_cast<float>(index) / static_cast<float>(count - 1));
                const float y = origin.y + (GRAPH_SIZE.y * (1.0f - ((value - low) / (high - low))));
                return ImVec2{x, y};
            };
            const auto drawLine = [&](std::span<const float> series, ImU32 color) {
                for (size_t i = 1; i < series.size(); ++i)
                    draw->AddLine(pointOf(i - 1, series[i - 1]), pointOf(i, series[i]), color, 1.5f);
            };

            if (markIndex && *markIndex < count) {
                const ImVec2 top = pointOf(*markIndex, high);
                draw->AddLine(top, {top.x, origin.y + GRAPH_SIZE.y}, COLOR_MARK);
            }

            drawLine(a, COLOR_A);
            drawLine(b, COLOR_B);
            ImGui::Text("縦軸 %.6g 〜 %.6g", static_cast<double>(low), static_cast<double>(high));

            // --- 指した刻みの値 ---
            if (!ImGui::IsItemHovered() && !ImGui::IsItemActive())
                return;

            const float ratio = std::clamp((ImGui::GetIO().MousePos.x - origin.x) / GRAPH_SIZE.x, 0.0f, 1.0f);
            const auto index = static_cast<size_t>(std::lround(ratio * static_cast<float>(count - 1)));
            std::string text = std::format("刻み {}", firstTick + index);
            if (index < a.size())
                text += std::format("\nA {:.6g}", a[index]);

            if (index < b.size())
                text += std::format("\nB {:.6g}", b[index]);

            ImGui::SetTooltip("%s", text.c_str());
        }

        // --auto-lab-compare の比べた結果を確かめる: A は元の実験と同じ・B は保存点の刻みから違う(足した木の分だけ箱の
        // セルロースが多い)・B のグラフの値も CPU リファレンスだけで B の記録を流した値と同じ
        std::expected<std::string, std::string> CheckAutoComparison(const sim::LabComparison& comparison,
                                                                    std::span<const sim::LabGaugeSample> original,
                                                                    const sim::BakedReactionTable& table) {
            if (!std::ranges::equal(comparison.a, original))
                return std::unexpected("保存点から流した A が元の実験と違う");

            if (comparison.firstDifference != AUTO_SAVE_POINT_TICK)
                return std::unexpected(std::format("A と B が保存点の刻み {} から違わない(最初に違った刻み {})",
                                                   AUTO_SAVE_POINT_TICK,
                                                   comparison.firstDifference.value_or(UINT64_MAX)));

            const auto cpu = sim::RunLabGaugeOnCpu(comparison.recordingB, table, comparison.gaugeCell);
            if (!cpu || *cpu != comparison.b)
                return std::unexpected("B のグラフの値が CPU リファレンスと違う");

            const uint32_t cellulose = table.SpeciesId("cellulose");
            const auto savePoint = static_cast<size_t>(AUTO_SAVE_POINT_TICK);
            const uint64_t celluloseA = comparison.a[savePoint].boxAmounts.at(cellulose);
            const uint64_t celluloseB = comparison.b[savePoint].boxAmounts.at(cellulose);
            if (celluloseB <= celluloseA)
                return std::unexpected("B に足した木が箱のセルロースに出ない");

            return std::format(
                "{} 刻みのグラフが CPU と一致・保存点 {} から A は元の実験と同じ、B は刻み {} から違う"
                "(刻み 20 の箱のセルロース A {} / B {} µmol・最後の最高温度 A {} / B {} mK)",
                original.size(), AUTO_SAVE_POINT_TICK, *comparison.firstDifference, celluloseA, celluloseB,
                comparison.a.back().maxTemperatureMilliKelvin, comparison.b.back().maxTemperatureMilliKelvin);
        }

    }  // namespace

    // --- パネル ---

    void GaugePanel::Build(LabPanel& lab) {
        if (m_autoPending) {
            m_autoPending = false;
            RunAuto(lab);
        }

        ImGui::SetNextWindowPos({500.0f, 420.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("計器と比べる", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            return;
        }

        sim::LabSession* session = lab.Session();
        if (session == nullptr) {
            ImGui::TextUnformatted(
                "実験室の箱の値を時間のグラフで見る・2 つの実験を並べて比べる(「実験室」で箱を作る)");
            ImGui::End();
            return;
        }

        BuildGauge(*session);
        BuildCompare(lab, *session);
        ImGui::End();
    }

    void GaugePanel::BuildQuantityControls(const sim::BakedReactionTable& table) {
        int quantity = static_cast<int>(m_quantity);
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo("量", &quantity, QUANTITY_NAMES.data(), static_cast<int>(QUANTITY_NAMES.size()));
        m_quantity = static_cast<GaugeQuantity>(quantity);
        if (!NeedsSpecies(m_quantity))
            return;

        // --- 物質(表の物質。名前の無い ID は出さない)---
        const auto species = static_cast<size_t>(m_species);
        const char* preview = species < table.speciesNames.size() ? table.speciesNames[species].c_str() : "?";
        ImGui::SetNextItemWidth(220.0f);
        if (!ImGui::BeginCombo("物質", preview))
            return;

        for (size_t id = 0; id < table.speciesNames.size(); ++id) {
            if (!table.speciesNames[id].empty() && ImGui::Selectable(table.speciesNames[id].c_str(), id == species))
                m_species = static_cast<int>(id);
        }

        ImGui::EndCombo();
    }

    void GaugePanel::BuildGauge(sim::LabSession& session) {
        ImGui::SeparatorText("計器(GPU から読み戻した箱の値。刻みごと)");
        const int maxCell = static_cast<int>(sim::LAB_BOX_EDGE) - 1;
        ImGui::SliderInt3("見るセル", m_cell.data(), 0, maxCell);
        session.SetGaugeCell(multires::MrCellIndex(static_cast<uint32_t>(m_cell[0]), static_cast<uint32_t>(m_cell[1]),
                                                   static_cast<uint32_t>(m_cell[2])));
        BuildQuantityControls(session.Table());
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderInt("直近の刻み", &m_windowTicks, 10, static_cast<int>(sim::LAB_GAUGE_MAX_SAMPLES));

        const std::vector<sim::LabGaugeSample>& gauge = session.Gauge();
        const size_t shown = std::min(gauge.size(), static_cast<size_t>(m_windowTicks));
        const std::span<const sim::LabGaugeSample> recent = std::span(gauge).last(shown);
        const std::vector<float> values = ValuesOf(recent, m_quantity, static_cast<uint32_t>(m_species));
        DrawSeries("##gauge", values, {}, std::nullopt, recent.empty() ? 0 : recent.front().tick);
        if (!values.empty())
            ImGui::Text("刻み %llu: %.6g", static_cast<unsigned long long>(recent.back().tick),
                        static_cast<double>(values.back()));
    }

    void GaugePanel::BuildCompare(LabPanel& lab, sim::LabSession& session) {
        ImGui::SeparatorText("比べる(同じ保存点から A = 元の実験の続き・B = 条件を 1 つ足す)");
        const auto nextTick = static_cast<int>(
            std::min<uint64_t>(session.NextTick(), uint64_t{MAX_COMPARE_TICKS} * 10));
        m_savePointTick = std::clamp(m_savePointTick, 0, nextTick);
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderInt("保存点の刻み", &m_savePointTick, 0, nextTick);
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderInt("流す刻みの数", &m_compareTicks, 1, MAX_COMPARE_TICKS);
        ImGui::TextUnformatted("B に足す条件(「実験室」で今組んでいる材料・温度・範囲):");
        ImGui::SameLine();
        int kind = m_changeTemperatureOnly ? 1 : 0;
        ImGui::RadioButton("置く", &kind, 0);
        ImGui::SameLine();
        ImGui::RadioButton("温度だけ", &kind, 1);
        m_changeTemperatureOnly = kind == 1;

        if (ImGui::Button("並べて流す"))
            RunComparison(lab, session);

        ImGui::SameLine();
        ImGui::BeginDisabled(!m_original.has_value());
        if (ImGui::Button("元の実験に戻す") && m_original) {
            const auto replayed = session.Replay(*m_original);
            m_message = replayed ? "比べる前の実験を流し直した" : "";
            m_error = replayed ? "" : replayed.error();
        }

        ImGui::EndDisabled();
        if (!m_error.empty())
            ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", m_error.c_str());

        if (!m_message.empty())
            ImGui::TextUnformatted(m_message.c_str());

        BuildComparisonGraph();
    }

    void GaugePanel::RunComparison(LabPanel& lab, sim::LabSession& session) {
        const std::optional<sim::Command> change = lab.ComposedCommand(m_changeTemperatureOnly);
        if (!change) {
            m_error = "B に足す条件が無い(「実験室」で材料を組む)";
            return;
        }

        // 比べる前の実験(置いてまだ刻んでいない操作は入らない)
        if (!m_original || session.NextTick() != m_original->tickCount || m_comparison)
            m_original = session.Recording();

        const sim::LabComparisonPlan plan = sim::MakeLabComparisonPlan(
            *m_original, static_cast<uint64_t>(m_savePointTick), static_cast<uint32_t>(m_compareTicks), *change);
        auto comparison = sim::RunLabComparison(session, plan);
        if (!comparison) {
            m_error = comparison.error();
            m_comparison.reset();
            return;
        }

        m_error.clear();
        m_comparison = std::move(*comparison);
        m_message = m_comparison->firstDifference
                        ? std::format("刻み {} から違う(箱は B の状態)", *m_comparison->firstDifference)
                        : std::string("A と B の計器の値は同じ(箱は B の状態)");
    }

    void GaugePanel::BuildComparisonGraph() {
        if (!m_comparison)
            return;

        const auto species = static_cast<uint32_t>(m_species);
        const std::vector<float> a = ValuesOf(m_comparison->a, m_quantity, species);
        const std::vector<float> b = ValuesOf(m_comparison->b, m_quantity, species);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COLOR_A), "A");
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COLOR_B), "B");
        ImGui::SameLine();
        ImGui::Text("(%s・保存点 刻み %llu・見るセル %u)", QUANTITY_NAMES[static_cast<size_t>(m_quantity)],
                    static_cast<unsigned long long>(m_comparison->savePointTick), m_comparison->gaugeCell);
        DrawSeries("##compare", a, b, static_cast<size_t>(m_comparison->savePointTick), 0);
        if (!a.empty() && !b.empty())
            ImGui::Text("最後の刻み: A %.6g / B %.6g", static_cast<double>(a.back()), static_cast<double>(b.back()));
    }

    // --- 人がいない確認(--auto-lab-compare)---

    void GaugePanel::RunAuto(LabPanel& lab) {
        const auto result = AutoCompare(lab);
        m_autoPassed = result.has_value();
        m_autoSummary = result ? *result : result.error();
        if (result)
            Log(Channel::Sim, Level::Info, "--auto-lab-compare: {}。OK", *result);
        else
            Log(Channel::Sim, Level::Error, "--auto-lab-compare: 失敗({})", result.error());
    }

    std::expected<std::string, std::string> GaugePanel::AutoCompare(LabPanel& lab) {
        if (!lab.EnsureSession())
            return std::unexpected("実験室の箱を作れない");

        sim::LabSession& session = *lab.Session();
        const auto& materials = lab.Materials();
        const auto wood = std::ranges::find_if(materials, [](const sim::LabMaterial& m) { return m.name == "木"; });
        if (wood == materials.end())
            return std::unexpected("木の材料が無い");

        // --- A: 木を置いて火を付ける(グラフの値 = 読み戻した GPU の箱)---
        if (auto reset = session.Reset(); !reset)
            return std::unexpected(reset.error());

        session.SetGaugeCell(AUTO_CELL.Index());
        static_cast<void>(session.Place(AUTO_CELL, wood->contents, AUTO_ROOM_MILLIKELVIN));
        auto stepped = session.Step(AUTO_IGNITE_TICK);
        static_cast<void>(session.SetTemperature(AUTO_CELL, AUTO_IGNITE_MILLIKELVIN));
        if (stepped)
            stepped = session.Step(AUTO_TICKS - AUTO_IGNITE_TICK);

        if (!stepped)
            return std::unexpected(stepped.error());

        if (session.Mismatch())
            return std::unexpected(session.Mismatch()->what);

        const sim::LabRecording experiment = session.Recording();
        const std::vector<sim::LabGaugeSample> original = session.Gauge();
        const sim::BakedReactionTable& table = session.Table();
        const auto cpu = sim::RunLabGaugeOnCpu(experiment, table, AUTO_CELL.Index());
        if (!cpu)
            return std::unexpected(cpu.error());

        if (original.size() != AUTO_TICKS || *cpu != original)
            return std::unexpected(std::format("グラフの値が CPU リファレンスと違う(刻み {})",
                                               sim::FirstGaugeDifference(original, *cpu).value_or(original.size())));

        // --- 刻み 20 を保存点に、隣のセルへ冷たい木を足した B と比べる ---
        const sim::Command change = sim::MakeLabFillCommand(0, 0, AUTO_ADDED_CELL, wood->contents,
                                                            AUTO_ROOM_MILLIKELVIN);
        const sim::LabComparisonPlan plan = sim::MakeLabComparisonPlan(experiment, AUTO_SAVE_POINT_TICK,
                                                                       AUTO_TICKS - AUTO_SAVE_POINT_TICK, change);
        const auto comparison = sim::RunLabComparison(session, plan);
        if (!comparison)
            return std::unexpected(comparison.error());

        return CheckAutoComparison(*comparison, original, table);
    }

}  // namespace bicameral::editor
