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
        constexpr size_t AUTO_LARGE_SHAPE = 4;    // BOX_SHAPES の 2×2×2(根を並べた箱の確認。T-0173)
        constexpr size_t AUTO_REFINED_SHAPE = 7;  // BOX_SHAPES の細かくできる 1×1×1(一部を細かくした箱の確認。T-0314)

        // 箱の大きさの選択肢(根を並べた数。T-0173。1 辺 4 m の根を x・y・z に)。後ろの 2 つは細かくできる箱(T-0314)
        constexpr std::array<lab::LabBoxShape, 9> BOX_SHAPES = {
            lab::LabBoxShape{.rootsX = 1, .rootsY = 1, .rootsZ = 1},
            lab::LabBoxShape{.rootsX = 2, .rootsY = 1, .rootsZ = 1},
            lab::LabBoxShape{.rootsX = 4, .rootsY = 1, .rootsZ = 1},
            lab::LabBoxShape{.rootsX = 2, .rootsY = 2, .rootsZ = 1},
            lab::LabBoxShape{.rootsX = 2, .rootsY = 2, .rootsZ = 2},
            lab::LabBoxShape{.rootsX = 4, .rootsY = 4, .rootsZ = 1},
            lab::LabBoxShape{.rootsX = 4, .rootsY = 4, .rootsZ = 4},
            lab::LabBoxShape{.rootsX = 1, .rootsY = 1, .rootsZ = 1, .refineBlocks = sim::LAB_DEFAULT_REFINE_BLOCKS},
            lab::LabBoxShape{.rootsX = 2, .rootsY = 2, .rootsZ = 2, .refineBlocks = sim::LAB_DEFAULT_REFINE_BLOCKS}};

        std::string ShapeLabel(lab::LabBoxShape shape) {
            constexpr uint32_t ROOT_METERS = 4;
            const std::string refine = shape.refineBlocks == 0
                                           ? std::string()
                                           : std::format("・細かくできる〔枠 {}〕", shape.refineBlocks);
            return std::format("{}×{}×{} の根({}×{}×{} m・{} セル{})", shape.rootsX, shape.rootsY, shape.rootsZ,
                               shape.rootsX * ROOT_METERS, shape.rootsY * ROOT_METERS, shape.rootsZ * ROOT_METERS,
                               sim::LabBoxCellCount(shape), refine);
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

        // 細かくした箱の計器と断面(T-0350。--auto-lab): GPU の箱の計器が CPU リファレンスだけで流した値と全刻み一致・
        // 細かくしただけの刻み 0 の箱の窒素が細かくする前と同じ(葉を µmol に直して数える)・火を付けた細かいセルの木の量が
        // 置いた量の 64 分の 1(µmol)・断面に細かいセル(火の所はレベル 2 で熱く、ほかはレベル 1)
        std::expected<void, std::string> CheckRefinedGauge(const sim::LabSession& session,
                                                           std::span<const sim::SpeciesAmount> wood,
                                                           sim::LabCellPosition lit) {
            const sim::BakedReactionTable& table = session.Table();
            const std::vector<sim::LabGaugeSample>& gauge = session.Gauge();
            const auto cpu = sim::RunLabGaugeOnCpu(session.Recording(), table, lit.Index());
            if (!cpu)
                return std::unexpected(cpu.error());

            if (gauge != *cpu)
                return std::unexpected("細かいセルの計器(GPU)が CPU だけで流した値と違う");

            const uint32_t nitrogen = table.SpeciesId("nitrogen");
            const uint32_t cellulose = table.SpeciesId("cellulose");
            const sim::LabGaugeSample unrefined = sim::SampleLabGauge(sim::MakeLabBoxNest(table, session.Shape()),
                                                                      table, 0, lit.Index(), session.Shape());
            if (gauge.size() < 2 || gauge[0].boxAmounts.at(nitrogen) != unrefined.boxAmounts.at(nitrogen))
                return std::unexpected("細かくした箱の窒素の合計が細かくする前と違う");

            constexpr uint32_t FINE_TO_MICROMOLES = 6;  // レベル 2 の量の単位は 8^-2 = 64 分の 1 µmol
            const uint64_t placed = wood.front().amount >> FINE_TO_MICROMOLES;
            if (wood.front().species != cellulose || gauge[1].cellAmounts.at(cellulose) != placed)
                return std::unexpected(std::format("見る細かいセルの木の量が {} µmol でない({})", placed,
                                                   gauge[1].cellAmounts.at(cellulose)));

            // --- 断面(GPU の箱): 火を付けた細かいセルの面 ---
            const lab::LabBoxShape shape = session.Shape();
            const uint32_t width = sim::LAB_BOX_EDGE * shape.rootsX * sim::LAB_FINE_SLICE_SCALE;
            const std::vector<sim::LabSliceCell> slice = sim::LabFineSlice(
                session.Gpu(), table, shape, lit.z / sim::LAB_FINE_SLICE_SCALE, lit.z % sim::LAB_FINE_SLICE_SCALE);
            const sim::LabSliceCell fire = slice.at((size_t{lit.y} * width) + lit.x);
            if (fire.level != 2 || fire.temperatureMilliKelvin <= static_cast<int32_t>(AUTO_ROOM_MILLIKELVIN))
                return std::unexpected(std::format("断面の火の所がレベル 2 で熱くない(レベル {}・{} mK)", fire.level,
                                                   fire.temperatureMilliKelvin));

            if (slice.front().level != 1)
                return std::unexpected(std::format("断面の隅がレベル 1 でない({})", slice.front().level));

            return {};
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
        else
            UseDefaults(loaded->table.templates);

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

        // 細かいセル(T-0350): 細かくできない箱では箱のセル。座標は箱のセルの中(0〜2^段 − 1)
        const int maxLevel = shape.refineBlocks == 0 ? 0 : static_cast<int>(lab::LAB_MAX_REFINE_LEVELS);
        m_fineLevel = std::clamp(m_fineLevel, 0, maxLevel);
        for (int& offset : m_fineCell)
            offset = std::clamp(offset, 0, (1 << m_fineLevel) - 1);
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

    // 初めの表のパッケージの既定(「置く」の初めの材料と温度。T-0399)
    void LabPanel::UseDefaults(const sim::MaterialTemplates& templates) {
        m_preset = templates.placeMaterial.value_or(0);
        if (templates.temperatureMilliKelvin > 0)
            m_temperatureKelvin = templates.temperatureMilliKelvin / 1000;
    }

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
        const bool fineCell = m_rangeKind == RangeKind::Cell && m_fineLevel > 0;  // 細かいセル 1 つ(T-0350)
        if (temperatureOnly) {
            return fineCell ? sim::MakeLabTemperatureCommand(0, 0, SelectedPosition(), milliKelvin)
                            : sim::MakeLabTemperatureRegionCommand(0, 0, PlaceRange(), milliKelvin);
        }

        const std::vector<sim::SpeciesAmount> contents = ComposedContents(milliKelvin);
        if (contents.empty())
            return std::nullopt;

        return fineCell ? sim::MakeLabFillCommand(0, 0, SelectedPosition(), contents, milliKelvin)
                        : sim::MakeLabFillRegionCommand(0, 0, PlaceRange(), contents, milliKelvin);
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
        if (shape.refineBlocks != 0)
            BuildFineCellControls();

        if (m_rangeKind == RangeKind::Box) {
            ImGui::SliderInt3("小さい角", m_rangeLow.data(), 0, maxCell);
            ImGui::SliderInt3("大きい角", m_rangeHigh.data(), 0, maxCell);
        }

        ClampCells();

        const std::vector<sim::SpeciesAmount> contents = ComposedContents(milliKelvin);
        ImGui::BeginDisabled(contents.empty());
        const bool fineCell = m_rangeKind == RangeKind::Cell && m_fineLevel > 0;
        if (ImGui::Button("置く")) {
            if (fineCell)
                static_cast<void>(m_session->Place(SelectedPosition(), contents, milliKelvin));
            else
                static_cast<void>(m_session->PlaceRegion(PlaceRange(), contents, milliKelvin));
        }

        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("温度だけ")) {
            if (fineCell)
                static_cast<void>(m_session->SetTemperature(SelectedPosition(), milliKelvin));
            else
                static_cast<void>(m_session->SetTemperatureRegion(PlaceRange(), milliKelvin));
        }

        // --- 範囲を細かくする(T-0314。細かくできる箱だけ。細かいセルの断面・計器は T-0350)---
        ImGui::BeginDisabled(m_session->Shape().refineBlocks == 0);
        for (uint32_t levels = 1; levels <= lab::LAB_MAX_REFINE_LEVELS; ++levels) {
            ImGui::SameLine();
            if (ImGui::Button(std::format("{} 段細かく", levels).c_str()))
                static_cast<void>(m_session->Refine(PlaceRange(), levels));
        }

        ImGui::EndDisabled();
        if (m_session->Shape().refineBlocks != 0)
            ImGui::TextDisabled("細かくしたブロック %u / 枠 %u", sim::LabRefinedBlockCount(m_session->Gpu()),
                                m_session->Shape().refineBlocks);

        BuildStepControls();
    }

    // 1 セルを細かいセルで選ぶ(細かくできる箱だけ。T-0350): 段の数と、箱のセルの中の座標。断面で細かいセルを押しても選べる
    void LabPanel::BuildFineCellControls() {
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderInt("細かさ (段)", &m_fineLevel, 0, static_cast<int>(lab::LAB_MAX_REFINE_LEVELS));
        if (m_fineLevel == 0)
            return;

        ImGui::SliderInt3("細かいセル(箱のセルの中)", m_fineCell.data(), 0, (1 << m_fineLevel) - 1);
        const sim::LabCellPosition selected = SelectedPosition();
        ImGui::TextDisabled("レベル %u の (%u, %u, %u)。1 セルの「置く」「温度だけ」はこのセル(量の単位 8^-%u µmol)",
                            selected.level, selected.x, selected.y, selected.z, selected.level);
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

    // 選んだ z の断面の温度(CPU)。押すとセルを選ぶ。細かくした所(T-0350)は箱のセルを 4×4 に分けて、そこを含む葉の温度を描き、
    // 押した所の葉(レベルと座標)を選ぶ
    void LabPanel::BuildSlice() {
        ImGui::SeparatorText(std::format("断面 z = {}(色 = CPU の温度)", m_cell[2]).c_str());
        const lab::LabBoxShape shape = m_session->Shape();
        constexpr int SCALE = static_cast<int>(sim::LAB_FINE_SLICE_SCALE);
        if (shape.refineBlocks != 0) {
            ImGui::SetNextItemWidth(150.0f);
            ImGui::SliderInt("細かい断面(箱のセルの中の z)", &m_fineZ, 0, SCALE - 1);
        }

        m_fineZ = shape.refineBlocks != 0 ? std::clamp(m_fineZ, 0, SCALE - 1) : 0;
        const std::vector<sim::LabSliceCell> slice = sim::LabFineSlice(m_session->Cpu(), m_session->Table(), shape,
                                                                       static_cast<uint32_t>(m_cell[2]),
                                                                       static_cast<uint32_t>(m_fineZ));
        const int edgeX = static_cast<int>(sim::LAB_BOX_EDGE * shape.rootsX);
        const int edgeY = static_cast<int>(sim::LAB_BOX_EDGE * shape.rootsY);
        const float pixels = SliceCellPixels(shape);
        for (int y = edgeY - 1; y >= 0; --y) {
            for (int x = 0; x < edgeX; ++x) {
                BuildSliceCell(slice, x, y, pixels);

                // 根の境は少し広く空ける(並べた根が見分けられるように)
                const bool rootEdge = (x + 1) % static_cast<int>(sim::LAB_BOX_EDGE) == 0;
                if (x + 1 < edgeX)
                    ImGui::SameLine(0.0f, rootEdge ? 6.0f : 2.0f);
            }
        }
    }

    // 断面の箱のセル 1 つ。細かく分かれていれば、ボタンの上に 4×4 の葉の色を重ねる
    void LabPanel::BuildSliceCell(std::span<const sim::LabSliceCell> slice, int x, int y, float pixels) {
        constexpr int SCALE = static_cast<int>(sim::LAB_FINE_SLICE_SCALE);
        const size_t width = sim::LAB_BOX_EDGE * m_session->Shape().rootsX * sim::LAB_FINE_SLICE_SCALE;
        const auto at = [&](int subX, int subY) -> const sim::LabSliceCell& {
            return slice[(static_cast<size_t>((y * SCALE) + subY) * width) + static_cast<size_t>((x * SCALE) + subX)];
        };
        bool split = false;
        for (int sub = 0; sub < SCALE * SCALE; ++sub)
            split = split || at(sub % SCALE, sub / SCALE).level > 0;

        const std::string id = std::format("##lab{}_{}", x, y);
        const bool selected = x == m_cell[0] && y == m_cell[1];
        if (selected)
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);

        const bool pressed = ImGui::ColorButton(id.c_str(), TemperatureColor(at(0, 0).temperatureMilliKelvin),
                                                ImGuiColorEditFlags_NoTooltip, {pixels, pixels});
        if (selected)
            ImGui::PopStyleVar();

        const ImVec2 low = ImGui::GetItemRectMin();
        const float sub = pixels / static_cast<float>(SCALE);
        const int hoverX = std::clamp(static_cast<int>((ImGui::GetMousePos().x - low.x) / sub), 0, SCALE - 1);
        const int hoverY = SCALE - 1 -
                           std::clamp(static_cast<int>((ImGui::GetMousePos().y - low.y) / sub), 0, SCALE - 1);
        if (split) {
            // 上が y の大きい方(断面と同じ向き)
            ImDrawList* draw = ImGui::GetWindowDrawList();
            for (int subY = 0; subY < SCALE; ++subY) {
                for (int subX = 0; subX < SCALE; ++subX) {
                    const ImVec2 cornerLow = {low.x + (sub * static_cast<float>(subX)),
                                              low.y + (sub * static_cast<float>(SCALE - 1 - subY))};
                    const ImVec2 cornerHigh = {cornerLow.x + sub, cornerLow.y + sub};
                    draw->AddRectFilled(
                        cornerLow, cornerHigh,
                        ImGui::ColorConvertFloat4ToU32(TemperatureColor(at(subX, subY).temperatureMilliKelvin)));
                }
            }
        }

        if (pressed) {
            m_cell[0] = x;
            m_cell[1] = y;
            const sim::LabSliceCell& leaf = split ? at(hoverX, hoverY) : at(0, 0);
            const int shift = static_cast<int>(lab::LAB_MAX_REFINE_LEVELS - leaf.level);
            m_fineLevel = static_cast<int>(leaf.level);
            m_fineCell = {hoverX >> shift, hoverY >> shift, m_fineZ >> shift};
        }

        if (ImGui::IsItemHovered()) {
            const sim::LabSliceCell& leaf = split ? at(hoverX, hoverY) : at(0, 0);
            ImGui::SetTooltip("(%d, %d, %d) レベル %u  %.1f K", x, y, m_cell[2], leaf.level,
                              static_cast<double>(leaf.temperatureMilliKelvin) / 1000.0);
        }
    }

    // 選んだセル: 箱のセル m_cell の中の、レベル m_fineLevel の m_fineCell(T-0350。細かくしていなければ箱のセル)
    sim::LabCellPosition LabPanel::SelectedPosition() const {
        const auto level = static_cast<uint32_t>(m_fineLevel);
        const auto along = [&](size_t axis) {
            return (static_cast<uint32_t>(m_cell.at(axis)) << level) + static_cast<uint32_t>(m_fineCell.at(axis));
        };

        return {.x = along(0), .y = along(1), .z = along(2), .level = level};
    }

    // 選んだセルを含む葉の成分と温度を CPU と GPU で並べる(細かくした所はその葉。T-0350)
    void LabPanel::BuildCell() const {
        const sim::LabCellPosition selected = SelectedPosition();
        const auto cpuLeaf = sim::LabLeafAt(m_session->Cpu(), selected);
        const auto gpuLeaf = sim::LabLeafAt(m_session->Gpu(), selected);
        if (!cpuLeaf || !gpuLeaf) {
            ImGui::SeparatorText(
                std::format("セル ({}, {}, {}) レベル {}", selected.x, selected.y, selected.z, selected.level).c_str());
            ImGui::TextDisabled("細かく分かれている(断面で細かいセルを押すか、細かさを上げて選ぶ)");
            return;
        }

        const reaction::RxCell& cpu = cpuLeaf->cell;
        const reaction::RxCell& gpu = gpuLeaf->cell;
        const sim::BakedReactionTable& table = m_session->Table();
        const sim::LabCellPosition leaf = cpuLeaf->position;

        ImGui::SeparatorText(std::format("セル ({}, {}, {}) レベル {}(量の単位 8^-{} µmol)", leaf.x, leaf.y, leaf.z,
                                         leaf.level, leaf.level)
                                 .c_str());
        ImGui::Text("温度 CPU %.1f K / GPU %.1f K   エネルギー CPU %lld mJ / GPU %lld mJ",
                    static_cast<double>(TemperatureOf(table, cpu)) / 1000.0,
                    static_cast<double>(TemperatureOf(table, gpu)) / 1000.0, static_cast<long long>(cpu.energy),
                    static_cast<long long>(gpu.energy));
        if (!ImGui::BeginTable("lab_cell", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
            return;

        ImGui::TableSetupColumn("物質");
        ImGui::TableSetupColumn("CPU");
        ImGui::TableSetupColumn("GPU");
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

        // --- 材料が 1 つも無い表(ブランクの世界。T-0371)は空の箱だけを確かめる ---
        if (m_materials.empty()) {
            if (auto empty = RunAutoEmptyBox(); !empty) {
                Log(Channel::Sim, Level::Error, "--auto-lab: 空の箱で失敗({})", empty.error());
                return;
            }

            Log(Channel::Sim, Level::Info,
                "--auto-lab: 材料の無い表(ブランクの世界)で空の箱を {} 刻み流して GPU と CPU "
                "が一致し、隅のセルは空のまま・"
                "記録から別の箱で再生しても同じ",
                AUTO_TICKS);
            m_autoFailed = false;
            return;
        }

        const sim::LabMaterial* material = sim::FindLabMaterial(m_materials, "wood");
        if (material == nullptr)
            return;

        // 材料の一覧は箱を作り直すたびに作り直す(CreateSession)ので、成分を写しておく
        // (一覧の反復子を次の箱で使うと無効で、debug の窓は検査のダイアログで止まっていた。T-0350)
        const std::vector<sim::SpeciesAmount> wood = material->contents;

        // --- 箱全体を酸素の多い 200 kPa の空気にしてから(範囲のコマンドと分圧の換算。T-0220)、真ん中に木を 8 セル ---
        const uint64_t nitrogen = sim::LabGasMicromoles(AUTO_NITROGEN_PASCAL, AUTO_ROOM_MILLIKELVIN);
        const std::array<sim::SpeciesAmount, 2> atmosphere = {
            sim::SpeciesAmount{.species = m_table->SpeciesId("oxygen"),
                               .amount = sim::LabGasMicromoles(AUTO_OXYGEN_PASCAL, AUTO_ROOM_MILLIKELVIN)},
            sim::SpeciesAmount{.species = m_table->SpeciesId("nitrogen"), .amount = nitrogen}};
        static_cast<void>(m_session->PlaceRegion(sim::LabWholeBox(), atmosphere, AUTO_ROOM_MILLIKELVIN));
        for (uint32_t i = 0; i < 8; ++i)
            static_cast<void>(m_session->Place({.x = 3 + (i & 1u), .y = 3 + ((i >> 1) & 1u), .z = 3 + (i >> 2)}, wood,
                                               AUTO_ROOM_MILLIKELVIN));

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
        if (auto large = RunAutoLargeBox(wood); !large) {
            Log(Channel::Sim, Level::Error, "--auto-lab: 根を並べた箱で失敗({})", large.error());
            return;
        }

        // --- 一部を細かくした箱(T-0314)---
        if (auto refined = RunAutoRefinedBox(wood); !refined) {
            Log(Channel::Sim, Level::Error, "--auto-lab: 一部を細かくした箱で失敗({})", refined.error());
            return;
        }

        m_autoFailed = false;
        m_cell = {3, 3, 3};
    }

    // 空の箱(ブランクの世界。T-0371): 表に空気が無いので箱は空のセルで満たされる(MakeLabBoxNest)。AUTO_TICKS 刻み流し、
    // GPU と CPU が毎刻み一致・隅のセルと最後のセルが空のまま・記録から別の箱で再生しても同じ
    std::expected<void, std::string> LabPanel::RunAutoEmptyBox() {
        auto result = m_session->Step(AUTO_TICKS);
        if (result && m_session->Mismatch())
            result = std::unexpected(m_session->Mismatch()->what);

        if (!result)
            return result;

        const uint32_t lastCell = sim::LabBoxCellCount(sim::LAB_SINGLE_ROOT_SHAPE) - 1;
        for (const uint32_t index : {0u, lastCell}) {
            if (sim::LabBoxCell(m_session->Gpu(), index).speciesCount != 0)
                return std::unexpected(std::format("空の箱のセル {} に物がある", index));
        }

        return ReplayInNewSession(m_session->Recording(), false);
    }

    // 一部を細かくした箱(細かくできる根 1 つ): (2..5)³ を 2 段細かくし、細かいセル 2³ に木を置いて 1 つに火を付けて流し、
    // GPU と CPU が毎刻み一致・細かくしたブロックが 16・細かい所の木が燃えた・細かいセルの計器と断面(T-0350)・
    // 記録から同じ形の別の箱で再生しても同じ
    std::expected<void, std::string> LabPanel::RunAutoRefinedBox(std::span<const sim::SpeciesAmount> wood) {
        m_shapeChoice = AUTO_REFINED_SHAPE;
        if (!CreateSession())
            return std::unexpected(std::format("箱を作れない: {}", m_error));

        // --- 計器は火を付ける細かいセルを見る(T-0350)---
        constexpr uint32_t FINE = 8;  // レベル 2 の座標(箱のセル 2 = 0.25 m 角の細かいセルの 8)
        const sim::LabCellPosition lit = {.x = FINE, .y = FINE, .z = FINE, .level = 2};
        m_session->SetGaugeCell(lit.Index());
        if (m_session->GaugeCell() != lit.Index())
            return std::unexpected("計器の見るセルを細かいセルにできない");

        static_cast<void>(m_session->Refine({.low = {.x = 2, .y = 2, .z = 2}, .high = {.x = 5, .y = 5, .z = 5}}, 2));
        auto result = m_session->Step(1);
        for (uint32_t i = 0; i < 8; ++i) {
            if (!m_session->Place({.x = FINE + (i & 1u), .y = FINE + ((i >> 1) & 1u), .z = FINE + (i >> 2), .level = 2},
                                  wood, AUTO_ROOM_MILLIKELVIN))
                return std::unexpected("細かいセルに木を置けない");
        }

        if (result)
            result = m_session->Step(AUTO_IGNITE_TICK - 1);

        static_cast<void>(m_session->SetTemperature(lit, AUTO_IGNITE_MILLIKELVIN));
        if (result)
            result = m_session->Step(AUTO_TICKS - AUTO_IGNITE_TICK);

        if (!result)
            return result;

        if (m_session->Mismatch())
            return std::unexpected(m_session->Mismatch()->what);

        const uint32_t refined = sim::LabRefinedBlockCount(m_session->Gpu());
        if (refined != 16)
            return std::unexpected(std::format("細かくしたブロックが 16 でない({})", refined));

        // --- 火を付けた細かいセルのセルロースが減った(GPU の箱)---
        const uint32_t cellulose = m_session->Table().SpeciesId("cellulose");
        const auto litCell = sim::LabBoxFineCell(m_session->Gpu(), lit);
        uint64_t left = 0;
        for (uint32_t i = 0; litCell && i < litCell->speciesCount; ++i)
            left += litCell->species[i] == cellulose ? litCell->amounts[i] : 0;

        if (!litCell || left >= wood.front().amount)
            return std::unexpected("細かいセルの木が燃えていない");

        if (auto checked = CheckRefinedGauge(*m_session, wood, lit); !checked)
            return checked;

        if (auto replayed = ReplayInNewSession(m_session->Recording(), false); !replayed)
            return replayed;

        Log(Channel::Sim, Level::Info,
            "--auto-lab: 根の一部を 2 段細かくした箱で {} 刻み GPU と CPU が一致し、細かいセルの木が燃え"
            "(セルロース {} → {})、細かいセルの計器が CPU だけで流した値と全刻み一致・断面に細かいセル、"
            "記録から同じ形の別の箱で再生しても同じ",
            AUTO_TICKS, wood.front().amount, left);

        return {};
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
        for (uint32_t i = 0; i < 8; ++i) {
            if (!m_session->Place({.x = BORDER + (i & 1u), .y = BORDER + ((i >> 1) & 1u), .z = BORDER + (i >> 2)}, wood,
                                  AUTO_ROOM_MILLIKELVIN))
                return std::unexpected("木を置けない");
        }

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
        // 温まったと見る幅は 1 mK(熱が根の境を越えた = 室温より上)。実験室の箱は熱の放射の段を持たず、熱伝導率は
        // D-637・T-1066(2026-10-24。試験の倍率 固体 20000 → 1)から現実の値なので、57 刻みで隣のセルの木は数 mK しか上がらない
        // (前の幅 1 K は倍率 20000 の時のもの。T-1251)
        constexpr int32_t WARMED_MILLIKELVIN = 1;
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
