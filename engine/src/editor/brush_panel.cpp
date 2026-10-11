// brush_panel.cpp — 世界に物を置く筆のパネル(T-0222・T-0245・D-449)。流れは brush_panel.h。
#include "editor/brush_panel.h"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>

#include <imgui.h>

#include "core/log.h"

namespace bicameral::editor {
    namespace {

        static_assert(lab::LAB_MAX_FILL_SPECIES == sim::PROBE_PLACE_MAX_SPECIES,
                      "材料の行の数 = 置くコマンドの物質の数");

        constexpr int MAX_TEMPERATURE_KELVIN = 6000;  // common/probe_place.hlsli の温度の上限
        constexpr auto MAX_SIZE = static_cast<int>(sim::PROBE_PLACE_MAX_RADIUS);

        // --- --auto-place の 1 回ずつの筆の運び(フレーム・材料・形・当たるはずか)---
        // 木箱の左の空気に木の球を置く → その中心に木炭を足す → 木の球に仮の魔素を足す(材料の一覧に魔素がある確認。T-0225)
        // → 格子の角にはみ出す 1 セル → 格子の端からはみ出す大きな空気の足し → 半径が上限を超える(当たらない)
        // → 木の球に火を付ける(つつき)。当たるのは 5 つ
        struct AutoStroke {
            uint64_t frame;
            std::string_view material;
            sim::ProbePlaceShape shape;
            bool expectApplied;
        };

        constexpr uint32_t AUTO_CENTER_X = 20;
        constexpr uint32_t AUTO_CENTER_Y = 32;
        constexpr uint64_t AUTO_IGNITE_FRAME = 16;

        constexpr std::array<AutoStroke, 6> AUTO_STROKES = {{
            {.frame = 6,
             .material = "木",
             .shape = {.x = AUTO_CENTER_X, .y = AUTO_CENTER_Y, .z = sim::PROBE_VIEW_Z, .radius = 3},
             .expectApplied = true},
            {.frame = 8,
             .material = "木炭",
             .shape = {.x = AUTO_CENTER_X, .y = AUTO_CENTER_Y, .z = sim::PROBE_VIEW_Z, .radius = 1, .replace = false},
             .expectApplied = true},
            {.frame = 9,
             .material = "魔素(試験)",
             .shape = {.x = AUTO_CENTER_X, .y = AUTO_CENTER_Y, .z = sim::PROBE_VIEW_Z, .radius = 2, .replace = false},
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

        // --- --auto-place のドラッグ(T-0245)---
        // フレーム 18: 木の直方体(半幅 1・2・0。置き換え)を (8, 44) で押し、(12, 44)・(20, 46) へ動かす。間隔 1 で 13 個のうち
        //   1 フレームの上限の 8 個 → 19: 離す(残りの 5 個を続きから)。22: 自由に組んだ材料(木炭を g で・仮の魔素を mol で。900 K)の
        //   z の軸の円柱(半径 1・厚さ 1)を足すで (40, 48) から (52, 48) へ。重ならない間隔 3 で 5 個。合わせて 18 個が当たる
        constexpr uint64_t AUTO_PAINT_BOX_FRAME = 18;
        constexpr uint64_t AUTO_PAINT_RELEASE_FRAME = 19;
        constexpr uint64_t AUTO_PAINT_CYLINDER_FRAME = 22;
        constexpr uint32_t AUTO_PAINT_STEPS = 3;
        constexpr uint32_t AUTO_PAINTED = 18;

        render::CellCoordinate AutoCell(uint32_t x, uint32_t y) {
            return {.x = x, .y = y, .z = sim::PROBE_VIEW_Z};
        }

        BrushCell ToBrushCell(const render::CellCoordinate& cell) {
            return {cell.x, cell.y, cell.z};
        }

    }  // namespace

    void BrushPanel::UseTable(std::shared_ptr<const sim::BakedReactionTable> table, uint64_t version) {
        if (table == nullptr || (m_table != nullptr && version == m_tableVersion))
            return;

        // --- 材料の行の物質 ID を名前で新しい表へ(初めの表なら木のひな形を読み込む)---
        const bool first = m_table == nullptr;
        if (!first)
            RemapMaterialRows(*m_table, *table, m_rows);

        m_table = std::move(table);
        m_tableVersion = version;
        m_materials = sim::MakeLabMaterials(*m_table);
        if (first && m_preset < m_materials.size())
            LoadMaterialRows(m_materials[m_preset], m_rows);
    }

    const sim::LabMaterial* BrushPanel::FindMaterial(std::string_view name) const {
        const auto found = std::ranges::find(m_materials, name, &sim::LabMaterial::name);

        return found != m_materials.end() ? &*found : nullptr;
    }

    // --- 形 ---

    uint32_t BrushPanel::TemperatureMilliKelvin() const {
        return static_cast<uint32_t>(std::clamp(m_temperatureKelvin, 1, MAX_TEMPERATURE_KELVIN)) * 1000u;
    }

    sim::ProbePlaceShape BrushPanel::MakeShape(const BrushCell& center) const {
        sim::ProbePlaceShape shape{.x = center[0],
                                   .y = center[1],
                                   .z = center[2],
                                   .radius = static_cast<uint32_t>(m_radiusCells),
                                   .replace = m_replace,
                                   .temperatureMilliKelvin = TemperatureMilliKelvin()};
        if (m_shape == ShapeKind::Box) {
            shape.kind = sim::PROBE_PLACE_SHAPE_BOX;
            shape.size2 = static_cast<uint32_t>(m_halfY);
            shape.size3 = static_cast<uint32_t>(m_halfZ);
        } else if (m_shape == ShapeKind::Cylinder) {
            shape.kind = sim::PROBE_PLACE_SHAPE_CYLINDER;
            shape.size2 = static_cast<uint32_t>(m_halfLength);
            shape.size3 = static_cast<uint32_t>(m_axis);
        }

        return shape;
    }

    // 形を囲む箱の半幅(common/probe_place.hlsli の ProbeDecodePlace と同じ。ドラッグの間隔に使う)
    BrushExtents BrushPanel::Extents() const {
        const auto radius = static_cast<uint32_t>(m_radiusCells);
        if (m_shape == ShapeKind::Box)
            return {radius, static_cast<uint32_t>(m_halfY), static_cast<uint32_t>(m_halfZ)};

        BrushExtents extents = {radius, radius, radius};
        if (m_shape == ShapeKind::Cylinder)
            extents[static_cast<size_t>(m_axis)] = static_cast<uint32_t>(m_halfLength);

        return extents;
    }

    // --- コマンド ---

    std::vector<sim::ProbeCommand> BrushPanel::Paint(std::span<const render::CellCoordinate> pressed,
                                                     std::span<const render::CellCoordinate> dragged, bool released,
                                                     size_t waitingCommands, uint32_t& sequence) {
        std::vector<sim::ProbeCommand> commands;
        if (m_table == nullptr)
            return commands;

        const std::vector<sim::SpeciesAmount> contents = ComposeMaterial(*m_table, m_rows, TemperatureMilliKelvin());
        if (contents.empty())
            return commands;  // 材料が空(パネルに出す)

        std::vector<BrushCell> pressedCells;
        std::vector<BrushCell> draggedCells;
        std::ranges::transform(pressed, std::back_inserter(pressedCells), ToBrushCell);
        std::ranges::transform(dragged, std::back_inserter(draggedCells), ToBrushCell);

        const std::vector<BrushCell> dabs = m_stroke.Paint(pressedCells, draggedCells, released, Extents(), !m_replace,
                                                           BrushStroke::Budget(waitingCommands));
        for (const BrushCell& dab : dabs)
            commands.push_back(sim::MakePlaceCommand(0, sequence++, MakeShape(dab), contents));

        m_paintedCount += static_cast<uint32_t>(commands.size());

        return commands;
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

        if (m_table == nullptr) {
            ImGui::TextUnformatted("反応表がまだ無い");
            ImGui::End();
            return;
        }

        ImGui::Checkbox("筆を持つ(左ボタンで置く・ドラッグで塗る。外すとつつき)", &m_holding);
        BuildShape();
        BuildMaterial();

        ImGui::SeparatorText("置き方");
        if (ImGui::RadioButton("置き換え", m_replace))
            m_replace = true;

        ImGui::SameLine();
        if (ImGui::RadioButton("足す", !m_replace))
            m_replace = false;

        ImGui::TextDisabled("ドラッグの間隔: 置き換え = 半幅ごと・足す = 重ならない間隔(2 × 半幅 + 1)");
        ImGui::TextDisabled("1 フレームに %u 個まで・待っているコマンドが %zu 個を超える間は待つ",
                            BrushStroke::MAX_DABS_PER_FRAME, BrushStroke::MAX_WAITING_COMMANDS);
        ImGui::Text("投げた %u・置いた回数 %u", m_paintedCount, m_placedCount);
        if (!m_lastPlaced.empty())
            ImGui::TextUnformatted(m_lastPlaced.c_str());

        ImGui::End();
    }

    void BrushPanel::BuildShape() {
        ImGui::SeparatorText("形");
        int shape = static_cast<int>(m_shape);
        ImGui::RadioButton("球", &shape, static_cast<int>(ShapeKind::Sphere));
        ImGui::SameLine();
        ImGui::RadioButton("直方体", &shape, static_cast<int>(ShapeKind::Box));
        ImGui::SameLine();
        ImGui::RadioButton("円柱", &shape, static_cast<int>(ShapeKind::Cylinder));
        m_shape = static_cast<ShapeKind>(shape);

        switch (m_shape) {
            case ShapeKind::Sphere: ImGui::SliderInt("半径(セル)", &m_radiusCells, 0, MAX_SIZE); break;
            case ShapeKind::Box:
                ImGui::SliderInt("x の半幅(セル)", &m_radiusCells, 0, MAX_SIZE);
                ImGui::SliderInt("y の半幅(セル)", &m_halfY, 0, MAX_SIZE);
                ImGui::SliderInt("z の半幅(セル)", &m_halfZ, 0, MAX_SIZE);
                break;
            case ShapeKind::Cylinder:
                ImGui::SliderInt("半径(セル)", &m_radiusCells, 0, MAX_SIZE);
                ImGui::SliderInt("軸の向きの半幅(セル)", &m_halfLength, 0, MAX_SIZE);
                ImGui::RadioButton("軸 x", &m_axis, 0);
                ImGui::SameLine();
                ImGui::RadioButton("軸 y(縦)", &m_axis, 1);
                ImGui::SameLine();
                ImGui::RadioButton("軸 z", &m_axis, 2);
                break;
        }

        m_radiusCells = std::clamp(m_radiusCells, 0, MAX_SIZE);
    }

    void BrushPanel::BuildMaterial() {
        ImGui::SeparatorText("材料(表の全物質から 3 つまで・1 セルあたり。kPa は 1 セル 0.125 m³ の理想気体の分圧)");
        const char* preview = m_preset < m_materials.size() ? m_materials[m_preset].name.data() : "(読み込む)";
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo("ひな形", preview)) {
            for (size_t i = 0; i < m_materials.size(); ++i) {
                if (ImGui::Selectable(m_materials[i].name.data(), i == m_preset)) {
                    m_preset = i;
                    LoadMaterialRows(m_materials[i], m_rows);
                }
            }

            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("温度 (K)", &m_temperatureKelvin, 10, 100);
        m_temperatureKelvin = std::clamp(m_temperatureKelvin, 1, MAX_TEMPERATURE_KELVIN);
        BuildMaterialRowsUi(*m_table, m_rows, TemperatureMilliKelvin());
        if (ComposeMaterial(*m_table, m_rows, TemperatureMilliKelvin()).empty())
            ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "材料が空(置けない)");
    }

    // --- 人がいない確認(--auto-place)---

    std::vector<sim::ProbeCommand> BrushPanel::TakeAutoCommands(uint64_t frame, size_t waitingCommands,
                                                                uint32_t& sequence) {
        std::vector<sim::ProbeCommand> commands;
        if (!m_autoRunning)
            return commands;

        RunAutoStrokes(frame, sequence, commands);
        if (frame == AUTO_IGNITE_FRAME)
            commands.push_back(sim::MakePokeCommand(0, sequence++, AUTO_CENTER_X, AUTO_CENTER_Y, sim::PROBE_VIEW_Z));

        RunAutoPaint(frame, waitingCommands, sequence, commands);

        return commands;
    }

    void BrushPanel::RunAutoStrokes(uint64_t frame, uint32_t& sequence, std::vector<sim::ProbeCommand>& commands) {
        for (const AutoStroke& stroke : AUTO_STROKES) {
            if (stroke.frame != frame)
                continue;

            const sim::LabMaterial* material = FindMaterial(stroke.material);
            if (material == nullptr) {
                Log(Channel::Sim, Level::Error, "--auto-place: 材料「{}」が反応表に無い", stroke.material);
                continue;
            }

            commands.push_back(sim::MakePlaceCommand(0, sequence++, stroke.shape, material->contents));
            m_autoSent += 1;
            m_autoExpected += stroke.expectApplied ? 1 : 0;
        }
    }

    // ドラッグで塗る(T-0245): パネルの値を決めて、窓の入力と同じ Paint の道で投げる
    void BrushPanel::RunAutoPaint(uint64_t frame, size_t waitingCommands, uint32_t& sequence,
                                  std::vector<sim::ProbeCommand>& commands) {
        std::vector<render::CellCoordinate> pressed;
        std::vector<render::CellCoordinate> dragged;
        bool released = false;
        if (frame == AUTO_PAINT_BOX_FRAME) {
            const sim::LabMaterial* wood = FindMaterial("木");
            if (wood == nullptr)
                return;

            LoadMaterialRows(*wood, m_rows);
            m_shape = ShapeKind::Box;
            m_radiusCells = 1;
            m_halfY = 2;
            m_halfZ = 0;
            m_replace = true;
            m_temperatureKelvin = 300;
            pressed = {AutoCell(8, 44)};
            dragged = {AutoCell(12, 44), AutoCell(20, 46)};
        } else if (frame == AUTO_PAINT_RELEASE_FRAME) {
            released = true;
        } else if (frame == AUTO_PAINT_CYLINDER_FRAME) {
            const sim::LabMaterial* charcoal = FindMaterial("木炭");
            const sim::LabMaterial* mana = FindMaterial("魔素(試験)");
            if (charcoal == nullptr || mana == nullptr)
                return;

            m_rows = {};
            m_rows[0] = {.species = static_cast<int>(charcoal->contents.front().species),
                         .amount = 6.0,
                         .unit = AmountUnit::Gram};
            m_rows[1] = {
                .species = static_cast<int>(mana->contents.front().species), .amount = 0.5, .unit = AmountUnit::Mole};
            m_shape = ShapeKind::Cylinder;
            m_radiusCells = 1;
            m_halfLength = 0;
            m_axis = 2;
            m_replace = false;
            m_temperatureKelvin = 900;
            pressed = {AutoCell(40, 48)};
            dragged = {AutoCell(52, 48)};
            released = true;
        } else {
            return;
        }

        const std::vector<sim::ProbeCommand> painted = Paint(pressed, dragged, released, waitingCommands, sequence);
        Log(Channel::Sim, Level::Info, "--auto-place: フレーム {} にドラッグで {} 個", frame, painted.size());
        m_autoPaintSteps += 1;
        m_autoPainted += static_cast<uint32_t>(painted.size());
        m_autoExpected += static_cast<uint32_t>(painted.size());
        commands.insert(commands.end(), painted.begin(), painted.end());
    }

    bool BrushPanel::AutoPassed() const {
        return m_autoSent == AUTO_STROKES.size() && m_autoPaintSteps == AUTO_PAINT_STEPS &&
               m_autoPainted == AUTO_PAINTED && m_placedCount == m_autoExpected;
    }

    std::string BrushPanel::AutoSummary() const {
        return std::format("投げた {} / {}・ドラッグ {} 手順で {} 個(はず {})・当たった {}(当たるはず {})", m_autoSent,
                           AUTO_STROKES.size(), m_autoPaintSteps, m_autoPainted, AUTO_PAINTED, m_placedCount,
                           m_autoExpected);
    }

}  // namespace bicameral::editor
