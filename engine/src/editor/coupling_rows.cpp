// coupling_rows.cpp — 反応表のパネルの「結合」の欄(coupling_rows.h。T-0536・ADR-0238)。
// 表示だけなので浮動小数点を使う(エディタ。シミュの値は整数のまま読む)。
#include "editor/coupling_rows.h"

#include <cmath>
#include <format>
#include <string>

#include <imgui.h>

namespace bicameral::editor {

    namespace {

        constexpr double Q16 = 65536.0;
        constexpr double GAS_CONSTANT = 8.314462618;  // J/(mol·K)
        constexpr double SHOWN_KELVINS = 300.0;       // 止まる坂を圧力の比で見せる温度
        constexpr double TICKS_PER_SECOND = 60.0;
        constexpr ImVec4 WARNING_COLOR = {1.0f, 0.8f, 0.3f, 1.0f};
        constexpr ImVec4 NOTE_COLOR = {0.7f, 0.7f, 0.7f, 1.0f};

        // 押す構造の止まる坂(蓄えが仕事なら q·w。気体を押すなら 300 K での圧力の比 exp(q·w ÷ RT))
        std::string StopSlope(const sim::Coupling& coupling) {
            if (coupling.form != sim::CouplingForm::Machine || coupling.disabled)
                return "-";

            const double costJoulesPerMole = static_cast<double>(coupling.costQ16) / Q16 * 1000.0;
            const double q = static_cast<double>(coupling.couplingQ16) / Q16;
            if (coupling.driver.kind == sim::CouplingQuantityKind::Heat)
                return "熱機関: Carnot × η_max";

            if (coupling.driver.kind != sim::CouplingQuantityKind::Work)
                return std::format("q·w·f_X = {:.0f} J/mol × f_X", q * costJoulesPerMole);

            if (coupling.driven.kind == sim::CouplingQuantityKind::Gas)
                return std::format("圧力の比 {:.3f}(300 K)",
                                   std::exp(q * costJoulesPerMole / (GAS_CONSTANT * SHOWN_KELVINS)));

            return std::format("{:.0f} J/mol", q * costJoulesPerMole);
        }

        std::string MachineNumbers(const sim::Coupling& coupling) {
            if (coupling.form != sim::CouplingForm::Machine)
                return "-";

            if (coupling.disabled)
                return "止めた(費用 ≤ 0・速さ < 0)";

            const double costJoulesPerMole = static_cast<double>(coupling.costQ16) / Q16 * 1000.0;
            const double speedWatts = static_cast<double>(coupling.speedPerTick) / 1000.0 * TICKS_PER_SECOND;

            return std::format("w {:.1f} J/mol・P {:.1f} W", costJoulesPerMole, speedWatts);
        }

    }  // namespace

    void BuildCouplingRows(const sim::BakedReactionTable& table) {
        ImGui::TextColored(
            NOTE_COLOR,
            "分類 drags(引きずり)と machines(押す構造)は同じ形: 結合の強さ q。|q| > 1 はベイクが ±1 に押さえる");
        if (table.couplings.empty()) {
            ImGui::TextColored(NOTE_COLOR, "結合が無い(パッケージの分類 drags・machines)");
            return;
        }

        constexpr ImGuiTableFlags FLAGS = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
        if (!ImGui::BeginTable("##couplings", 6, FLAGS))
            return;

        ImGui::TableSetupColumn("分類.鍵");
        ImGui::TableSetupColumn("X → Y");
        ImGui::TableSetupColumn("q(書いた → 効く)");
        ImGui::TableSetupColumn("η_max");
        ImGui::TableSetupColumn("費用・速さ");
        ImGui::TableSetupColumn("止まる坂");
        ImGui::TableHeadersRow();
        for (const sim::Coupling& coupling : table.couplings) {
            const bool machine = coupling.form == sim::CouplingForm::Machine;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s.%s", machine ? "machines" : "drags", coupling.key.c_str());
            ImGui::TableNextColumn();
            std::string flows = sim::CouplingQuantityName(coupling.driver, table) + " → " +
                                sim::CouplingQuantityName(coupling.driven, table);
            if (coupling.withinSpecies != 0)
                flows += " (" + table.speciesNames[coupling.withinSpecies] + " の中)";

            ImGui::TextUnformatted(flows.c_str());
            ImGui::TableNextColumn();
            const double written = static_cast<double>(coupling.writtenCouplingQ16) / Q16;
            const double used = static_cast<double>(coupling.couplingQ16) / Q16;
            if (coupling.writtenCouplingQ16 != coupling.couplingQ16)
                ImGui::TextColored(WARNING_COLOR, "%.4f → %.4f(押さえた)", written, used);
            else
                ImGui::Text("%.4f", used);

            ImGui::TableNextColumn();
            ImGui::Text("%.4f", static_cast<double>(coupling.peakEfficiencyQ16) / Q16);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(MachineNumbers(coupling).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(StopSlope(coupling).c_str());
        }

        ImGui::EndTable();
    }

}  // namespace bicameral::editor
