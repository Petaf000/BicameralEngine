// lab_panel.cpp — エディタの「実験室」のパネル(lab_panel.h)。材料の行は editor/material_rows(筆と共通。T-0245)。
#include "editor/lab_panel.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>

#include <imgui.h>

#include "core/log.h"
#include "script/reaction_package.h"

namespace bicameral::editor {

    namespace {

        constexpr int MAX_KELVIN = static_cast<int>(lab::LAB_MAX_TEMPERATURE_MILLIKELVIN / 1000);
        constexpr float SLICE_CELL_PIXELS = 22.0f;  // 根 1 つの箱の断面の 1 セル(大きい箱は細く。SliceCellPixels)
        constexpr float SLICE_MAX_PIXELS = 190.0f;  // 断面の幅の目安(根 1 つの箱の幅)
        constexpr float HOT_KELVIN = 2000.0f;       // 断面の色がいちばん明るくなる温度

        // --auto-lab の実験(tests/gpu_lab_box_test と同じ形を短く)
        constexpr uint32_t AUTO_IGNITE_TICK = 3;
        constexpr uint32_t AUTO_TICKS = 60;
        constexpr uint32_t AUTO_IGNITE_MILLIKELVIN = 1500000;
        constexpr std::string_view AUTO_SWAP_RULE = "cellulose_combustion";  // 途中で替える表で速くする規則
        constexpr double AUTO_SWAP_ACTIVATION_RATIO = 0.9;                   // その活性化エネルギーの倍率
        constexpr uint64_t AUTO_OXYGEN_PASCAL = 60000;  // 箱全体を酸素の多い 200 kPa の空気に(T-0220)
        constexpr uint64_t AUTO_NITROGEN_PASCAL = 140000;
        constexpr uint32_t AUTO_ROOM_MILLIKELVIN = 300000;
        constexpr size_t AUTO_LARGE_SHAPE = 4;  // BOX_SHAPES の 2×2×2(根を並べた箱の確認。T-0173)

        // 箱の大きさの選択肢(根を並べた数。T-0173。1 辺 4 m の根を x・y・z に)
        constexpr std::array<lab::LabBoxShape, 7> BOX_SHAPES = {
            lab::LabBoxShape{.rootsX = 1, .rootsY = 1, .rootsZ = 1},
            lab::LabBoxShape{.rootsX = 2, .rootsY = 1, .rootsZ = 1},
            lab::LabBoxShape{.rootsX = 4, .rootsY = 1, .rootsZ = 1},
            lab::LabBoxShape{.rootsX = 2, .rootsY = 2, .rootsZ = 1},
            lab::LabBoxShape{.rootsX = 2, .rootsY = 2, .rootsZ = 2},
            lab::LabBoxShape{.rootsX = 4, .rootsY = 4, .rootsZ = 1},
            lab::LabBoxShape{.rootsX = 4, .rootsY = 4, .rootsZ = 4}};

        std::string ShapeLabel(lab::LabBoxShape shape) {
            constexpr uint32_t ROOT_METERS = 4;
            return std::format("{}×{}×{} の根({}×{}×{} m・{} セル)", shape.rootsX, shape.rootsY, shape.rootsZ,
                               shape.rootsX * ROOT_METERS, shape.rootsY * ROOT_METERS, shape.rootsZ * ROOT_METERS,
                               sim::LabBoxCellCount(shape));
        }

        float SliceCellPixels(lab::LabBoxShape shape) {
            const uint32_t edge = sim::LAB_BOX_EDGE * std::max(shape.rootsX, shape.rootsY);
            return std::max(6.0f, std::min(SLICE_CELL_PIXELS, SLICE_MAX_PIXELS / static_cast<float>(edge)));
        }

        int32_t TemperatureOf(const sim::BakedReactionTable& table, const reaction::RxCell& cell) {
            return reaction::RxComputeThermal(table.View(), cell).temperature;
        }

        // 温度を色に(黒 → 赤 → 黄。表示だけなので浮動小数点を使う)
        ImVec4 TemperatureColor(int32_t milliKelvin) {
            const float t = std::clamp((static_cast<float>(milliKelvin) / 1000.0f - 300.0f) / (HOT_KELVIN - 300.0f),
                                       0.0f, 1.0f);
            return {0.15f + (0.85f * std::min(1.0f, t * 2.0f)), std::max(0.0f, (t * 2.0f) - 1.0f) * 0.9f, 0.1f, 1.0f};
        }

        // 記録の表のうち、この箱が持っていないものを中身から作り直して足す(別の起動の記録。T-0217・ADR-0050 と同じ確かめ方)
        std::expected<void, std::string> AddRecordedTables(sim::LabSession& session,
                                                           const sim::LabRecording& recording) {
            for (const sim::LabTableContent& content : recording.tables) {
                if (session.HasTable(content.version))
                    continue;

                const auto rebuilt = script::RebuildReactionTable(content.bytes, content.version);
                if (!rebuilt)
                    return std::unexpected(
                        std::format("記録の表(版 {:016x})を作り直せない: {}", content.version, rebuilt.error()));

                if (auto added = session.AddTable(rebuilt->table, content.version, content.bytes); !added)
                    return added;
            }

            return {};
        }

        script::ScriptValue* FieldOf(script::ScriptValue& table, std::string_view key) {
            const auto found = std::ranges::find_if(table.fields, [&](const script::ScriptField& field) {
                return field.key.IsString() && field.key.text == key;
            });

            return found == table.fields.end() ? nullptr : &found->value;
        }

        // --auto-lab で途中に替える表: 世界の表の中身の木の燃焼の活性化エネルギーを下げた(速く燃える)本物の表(版は中身のハッシュ)
        std::expected<script::LoadedReactionTable, std::string> MakeAutoSwappedTable(
            const script::LoadedReactionTable& world) {
            auto parsed = script::ParseTableBytes(world.tableBytes);
            if (!parsed)
                return std::unexpected(parsed.error());

            const auto category = parsed->tables.find(std::string(script::REACTION_RULES_CATEGORY));
            if (category == parsed->tables.end())
                return std::unexpected("表に反応の規則が無い");

            const auto rule = category->second.find(std::string(AUTO_SWAP_RULE));
            if (rule == category->second.end())
                return std::unexpected(std::format("表に規則 {} が無い", AUTO_SWAP_RULE));

            script::ScriptValue* rate = FieldOf(rule->second.value, "rate");
            script::ScriptValue* energy = rate != nullptr ? FieldOf(*rate, "activation_energy_j_per_mol") : nullptr;
            if (energy == nullptr || !energy->IsNumber())
                return std::unexpected(std::format("規則 {} の活性化エネルギーが読めない", AUTO_SWAP_RULE));

            energy->number = std::floor(energy->number * AUTO_SWAP_ACTIVATION_RATIO);
            return script::RebuildReactionTable(script::TableBytes(*parsed), script::TableVersion(*parsed));
        }

        sim::LabCellPosition PositionOf(const std::array<int, 3>& cell) {
            return {.x = static_cast<uint32_t>(cell[0]),
                    .y = static_cast<uint32_t>(cell[1]),
                    .z = static_cast<uint32_t>(cell[2])};
        }

    }  // namespace

    LabPanel::LabPanel(ID3D12Device* device) {
        if (device != nullptr)
            static_cast<void>(device->QueryInterface(IID_PPV_ARGS(&m_device)));
    }

    void LabPanel::UseTable(std::shared_ptr<const script::LoadedReactionTable> loaded) {
        if (loaded == nullptr || (m_loaded != nullptr && loaded->tableVersion == m_tableVersion))
            return;

        // --- 材料の行の物質 ID を名前で新しい表へ(物質を足す・消す表では ID がずれる。消えた物質の行は空に。T-0242)---
        if (m_table != nullptr)
            RemapMaterialRows(*m_table, loaded->table, m_rows);

        m_loaded = std::move(loaded);
        m_table = std::shared_ptr<const sim::BakedReactionTable>(m_loaded, &m_loaded->table);
        m_tableVersion = m_loaded->tableVersion;
        if (!m_session)
            return;

        // --- 箱があれば、次の刻みから新しい表で続ける(T-0218。初めから流し直すのはボタン「最新の表で初めから」)---
        Report(m_session->ChangeTable(*m_table, m_tableVersion, m_loaded->tableBytes));
        m_materials = sim::MakeLabMaterials(*m_table);
        m_tableChanges += 1;
        m_message = std::format("反応表が版 {:016x} に替わった。次の刻み {} から新しい表で続ける", m_tableVersion,
                                m_session->NextTick());
        Log(Channel::Sim, Level::Info, "実験室: {}", m_message);
    }

    bool LabPanel::CreateSession() {
        if (m_table == nullptr) {
            m_error = "反応表がまだ無い";
            return false;
        }

        const sim::BakedReactionTable& table = *m_table;
        if (!m_device) {
            m_error = "ID3D12Device5 が無い";
            return false;
        }

        const lab::LabBoxShape shape = BOX_SHAPES.at(m_shapeChoice);
        auto session = sim::LabSession::Create(m_device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, table, m_tableVersion,
                                               m_loaded->tableBytes, shape);
        if (!session) {
            m_error = session.error();
            return false;
        }

        m_session.emplace(std::move(*session));
        m_materials = sim::MakeLabMaterials(m_session->Table());
        LoadPreset(m_preset);
        ClampCells();
        Log(Channel::Sim, Level::Info, "実験室の箱を作った({})", ShapeLabel(shape));

        return true;
    }

    void LabPanel::Report(const std::expected<void, std::string>& result) {
        if (!result) {
            m_error = result.error();
            m_running = false;
            Log(Channel::Sim, Level::Error, "実験室: {}", m_error);
        }

        if (m_session && m_session->Mismatch())
            m_running = false;
    }

    // --- 窓 ---

    void LabPanel::Build() {
        if (m_autoPending) {
            m_autoPending = false;
            RunAuto();
        }

        ImGui::SetNextWindowPos({12.0f, 420.0f}, ImGuiCond_FirstUseEver);
        ImGui::Begin("実験室", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        if (!m_session) {
            ImGui::TextUnformatted(
                "小さな箱(根 = 8³ セル・0.5 m 角・300 K の空気。根を並べて大きくできる)に物を置いて反応を試し、\n"
                "GPU と CPU リファレンスを刻みごとに比べる(反応表は世界と同じ)");
            BuildShapeChoice();
            if (ImGui::Button("箱を作る"))
                static_cast<void>(CreateSession());

            if (!m_error.empty())
                ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", m_error.c_str());

            ImGui::End();
            return;
        }

        if (m_running)
            Report(m_session->Step(1));

        BuildStatus();
        BuildControls();
        BuildSlice();
        BuildCell();
        BuildRecording();
        ImGui::End();
    }

    // 箱の大きさ(根を並べた数。T-0173)。作った箱の大きさは変えられない(作り直す)
    void LabPanel::BuildShapeChoice() {
        const std::string current = ShapeLabel(BOX_SHAPES.at(m_shapeChoice));
        if (!ImGui::BeginCombo("箱の大きさ", current.c_str()))
            return;

        for (size_t i = 0; i < BOX_SHAPES.size(); ++i) {
            if (ImGui::Selectable(ShapeLabel(BOX_SHAPES.at(i)).c_str(), i == m_shapeChoice))
                m_shapeChoice = i;
        }

        ImGui::EndCombo();
    }

    // 選んでいるセル・範囲を今の箱の中に収める
    void LabPanel::ClampCells() {
        if (!m_session)
            return;

        const lab::LabBoxShape shape = m_session->Shape();
        const std::array<int, 3> edges = {static_cast<int>(sim::LAB_BOX_EDGE * shape.rootsX),
                                          static_cast<int>(sim::LAB_BOX_EDGE * shape.rootsY),
                                          static_cast<int>(sim::LAB_BOX_EDGE * shape.rootsZ)};
        for (size_t axis = 0; axis < edges.size(); ++axis) {
            m_cell.at(axis) = std::clamp(m_cell.at(axis), 0, edges.at(axis) - 1);
            m_rangeLow.at(axis) = std::clamp(m_rangeLow.at(axis), 0, edges.at(axis) - 1);
            m_rangeHigh.at(axis) = std::clamp(m_rangeHigh.at(axis), 0, edges.at(axis) - 1);
        }
    }

    void LabPanel::BuildStatus() {
        ImGui::Text("箱 %s", ShapeLabel(m_session->Shape()).c_str());
        BuildShapeChoice();
        ImGui::SameLine();
        if (ImGui::Button("この大きさで作り直す")) {
            m_running = false;
            static_cast<void>(CreateSession());
        }

        ImGui::Text("次の刻み %llu ・ 置いて待っているコマンド %zu",
                    static_cast<unsigned long long>(m_session->NextTick()), m_session->PendingCommands());
        ImGui::Text("反応表 版 %016llx(世界と同じ。替わった回数 %u)",
                    static_cast<unsigned long long>(m_session->TableVersion()), m_tableChanges);
        if (m_session->TableChangePending())
            ImGui::TextColored({1.0f, 0.85f, 0.3f, 1.0f}, "次の刻みから版 %016llx の表に替わる",
                               static_cast<unsigned long long>(m_session->LatestTableVersion()));
        if (const auto& mismatch = m_session->Mismatch(); mismatch) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "GPU と CPU が食い違った: %s", mismatch->what.c_str());
            if (mismatch->cell != lab::LAB_NO_CELL) {
                ImGui::Text("  エネルギー CPU %lld mJ / GPU %lld mJ、物質の数 CPU %u / GPU %u",
                            static_cast<long long>(mismatch->cpu.energy), static_cast<long long>(mismatch->gpu.energy),
                            mismatch->cpu.speciesCount, mismatch->gpu.speciesCount);
            }
        } else if (m_session->NextTick() > 0) {
            ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "刻み 0〜%llu で GPU と CPU がビット一致",
                               static_cast<unsigned long long>(m_session->NextTick() - 1));
        }

        if (const auto divergence = m_session->ReplayDivergence(); divergence)
            ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.0f}, "再生が記録と刻み %llu で違う",
                               static_cast<unsigned long long>(*divergence));

        if (!m_error.empty())
            ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", m_error.c_str());
    }

    // --- 材料を組む(T-0220。表の全物質から 3 つまで・量の単位・温度・置く範囲)---

    void LabPanel::LoadPreset(size_t index) {
        m_rows = {};
        if (index < m_materials.size())
            LoadMaterialRows(m_materials[index], m_rows);
    }

    std::vector<sim::SpeciesAmount> LabPanel::ComposedContents(uint32_t temperatureMilliKelvin) const {
        return ComposeMaterial(m_session->Table(), m_rows, temperatureMilliKelvin);
    }

    sim::LabCellRange LabPanel::PlaceRange() const {
        switch (m_rangeKind) {
            case RangeKind::Cell: return {.low = PositionOf(m_cell), .high = PositionOf(m_cell)};
            case RangeKind::Box: return {.low = PositionOf(m_rangeLow), .high = PositionOf(m_rangeHigh)};
            case RangeKind::Whole: return sim::LabWholeBox(m_session->Shape());
        }

        return sim::LabWholeBox(m_session->Shape());
    }

    std::optional<sim::Command> LabPanel::ComposedCommand(bool temperatureOnly) const {
        const auto milliKelvin = static_cast<uint32_t>(m_temperatureKelvin) * 1000u;
        if (temperatureOnly)
            return sim::MakeLabTemperatureRegionCommand(0, 0, PlaceRange(), milliKelvin);

        const std::vector<sim::SpeciesAmount> contents = ComposedContents(milliKelvin);
        if (contents.empty())
            return std::nullopt;

        return sim::MakeLabFillRegionCommand(0, 0, PlaceRange(), contents, milliKelvin);
    }

    void LabPanel::BuildMaterialRows(uint32_t milliKelvin) {
        BuildMaterialRowsUi(m_session->Table(), m_rows, milliKelvin);
    }

    void LabPanel::BuildControls() {
        ImGui::SeparatorText("材料(表の全物質から 3 つまで。kPa は 1 セル 0.125 m³ の理想気体の分圧)");
        const char* preview = m_preset < m_materials.size() ? m_materials[m_preset].name.data() : "(読み込む)";
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo("ひな形", preview)) {
            for (size_t i = 0; i < m_materials.size(); ++i) {
                if (ImGui::Selectable(m_materials[i].name.data(), i == m_preset)) {
                    m_preset = i;
                    LoadPreset(i);
                }
            }

            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("温度 (K)", &m_temperatureKelvin, 10, 100);
        m_temperatureKelvin = std::clamp(m_temperatureKelvin, 1, MAX_KELVIN);
        const auto milliKelvin = static_cast<uint32_t>(m_temperatureKelvin) * 1000u;
        BuildMaterialRows(milliKelvin);

        // --- 置く範囲 ---
        ImGui::SeparatorText("置く(次の刻みのコマンド)");
        int rangeKind = static_cast<int>(m_rangeKind);
        ImGui::RadioButton("1 セル", &rangeKind, static_cast<int>(RangeKind::Cell));
        ImGui::SameLine();
        ImGui::RadioButton("直方体", &rangeKind, static_cast<int>(RangeKind::Box));
        ImGui::SameLine();
        ImGui::RadioButton("箱全体", &rangeKind, static_cast<int>(RangeKind::Whole));
        m_rangeKind = static_cast<RangeKind>(rangeKind);

        // スライダーの上限は箱のいちばん長い辺。短い辺の外は ClampCells で収める
        const lab::LabBoxShape shape = m_session->Shape();
        const auto longest = std::max({shape.rootsX, shape.rootsY, shape.rootsZ});
        const int maxCell = static_cast<int>(sim::LAB_BOX_EDGE * longest) - 1;
        ImGui::SliderInt3("セル (x, y, z)", m_cell.data(), 0, maxCell);
        if (m_rangeKind == RangeKind::Box) {
            ImGui::SliderInt3("小さい角", m_rangeLow.data(), 0, maxCell);
            ImGui::SliderInt3("大きい角", m_rangeHigh.data(), 0, maxCell);
        }

        ClampCells();

        const std::vector<sim::SpeciesAmount> contents = ComposedContents(milliKelvin);
        ImGui::BeginDisabled(contents.empty());
        if (ImGui::Button("置く"))
            static_cast<void>(m_session->PlaceRegion(PlaceRange(), contents, milliKelvin));

        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("温度だけ"))
            static_cast<void>(m_session->SetTemperatureRegion(PlaceRange(), milliKelvin));

        BuildStepControls();
    }

    void LabPanel::BuildStepControls() {
        // --- 刻む ---
        ImGui::SeparatorText("刻む");
        ImGui::BeginDisabled(m_session->Mismatch().has_value());
        for (const uint32_t ticks : {1u, 10u, 60u}) {
            if (ImGui::Button(std::format("{} 刻み", ticks).c_str()))
                Report(m_session->Step(ticks));

            ImGui::SameLine();
        }

        ImGui::Checkbox("流し続ける", &m_running);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("最初に戻す")) {
            Report(m_session->Reset());
            m_error.clear();
            m_message.clear();
        }

        // 同じ操作を初めの箱から最新の表で(法則だけ替えて比べる。T-0194 の案 A)
        ImGui::SameLine();
        if (ImGui::Button("最新の表で初めから")) {
            const uint64_t tick = m_session->NextTick();
            Report(m_session->RerunWithLatestTable());
            m_message = std::format("置いた操作を初めの箱から最新の表で刻み {} まで流し直した", tick);
        }
    }

    // 選んだ z の断面の温度(CPU)。押すとセルを選ぶ
    void LabPanel::BuildSlice() {
        ImGui::SeparatorText(std::format("断面 z = {}(色 = CPU の温度)", m_cell[2]).c_str());
        const sim::MultiresNest& cpu = m_session->Cpu();
        const lab::LabBoxShape shape = m_session->Shape();
        const int edgeX = static_cast<int>(sim::LAB_BOX_EDGE * shape.rootsX);
        const int edgeY = static_cast<int>(sim::LAB_BOX_EDGE * shape.rootsY);
        const float pixels = SliceCellPixels(shape);
        for (int y = edgeY - 1; y >= 0; --y) {
            for (int x = 0; x < edgeX; ++x) {
                const uint32_t index = lab::LabCellIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                                                         static_cast<uint32_t>(m_cell[2]));
                const int32_t temperature = TemperatureOf(m_session->Table(), sim::LabBoxCell(cpu, index, shape));
                const std::string id = std::format("##lab{}_{}", x, y);
                const bool selected = x == m_cell[0] && y == m_cell[1];
                if (selected)
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);

                if (ImGui::ColorButton(id.c_str(), TemperatureColor(temperature), ImGuiColorEditFlags_NoTooltip,
                                       {pixels, pixels})) {
                    m_cell[0] = x;
                    m_cell[1] = y;
                }

                if (selected)
                    ImGui::PopStyleVar();

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("(%d, %d, %d) %.1f K", x, y, m_cell[2],
                                      static_cast<double>(temperature) / 1000.0);

                // 根の境は少し広く空ける(並べた根が見分けられるように)
                const bool rootEdge = (x + 1) % static_cast<int>(sim::LAB_BOX_EDGE) == 0;
                if (x + 1 < edgeX)
                    ImGui::SameLine(0.0f, rootEdge ? 6.0f : 2.0f);
            }
        }
    }

    // 選んだセルの成分と温度を CPU と GPU で並べる
    void LabPanel::BuildCell() const {
        const uint32_t index = PositionOf(m_cell).Index();
        const reaction::RxCell cpu = sim::LabBoxCell(m_session->Cpu(), index, m_session->Shape());
        const reaction::RxCell gpu = sim::LabBoxCell(m_session->Gpu(), index, m_session->Shape());
        const sim::BakedReactionTable& table = m_session->Table();

        ImGui::SeparatorText(std::format("セル ({}, {}, {})", m_cell[0], m_cell[1], m_cell[2]).c_str());
        ImGui::Text("温度 CPU %.1f K / GPU %.1f K   エネルギー CPU %lld mJ / GPU %lld mJ",
                    static_cast<double>(TemperatureOf(table, cpu)) / 1000.0,
                    static_cast<double>(TemperatureOf(table, gpu)) / 1000.0, static_cast<long long>(cpu.energy),
                    static_cast<long long>(gpu.energy));
        if (!ImGui::BeginTable("lab_cell", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
            return;

        ImGui::TableSetupColumn("物質");
        ImGui::TableSetupColumn("CPU (µmol)");
        ImGui::TableSetupColumn("GPU (µmol)");
        ImGui::TableHeadersRow();
        for (uint32_t species = 1; species < table.speciesNames.size(); ++species) {
            const auto amountOf = [&](const reaction::RxCell& cell) {
                for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                    if (cell.species[i] == species)
                        return cell.amounts[i];
                }

                return uint64_t{0};
            };

            const uint64_t a = amountOf(cpu);
            const uint64_t b = amountOf(gpu);
            if (a == 0 && b == 0)
                continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(table.speciesNames[species].c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(a));
            ImGui::TableNextColumn();
            if (a == b)
                ImGui::Text("%llu", static_cast<unsigned long long>(b));
            else
                ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "%llu", static_cast<unsigned long long>(b));
        }

        ImGui::EndTable();
    }

    // 記録(コマンドの列 + ハッシュの列)の保存と、読み込んでの再生
    void LabPanel::BuildRecording() {
        ImGui::SeparatorText("記録");
        std::array<char, 260> path{};
        std::copy_n(m_recordingPath.begin(), std::min(m_recordingPath.size(), path.size() - 1), path.begin());
        if (ImGui::InputText("ファイル", path.data(), path.size()))
            m_recordingPath = path.data();

        if (ImGui::Button("記録を保存")) {
            const std::vector<std::byte> bytes = sim::SerializeLabRecording(m_session->Recording());
            std::ofstream file(m_recordingPath, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            m_message = file ? std::format("{} 刻み・コマンド {} 個を保存した", m_session->NextTick(),
                                           m_session->Recording().commands.size())
                             : "保存できない";
        }

        ImGui::SameLine();
        if (ImGui::Button("読んで再生")) {
            std::ifstream file(m_recordingPath, std::ios::binary);
            const std::vector<char> chars{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            const auto recording = sim::ParseLabRecording(std::as_bytes(std::span(chars)));
            if (!recording) {
                m_message = recording.error();
            } else if (!sim::SameLabShape(recording->shape, m_session->Shape())) {
                // 箱の形が違う記録は、その形の箱を作って再生する(T-0173)
                const auto replayed = ReplayInNewSession(*recording, false);
                Report(replayed);
                m_message = replayed ? std::format("{} 刻みを {} の箱で再生した", recording->tickCount,
                                                   ShapeLabel(recording->shape))
                                     : replayed.error();
            } else if (auto added = AddRecordedTables(*m_session, *recording); !added) {
                m_message = added.error();
            } else {
                Report(m_session->Replay(*recording));
                m_message = std::format("{} 刻みを再生した", recording->tickCount);
            }
        }

        if (!m_message.empty())
            ImGui::TextUnformatted(m_message.c_str());
    }

    // --- 人がいない確認(--auto-lab)---

    void LabPanel::RunAuto() {
        m_autoFailed = true;
        if (!CreateSession()) {
            Log(Channel::Sim, Level::Error, "--auto-lab: 箱を作れない: {}", m_error);
            return;
        }

        const auto wood = std::ranges::find_if(m_materials, [](const sim::LabMaterial& m) { return m.name == "木"; });
        if (wood == m_materials.end())
            return;

        // --- 箱全体を酸素の多い 200 kPa の空気にしてから(範囲のコマンドと分圧の換算。T-0220)、真ん中に木を 8 セル ---
        const uint64_t nitrogen = sim::LabGasMicromoles(AUTO_NITROGEN_PASCAL, AUTO_ROOM_MILLIKELVIN);
        const std::array<sim::SpeciesAmount, 2> atmosphere = {
            sim::SpeciesAmount{.species = m_table->SpeciesId("oxygen"),
                               .amount = sim::LabGasMicromoles(AUTO_OXYGEN_PASCAL, AUTO_ROOM_MILLIKELVIN)},
            sim::SpeciesAmount{.species = m_table->SpeciesId("nitrogen"), .amount = nitrogen}};
        static_cast<void>(m_session->PlaceRegion(sim::LabWholeBox(), atmosphere, AUTO_ROOM_MILLIKELVIN));
        for (uint32_t i = 0; i < 8; ++i)
            static_cast<void>(m_session->Place({.x = 3 + (i & 1u), .y = 3 + ((i >> 1) & 1u), .z = 3 + (i >> 2)},
                                               wood->contents, AUTO_ROOM_MILLIKELVIN));

        // --- 火を付けた刻みに、刻みの途中の表の差し替えも通す(木が速く燃える本物の表。T-0218)---
        const auto swapped = MakeAutoSwappedTable(*m_loaded);
        if (!swapped) {
            Log(Channel::Sim, Level::Error, "--auto-lab: 途中で替える表を作れない({})", swapped.error());
            return;
        }

        auto result = m_session->Step(AUTO_IGNITE_TICK);
        static_cast<void>(m_session->SetTemperature({.x = 3, .y = 3, .z = 3}, AUTO_IGNITE_MILLIKELVIN));
        if (result)
            result = m_session->ChangeTable(swapped->table, swapped->tableVersion, swapped->tableBytes);

        if (result)
            result = m_session->Step(AUTO_TICKS - AUTO_IGNITE_TICK);

        if (result && m_session->Mismatch())
            result = std::unexpected(m_session->Mismatch()->what);

        // --- 箱の隅(燃えない窒素)は範囲で置いた量のまま(GPU の箱)---
        const reaction::RxCell corner = sim::LabBoxCell(m_session->Gpu(), 0);
        bool cornerFilled = false;
        for (uint32_t i = 0; i < corner.speciesCount; ++i)
            cornerFilled = cornerFilled ||
                           (corner.species[i] == atmosphere[1].species && corner.amounts[i] == nitrogen);

        if (result && !cornerFilled)
            result = std::unexpected("箱全体に置いた窒素が隅のセルに無い");

        if (result)
            result = ReplayInNewSession(m_session->Recording(), true);

        if (!result) {
            Log(Channel::Sim, Level::Error, "--auto-lab: 失敗({})", result.error());
            return;
        }

        Log(Channel::Sim, Level::Info,
            "--auto-lab: 箱全体を 200 kPa の空気(窒素 {} µmol/セル)にして {} 刻みで GPU と CPU が一致し(刻み {} "
            "で表を替えた)、"
            "記録から別の箱で再生しても同じ",
            nitrogen, AUTO_TICKS, AUTO_IGNITE_TICK);

        // --- 根を並べた箱(T-0173)---
        if (auto large = RunAutoLargeBox(wood->contents); !large) {
            Log(Channel::Sim, Level::Error, "--auto-lab: 根を並べた箱で失敗({})", large.error());
            return;
        }

        m_autoFailed = false;
        m_cell = {3, 3, 3};
    }

    // 根を並べた箱(2×2×2 の根・16³ セル): 箱全体の空気 → 8 つの根の境に木を 8 セル → 1 つに火を付けて流し、GPU と CPU が毎刻み一致・
    // 火の隣の別の根の木が温まる(根の境を熱が通る)・反対の隅の根まで範囲が届く・記録から同じ形の別の箱で再生しても同じ
    std::expected<void, std::string> LabPanel::RunAutoLargeBox(std::span<const sim::SpeciesAmount> wood) {
        m_shapeChoice = AUTO_LARGE_SHAPE;
        if (!CreateSession())
            return std::unexpected(std::format("箱を作れない: {}", m_error));

        const lab::LabBoxShape shape = m_session->Shape();
        const sim::LabCellRange whole = sim::LabWholeBox(shape);
        const uint64_t nitrogen = sim::LabGasMicromoles(AUTO_NITROGEN_PASCAL, AUTO_ROOM_MILLIKELVIN);
        const std::array<sim::SpeciesAmount, 2> atmosphere = {
            sim::SpeciesAmount{.species = m_table->SpeciesId("oxygen"),
                               .amount = sim::LabGasMicromoles(AUTO_OXYGEN_PASCAL, AUTO_ROOM_MILLIKELVIN)},
            sim::SpeciesAmount{.species = m_table->SpeciesId("nitrogen"), .amount = nitrogen}};
        static_cast<void>(m_session->PlaceRegion(whole, atmosphere, AUTO_ROOM_MILLIKELVIN));

        // --- 根の境(7 と 8 の間)を挟んで木を 2×2×2。(7, 7, 7) に火を付ける ---
        constexpr uint32_t BORDER = sim::LAB_BOX_EDGE - 1;
        for (uint32_t i = 0; i < 8; ++i)
            static_cast<void>(
                m_session->Place({.x = BORDER + (i & 1u), .y = BORDER + ((i >> 1) & 1u), .z = BORDER + (i >> 2)}, wood,
                                 AUTO_ROOM_MILLIKELVIN));

        auto result = m_session->Step(AUTO_IGNITE_TICK);
        static_cast<void>(m_session->SetTemperature({.x = BORDER, .y = BORDER, .z = BORDER}, AUTO_IGNITE_MILLIKELVIN));
        if (result)
            result = m_session->Step(AUTO_TICKS - AUTO_IGNITE_TICK);

        if (!result)
            return result;

        if (m_session->Mismatch())
            return std::unexpected(m_session->Mismatch()->what);

        // --- 隣の根(x = 8)の木が温まった・反対の隅の根の隅まで窒素が置いた量のまま(GPU の箱)---
        const sim::BakedReactionTable& table = m_session->Table();
        const uint32_t neighbor = lab::LabCellIndex(BORDER + 1, BORDER, BORDER);
        const int32_t neighborMilliKelvin = TemperatureOf(table, sim::LabBoxCell(m_session->Gpu(), neighbor, shape));
        constexpr int32_t WARMED_MILLIKELVIN = 1000;  // 1 K
        if (neighborMilliKelvin < static_cast<int32_t>(AUTO_ROOM_MILLIKELVIN) + WARMED_MILLIKELVIN)
            return std::unexpected(std::format("隣の根の木が温まっていない({} mK)", neighborMilliKelvin));

        const reaction::RxCell corner = sim::LabBoxCell(m_session->Gpu(), whole.high.Index(), shape);
        bool cornerFilled = false;
        for (uint32_t i = 0; i < corner.speciesCount; ++i)
            cornerFilled = cornerFilled ||
                           (corner.species[i] == atmosphere[1].species && corner.amounts[i] == nitrogen);

        if (!cornerFilled)
            return std::unexpected("箱全体に置いた窒素が反対の隅の根に無い");

        if (auto replayed = ReplayInNewSession(m_session->Recording(), false); !replayed)
            return replayed;

        Log(Channel::Sim, Level::Info,
            "--auto-lab: {} の箱で {} 刻み GPU と CPU が一致し、根の境の向こうの木が {} mK "
            "に温まり、記録から同じ形の別の箱で"
            "再生しても同じ",
            ShapeLabel(shape), AUTO_TICKS, neighborMilliKelvin);

        return {};
    }

    // 別の起動の形: 世界の表だけを持つ新しい箱(記録と同じ形。T-0173)で、ファイルの形を通した記録を再生する
    // (途中の表は記録の中身から作り直す。T-0217)。通ったら新しい箱をパネルの箱にする(流し終えると世界の表に戻す印が置かれている)。
    // expectMissingTables なら、中身を足す前の再生が断られること(途中で表を替えた記録の確かめ)も見る
    std::expected<void, std::string> LabPanel::ReplayInNewSession(const sim::LabRecording& recorded,
                                                                  bool expectMissingTables) {
        const auto parsed = sim::ParseLabRecording(sim::SerializeLabRecording(recorded));
        if (!parsed)
            return std::unexpected(parsed.error());

        auto fresh = sim::LabSession::Create(m_device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, *m_table, m_tableVersion,
                                             m_loaded->tableBytes, parsed->shape);
        if (!fresh)
            return std::unexpected(fresh.error());

        if (expectMissingTables && fresh->Replay(*parsed).has_value())
            return std::unexpected("中身を足す前に、持っていない表の記録を再生できてしまった");

        if (auto added = AddRecordedTables(*fresh, *parsed); !added)
            return added;

        if (auto replayed = fresh->Replay(*parsed); !replayed)
            return replayed;

        if (fresh->Mismatch())
            return std::unexpected(fresh->Mismatch()->what);

        if (fresh->ReplayDivergence() || fresh->Recording().hashes != recorded.hashes)
            return std::unexpected("別の箱での再生が記録と違う");

        // --- パネルの箱の大きさの選択も記録の形に合わせる(選択肢に無い形ならそのまま)---
        const auto choice = std::ranges::find_if(
            BOX_SHAPES, [&](lab::LabBoxShape shape) { return sim::SameLabShape(shape, parsed->shape); });
        if (choice != BOX_SHAPES.end())
            m_shapeChoice = static_cast<size_t>(choice - BOX_SHAPES.begin());

        m_session.emplace(std::move(*fresh));
        ClampCells();
        return {};
    }

}  // namespace bicameral::editor
