// mixer_panel.cpp — 環境ミキサーのパネル(mixer_panel.h。T-0452・D-457)。
#include "editor/mixer_panel.h"

#include <algorithm>
#include <format>
#include <utility>

#include <imgui.h>

#include "core/log.h"
#include "editor/editor_overlay.h"

namespace bicameral::editor {

    namespace {

        constexpr ImVec4 ERROR_COLOR = {1.0f, 0.4f, 0.4f, 1.0f};
        constexpr ImVec4 GOOD_COLOR = {0.5f, 0.9f, 0.5f, 1.0f};
        constexpr ImVec4 NOTE_COLOR = {0.7f, 0.7f, 0.7f, 1.0f};

        // 自動の確認が差し替えを待つ上限(フレーム。止めている・刻みが遅い時の待ち)
        constexpr uint32_t AUTO_WAIT_FRAMES = 1200;

        std::string JoinNames(const std::vector<std::string>& names) {
            std::string text;
            for (const std::string& name : names)
                text += (text.empty() ? "" : "・") + name;

            return text;
        }

    }  // namespace

    MixerPanel::MixerPanel() {
        auto catalog = script::ReadStandardCatalog(script::DefaultStandardPackageRoot());
        if (!catalog) {
            m_catalogError = catalog.error();
            return;
        }

        m_catalog = std::move(*catalog);
    }

    // --- パネル ---

    void MixerPanel::Build(const ReactionTableStatus& status, bool replaying) {
        RunAuto(status);

        ImGui::SetNextWindowPos({420.0f, 40.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({460.0f, 300.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("環境ミキサー")) {
            ImGui::End();
            return;
        }

        if (!m_catalog) {
            ImGui::TextColored(ERROR_COLOR, "標準パッケージの棚を読めない: %s", m_catalogError.c_str());
            ImGui::End();
            return;
        }

        ImGui::TextColored(NOTE_COLOR, "標準パッケージを世界に足す(%s)。表の法則は次の刻みの境界で替わる",
                           m_catalog->root.generic_string().c_str());
        ImGui::TextColored(NOTE_COLOR, "重力・初めのセル(空気で満たす)は世界を作り直すまで効かない(T-0453)");
        if (replaying)
            ImGui::TextColored(NOTE_COLOR, "再生中は足せない(記録の表を当てる)");

        ImGui::Separator();
        for (const script::StandardPackage& package : m_catalog->packages)
            BuildPackage(package, replaying);

        ImGui::Separator();
        ImGui::Text("足したもの: %s", m_mix.empty() ? "(なし)" : JoinNames(m_mix).c_str());
        if (!m_message.empty())
            ImGui::TextColored(m_messageFailed ? ERROR_COLOR : GOOD_COLOR, "%s", m_message.c_str());

        if (status.lastFailed && !status.message.empty())
            ImGui::TextColored(ERROR_COLOR, "読み直せなかった(古い表のまま): %s", status.message.c_str());

        ImGui::End();
    }

    // 1 つのパッケージの行: 足す / 外すボタン・見せる名前・依存(一緒に足す・外すもの)
    void MixerPanel::BuildPackage(const script::StandardPackage& package, bool replaying) {
        ImGui::PushID(package.name.c_str());
        const bool mixed = std::ranges::contains(m_mix, package.name);
        const bool busy = replaying || m_request.has_value();

        ImGui::BeginDisabled(busy);
        if (ImGui::Button(mixed ? "外す" : "足す", {48.0f, 0.0f})) {
            if (mixed) {
                Request(script::MixRemove(*m_catalog, m_mix, package.name));
            } else if (auto added = script::MixAdd(*m_catalog, m_mix, package.name); added) {
                Request(std::move(*added));
            } else {
                m_message = added.error();
                m_messageFailed = true;
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextColored(mixed ? GOOD_COLOR : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", package.title.c_str());
        ImGui::SameLine();
        ImGui::TextColored(NOTE_COLOR, "(%s)", package.name.c_str());

        // 依存: 足すと一緒に足すもの・外すと一緒に外れるもの
        if (ImGui::IsItemHovered()) {
            const std::vector<std::string> dependents = script::MixDependents(*m_catalog, m_mix, package.name);
            ImGui::SetTooltip("依存: %s\n外すと一緒に外れる: %s",
                              package.depends.empty() ? "なし" : JoinNames(package.depends).c_str(),
                              dependents.empty() ? "なし" : JoinNames(dependents).c_str());
        }

        ImGui::PopID();
    }

    void MixerPanel::Request(std::vector<std::string> mix) {
        auto folders = script::MixFolders(*m_catalog, mix);
        if (!folders) {
            m_message = folders.error();
            m_messageFailed = true;
            return;
        }

        m_request = MixRequest{.mix = std::move(mix), .folders = std::move(*folders)};
    }

    void MixerPanel::Result(const MixRequest& request, bool applied, std::string message) {
        m_messageFailed = !applied;
        m_message = std::move(message);
        if (applied)
            m_mix = request.mix;

        Log(Channel::Tool, applied ? Level::Info : Level::Warning, "環境ミキサー: [{}] {}", JoinNames(request.mix),
            m_message);
    }

    // --- 自動の確認(--auto-mixer)---

    void MixerPanel::RunAuto(const ReactionTableStatus& status) {
        if (m_autoStage == AutoStage::Off || AutoFinished())
            return;

        if (!m_catalog)
            return FailAuto("棚を読めない: " + m_catalogError);

        if (m_messageFailed)
            return FailAuto("足せなかった: " + m_message);

        if (++m_autoWaitFrames > AUTO_WAIT_FRAMES)
            return FailAuto("差し替えが当たらない(待ちの上限)");

        switch (m_autoStage) {
            // 棚の全部を足す(依存の後の 1 回の依頼)
            case AutoStage::Start: {
                m_autoInitialVersion = status.version;
                std::vector<std::string> all;
                for (const script::StandardPackage& package : m_catalog->packages) {
                    auto added = script::MixAdd(*m_catalog, all, package.name);
                    if (!added)
                        return FailAuto(added.error());

                    all = std::move(*added);
                }

                Request(std::move(all));
                m_autoStage = AutoStage::Added;
                m_autoWaitFrames = 0;
                break;
            }

            // 当たった(版が変わり、物質がある)→ 元素を外す(依存するものも全部外れる)
            case AutoStage::Added:
                if (m_request || status.version == m_autoInitialVersion || !m_table ||
                    m_table->tableVersion != status.version)
                    return;

                if (m_table->table.species.size() <= 1 || m_table->table.rules.empty())
                    return FailAuto("足した表に物質か規則が無い");

                m_autoAddedVersion = status.version;
                Log(Channel::Tool, Level::Info,
                    "--auto-mixer: 足した表が当たった(版 {:016x}・物質 {}・規則 {}・重力 {} µm/s²)", status.version,
                    m_table->table.species.size() - 1, m_table->table.rules.size(),
                    m_table->table.environment.gravityMicrometersPerSecond2);
                Request(script::MixRemove(*m_catalog, m_mix, "real_elements"));
                m_autoStage = AutoStage::Removed;
                m_autoWaitFrames = 0;
                break;

            // 外した表が当たった(初めの版に戻った)
            case AutoStage::Removed:
                if (m_request || status.version != m_autoInitialVersion)
                    return;

                if (!m_mix.empty())
                    return FailAuto("外したのに足した一覧が残っている: " + JoinNames(m_mix));

                Log(Channel::Tool, Level::Info, "--auto-mixer: 外して初めの表(版 {:016x})に戻った。OK", status.version);
                m_autoStage = AutoStage::Done;
                break;

            default: break;
        }
    }

    void MixerPanel::FailAuto(std::string why) {
        m_autoFailure = std::move(why);
        m_autoStage = AutoStage::Failed;
        Log(Channel::Tool, Level::Error, "--auto-mixer: {}", m_autoFailure);
    }

}  // namespace bicameral::editor
