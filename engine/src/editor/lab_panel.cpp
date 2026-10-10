// lab_panel.cpp — エディタの「実験室」のパネル(lab_panel.h)。
#include "editor/lab_panel.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

#include <imgui.h>

#include "core/log.h"

namespace bicameral::editor {

    namespace {

        constexpr int MAX_KELVIN = static_cast<int>(lab::LAB_MAX_TEMPERATURE_MILLIKELVIN / 1000);
        constexpr float SLICE_CELL_PIXELS = 22.0f;
        constexpr float HOT_KELVIN = 2000.0f;  // 断面の色がいちばん明るくなる温度

        // --auto-lab の実験(tests/gpu_lab_box_test と同じ形を短く)
        constexpr uint32_t AUTO_IGNITE_TICK = 3;
        constexpr uint32_t AUTO_TICKS = 60;
        constexpr uint32_t AUTO_IGNITE_MILLIKELVIN = 1500000;
        constexpr uint64_t
            AUTO_SWAP_VERSION_BITS = 0x5A5A'0000'0000'0001ULL;  // 同じ中身の表に付ける別の版(世界の版と違えばよい)

        int32_t TemperatureOf(const sim::BakedReactionTable& table, const reaction::RxCell& cell) {
            return reaction::RxComputeThermal(table.View(), cell).temperature;
        }

        // 温度を色に(黒 → 赤 → 黄。表示だけなので浮動小数点を使う)
        ImVec4 TemperatureColor(int32_t milliKelvin) {
            const float t = std::clamp((static_cast<float>(milliKelvin) / 1000.0f - 300.0f) / (HOT_KELVIN - 300.0f),
                                       0.0f, 1.0f);
            return {0.15f + (0.85f * std::min(1.0f, t * 2.0f)), std::max(0.0f, (t * 2.0f) - 1.0f) * 0.9f, 0.1f, 1.0f};
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

    void LabPanel::UseTable(std::shared_ptr<const sim::BakedReactionTable> table, uint64_t version) {
        if (table == nullptr || (m_table != nullptr && version == m_tableVersion))
            return;

        m_table = std::move(table);
        m_tableVersion = version;
        if (!m_session)
            return;

        // --- 箱があれば、次の刻みから新しい表で続ける(T-0218。初めから流し直すのはボタン「最新の表で初めから」)---
        Report(m_session->ChangeTable(*m_table, m_tableVersion));
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

        auto session = sim::LabSession::Create(m_device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, table, m_tableVersion);
        if (!session) {
            m_error = session.error();
            return false;
        }

        m_session.emplace(std::move(*session));
        m_materials = sim::MakeLabMaterials(m_session->Table());
        m_material = std::min<int>(m_material, static_cast<int>(m_materials.size()) - 1);
        Log(Channel::Sim, Level::Info, "実験室の箱を作った");

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
                "小さな箱(8³ セル・0.5 m 角・300 K の空気)に物を置いて反応を試し、\n"
                "GPU と CPU リファレンスを刻みごとに比べる(反応表は世界と同じ)");
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

    void LabPanel::BuildStatus() {
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

    void LabPanel::BuildControls() {
        ImGui::SeparatorText("置く(次の刻みのコマンド)");
        ImGui::SliderInt3("セル (x, y, z)", m_cell.data(), 0, static_cast<int>(sim::LAB_BOX_EDGE) - 1);

        const char* preview = m_materials.empty() ? "" : m_materials[static_cast<size_t>(m_material)].name.data();
        if (ImGui::BeginCombo("材料", preview)) {
            for (size_t i = 0; i < m_materials.size(); ++i) {
                if (ImGui::Selectable(m_materials[i].name.data(), static_cast<int>(i) == m_material))
                    m_material = static_cast<int>(i);
            }

            ImGui::EndCombo();
        }

        ImGui::SliderInt("温度 (K)", &m_temperatureKelvin, 0, MAX_KELVIN);
        const auto milliKelvin = static_cast<uint32_t>(m_temperatureKelvin) * 1000u;
        if (ImGui::Button("置く") && !m_materials.empty())
            static_cast<void>(m_session->Place(PositionOf(m_cell),
                                               m_materials[static_cast<size_t>(m_material)].contents, milliKelvin));

        ImGui::SameLine();
        if (ImGui::Button("温度だけ"))
            static_cast<void>(m_session->SetTemperature(PositionOf(m_cell), milliKelvin));

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
        for (int y = static_cast<int>(sim::LAB_BOX_EDGE) - 1; y >= 0; --y) {
            for (int x = 0; x < static_cast<int>(sim::LAB_BOX_EDGE); ++x) {
                const uint32_t index = multires::MrCellIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                                                             static_cast<uint32_t>(m_cell[2]));
                const int32_t temperature = TemperatureOf(m_session->Table(), sim::LabBoxCell(cpu, index));
                const std::string id = std::format("##lab{}_{}", x, y);
                const bool selected = x == m_cell[0] && y == m_cell[1];
                if (selected)
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);

                if (ImGui::ColorButton(id.c_str(), TemperatureColor(temperature), ImGuiColorEditFlags_NoTooltip,
                                       {SLICE_CELL_PIXELS, SLICE_CELL_PIXELS})) {
                    m_cell[0] = x;
                    m_cell[1] = y;
                }

                if (selected)
                    ImGui::PopStyleVar();

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("(%d, %d, %d) %.1f K", x, y, m_cell[2],
                                      static_cast<double>(temperature) / 1000.0);

                if (x + 1 < static_cast<int>(sim::LAB_BOX_EDGE))
                    ImGui::SameLine(0.0f, 2.0f);
            }
        }
    }

    // 選んだセルの成分と温度を CPU と GPU で並べる
    void LabPanel::BuildCell() const {
        const uint32_t index = PositionOf(m_cell).Index();
        const reaction::RxCell cpu = sim::LabBoxCell(m_session->Cpu(), index);
        const reaction::RxCell gpu = sim::LabBoxCell(m_session->Gpu(), index);
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

        for (uint32_t i = 0; i < 8; ++i)
            static_cast<void>(m_session->Place({.x = 3 + (i & 1u), .y = 3 + ((i >> 1) & 1u), .z = 3 + (i >> 2)},
                                               wood->contents, 300000));

        // --- 火を付けた刻みに、刻みの途中の表の差し替えも通す(同じ中身を別の版として。T-0218)---
        auto result = m_session->Step(AUTO_IGNITE_TICK);
        static_cast<void>(m_session->SetTemperature({.x = 3, .y = 3, .z = 3}, AUTO_IGNITE_MILLIKELVIN));
        if (result)
            result = m_session->ChangeTable(*m_table, m_tableVersion ^ AUTO_SWAP_VERSION_BITS);

        if (result)
            result = m_session->Step(AUTO_TICKS - AUTO_IGNITE_TICK);

        const sim::LabRecording recorded = m_session->Recording();
        const auto parsed = sim::ParseLabRecording(sim::SerializeLabRecording(recorded));
        if (result && parsed)
            result = m_session->Replay(*parsed);

        if (!result || !parsed || m_session->Mismatch() || m_session->ReplayDivergence() ||
            m_session->Recording().hashes != recorded.hashes) {
            Log(Channel::Sim, Level::Error, "--auto-lab: 失敗({})",
                !result ? result.error() : (m_session->Mismatch() ? m_session->Mismatch()->what : "再生が記録と違う"));
            return;
        }

        // --- 世界の表の版に戻す(次の刻みから。中身は同じ)---
        if (auto restored = m_session->ChangeTable(*m_table, m_tableVersion); !restored) {
            Log(Channel::Sim, Level::Error, "--auto-lab: 表の版を戻せない({})", restored.error());
            return;
        }

        m_autoFailed = false;
        m_cell = {3, 3, 3};
        Log(Channel::Sim, Level::Info,
            "--auto-lab: {} 刻みで GPU と CPU が一致し(刻み {} で表を替えた)、記録から再生しても同じ", AUTO_TICKS,
            AUTO_IGNITE_TICK);
    }

}  // namespace bicameral::editor
