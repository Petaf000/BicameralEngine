// brush_panel.cpp — 世界に物を置く筆のパネル(T-0222・D-449)。流れは brush_panel.h。
#include "editor/brush_panel.h"

#include <algorithm>
#include <array>
#include <format>

#include <imgui.h>

#include "core/log.h"

namespace bicameral::editor {
    namespace {

        constexpr int MAX_TEMPERATURE_KELVIN = 6000;  // common/probe_place.hlsli の温度の上限
        constexpr int MAX_AMOUNT_PERCENT = 400;

        // --- --auto-place の筆の運び(フレーム・材料・中心・半径・置き換えか)---
        // 木箱の左の空気に木の球を置く → その中心に木炭を足す → 格子の角にはみ出す 1 セル → 格子の端からはみ出す大きな空気の足し
        // → 半径が上限を超える(当たらない)→ 木の球に火を付ける(つつき)。当たるのは 4 つ
        struct AutoStroke {
            uint64_t frame;
            std::string_view material;
            sim::ProbePlaceShape shape;
            bool expectApplied;
        };

        constexpr uint32_t AUTO_CENTER_X = 20;
        constexpr uint32_t AUTO_CENTER_Y = 32;
        constexpr uint64_t AUTO_IGNITE_FRAME = 16;

        constexpr std::array<AutoStroke, 5> AUTO_STROKES = {{
            {.frame = 6,
             .material = "木",
             .shape = {.x = AUTO_CENTER_X, .y = AUTO_CENTER_Y, .z = sim::PROBE_VIEW_Z, .radius = 3},
             .expectApplied = true},
            {.frame = 8,
             .material = "木炭",
             .shape = {.x = AUTO_CENTER_X, .y = AUTO_CENTER_Y, .z = sim::PROBE_VIEW_Z, .radius = 1, .replace = false},
             .expectApplied = true},
            {.frame = 10,
             .material = "二酸化炭素",
             .shape = {.x = 0, .y = 0, .z = 0, .radius = 0, .temperatureMilliKelvin = 900000},
             .expectApplied = true},
            {.frame = 12,
             .material = "空気",
             .shape = {.x = 63, .y = 63, .z = 40, .radius = sim::PROBE_PLACE_MAX_RADIUS, .replace = false},
             .expectApplied = true},
            {.frame = 14,
             .material = "木",
             .shape = {.x = 40, .y = 10, .z = 10, .radius = sim::PROBE_PLACE_MAX_RADIUS + 1},
             .expectApplied = false},
        }};

    }  // namespace

    void BrushPanel::UseTable(std::shared_ptr<const sim::BakedReactionTable> table, uint64_t version) {
        if (table == nullptr || (m_table != nullptr && version == m_tableVersion))
            return;

        m_table = std::move(table);
        m_tableVersion = version;
        m_materials = sim::MakeLabMaterials(*m_table);
    }

    const sim::LabMaterial* BrushPanel::FindMaterial(std::string_view name) const {
        const auto found = std::ranges::find(m_materials, name, &sim::LabMaterial::name);

        return found != m_materials.end() ? &*found : nullptr;
    }

    // --- コマンド ---

    sim::ProbeCommand BrushPanel::MakeCommandWith(const sim::LabMaterial& material, const sim::ProbePlaceShape& shape,
                                                  uint32_t amountPercent, uint32_t sequence) const {
        std::vector<sim::SpeciesAmount> contents = material.contents;
        for (sim::SpeciesAmount& entry : contents)
            entry.amount = entry.amount * amountPercent / 100;

        return sim::MakePlaceCommand(0, sequence, shape, contents);
    }

    sim::ProbeCommand BrushPanel::MakeCommand(uint32_t x, uint32_t y, uint32_t z, uint32_t sequence) const {
        const sim::LabMaterial* material = FindMaterial(m_materialName);
        if (material == nullptr)
            material = &m_materials.front();  // Holding() の時だけ呼ばれる(一覧は空でない)

        const sim::ProbePlaceShape shape{.x = x,
                                         .y = y,
                                         .z = z,
                                         .radius = static_cast<uint32_t>(m_radiusCells),
                                         .replace = m_replace,
                                         .temperatureMilliKelvin = static_cast<uint32_t>(m_temperatureKelvin) * 1000};

        return MakeCommandWith(*material, shape, static_cast<uint32_t>(m_amountPercent), sequence);
    }

    void BrushPanel::NotePlaced(const sim::ProbeEvent& event) {
        m_placedCount += 1;
        m_lastPlaced = std::format("刻み {} に ({}, {}, {}) を中心に置いた", event.tick, event.PokeX(), event.PokeY(),
                                   event.PokeZ());
        Log(Channel::Sim, Level::Info, "筆: {}", m_lastPlaced);
    }

    // --- パネル ---

    void BrushPanel::Build() {
        ImGui::SetNextWindowPos({16.0f, 520.0f}, ImGuiCond_FirstUseEver);
        ImGui::Begin("筆(世界に置く)", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        if (m_materials.empty()) {
            ImGui::TextUnformatted("反応表に置ける材料が無い");
            ImGui::End();
            return;
        }

        ImGui::Checkbox("筆を持つ(左クリックで置く。外すとつつき)", &m_holding);

        // --- 材料(今の反応表にある物質だけ)---
        if (ImGui::BeginCombo("材料", m_materialName.c_str())) {
            for (const sim::LabMaterial& material : m_materials) {
                if (ImGui::Selectable(std::string(material.name).c_str(), material.name == m_materialName))
                    m_materialName = material.name;
            }

            ImGui::EndCombo();
        }

        // --- 形と量 ---
        ImGui::SliderInt("半径(セル)", &m_radiusCells, 0, static_cast<int>(sim::PROBE_PLACE_MAX_RADIUS));
        ImGui::SliderInt("量(%)", &m_amountPercent, 1, MAX_AMOUNT_PERCENT);
        ImGui::SliderInt("温度(K)", &m_temperatureKelvin, 1, MAX_TEMPERATURE_KELVIN);
        if (ImGui::RadioButton("置き換え", m_replace))
            m_replace = true;

        ImGui::SameLine();
        if (ImGui::RadioButton("足す", !m_replace))
            m_replace = false;

        ImGui::Text("置いた回数 %u", m_placedCount);
        if (!m_lastPlaced.empty())
            ImGui::TextUnformatted(m_lastPlaced.c_str());

        ImGui::End();
    }

    // --- 人がいない確認(--auto-place)---

    std::vector<sim::ProbeCommand> BrushPanel::TakeAutoCommands(uint64_t frame, uint32_t& sequence) {
        std::vector<sim::ProbeCommand> commands;
        if (!m_autoRunning)
            return commands;

        for (const AutoStroke& stroke : AUTO_STROKES) {
            if (stroke.frame != frame)
                continue;

            const sim::LabMaterial* material = FindMaterial(stroke.material);
            if (material == nullptr) {
                Log(Channel::Sim, Level::Error, "--auto-place: 材料「{}」が反応表に無い", stroke.material);
                continue;
            }

            commands.push_back(MakeCommandWith(*material, stroke.shape, 100, sequence++));
            m_autoSent += 1;
            m_autoExpected += stroke.expectApplied ? 1 : 0;
        }

        if (frame == AUTO_IGNITE_FRAME)
            commands.push_back(sim::MakePokeCommand(0, sequence++, AUTO_CENTER_X, AUTO_CENTER_Y, sim::PROBE_VIEW_Z));

        return commands;
    }

    bool BrushPanel::AutoPassed() const {
        return m_autoSent == AUTO_STROKES.size() && m_placedCount == m_autoExpected;
    }

    std::string BrushPanel::AutoSummary() const {
        return std::format("投げた {} / {}・当たった {}(当たるはず {})", m_autoSent, AUTO_STROKES.size(), m_placedCount,
                           m_autoExpected);
    }

}  // namespace bicameral::editor
