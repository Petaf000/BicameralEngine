// reaction_table_panel.cpp — エディタの「反応表」のパネル(reaction_table_panel.h。T-0219)。
// 表示の計算(始まる温度・kJ/mol への直し)は浮動小数点を使う(エディタの表示だけ。書き戻す値は整数に丸めて、それが正になる)。
#include "editor/reaction_table_panel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <numbers>
#include <utility>

#include <imgui.h>

#include "core/log.h"
#include "editor/editor_overlay.h"
#include "script/luau_literal_edit.h"

namespace bicameral::editor {

    namespace {

        // 「始まる温度」= 速度定数 k = A·exp(−Ea/RT) が START_RATE_PER_SECOND になる温度(表示の定義。仮。T-0219 の判断待ち)
        constexpr double START_RATE_PER_SECOND = 0.01;
        constexpr double GAS_CONSTANT = 8.314462618;  // J/(mol·K)

        // --auto-table-edit が書き換える値(試験の表の木の燃焼の速さ)と書く値
        std::vector<std::string> AutoKeyPath() {
            return {"reactions", "cellulose_combustion", "rate", "a"};
        }

        constexpr const char* AUTO_TEXT = "4e10";

        constexpr ImVec4 ERROR_COLOR = {1.0f, 0.4f, 0.4f, 1.0f};
        constexpr ImVec4 GOOD_COLOR = {0.5f, 0.9f, 0.5f, 1.0f};
        constexpr ImVec4 WARNING_COLOR = {1.0f, 0.8f, 0.3f, 1.0f};

        std::string ReadText(const fs::path& path) {
            std::ifstream file(path, std::ios::binary);

            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }

        std::string Lower(std::string_view text) {
            std::string lower(text);
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            return lower;
        }

        std::string JoinPath(const std::vector<std::string>& keyPath) {
            std::string text;
            for (const std::string& key : keyPath) {
                if (!text.empty())
                    text += '.';

                text += key;
            }

            return text;
        }

        // ln(A)(A = 仮数 × 10^指数)
        double LogPreExponential(const script::RuleRow& rule) {
            return std::log(static_cast<double>(rule.preExponentialMantissa)) +
                   (rule.preExponentialExponent10 * std::numbers::ln10);
        }

        // 始まる温度(K)。A が START_RATE_PER_SECOND 以下なら、どの温度でも届かない(無し)
        std::optional<double> StartKelvin(const script::RuleRow& rule) {
            const double logRatio = LogPreExponential(rule) - std::log(START_RATE_PER_SECOND);
            if (logRatio <= 0.0)
                return std::nullopt;

            return static_cast<double>(rule.activationEnergyJoulesPerMol) / (GAS_CONSTANT * logRatio);
        }

        // 始まる温度 kelvin になる活性化エネルギー(J/mol。整数に丸める)
        std::optional<int64_t> ActivationForStart(const script::RuleRow& rule, int kelvin) {
            const double logRatio = LogPreExponential(rule) - std::log(START_RATE_PER_SECOND);
            if (logRatio <= 0.0 || kelvin <= 0)
                return std::nullopt;

            return std::llround(GAS_CONSTANT * kelvin * logRatio);
        }

        std::string KiloJoules(int64_t joules) {
            return std::format("{:.1f} kJ/mol", static_cast<double>(joules) / 1000.0);
        }

    }  // namespace

    void ReactionTablePanel::UseTable(std::shared_ptr<const script::LoadedReactionTable> table) {
        const bool same = m_table != nullptr && table != nullptr && table->tableVersion == m_table->tableVersion &&
                          table->packageRoot == m_table->packageRoot;
        if (table == nullptr || same)
            return;

        m_table = std::move(table);
        auto document = script::BuildReactionTableDocument(*m_table);
        if (!document) {
            m_document.reset();
            m_documentError = document.error();
            return;
        }

        m_document = std::move(*document);
        m_documentError.clear();
    }

    // --- 値を書く ---

    bool ReactionTablePanel::Write(const std::vector<std::string>& keyPath, const std::string& text) {
        const script::ReactionValue* value = m_document ? script::FindReactionValue(*m_document, keyPath) : nullptr;
        if (value == nullptr) {
            m_message = std::format("{} が今の表に無い", JoinPath(keyPath));
            m_messageFailed = true;
            return false;
        }

        const std::string before = value->text;
        const auto written = script::WriteReactionValue(*value, text);
        if (!written) {
            m_message = written.error();
            m_messageFailed = true;
            return false;
        }

        m_undo.push_back({.keyPath = keyPath, .text = before});
        m_message = std::format("{} = {} を書いた(ホットリロードが読み直して当てる)", JoinPath(keyPath), text);
        m_messageFailed = false;
        Log(Channel::Tool, Level::Info, "反応表のパネル: {}", m_message);

        return true;
    }

    // 最後に書いた値を戻す(戻すのも書き戻し。ホットリロードが当てる)
    bool ReactionTablePanel::Undo() {
        if (m_undo.empty())
            return false;

        const UndoEntry entry = std::move(m_undo.back());
        m_undo.pop_back();
        const bool written = Write(entry.keyPath, entry.text);
        if (written)
            m_undo.pop_back();  // 戻した書き込み自身は積まない

        return written;
    }

    // --- パネル ---

    void ReactionTablePanel::Build(const ReactionTableStatus& status) {
        if (m_autoStage != AutoStage::Off)
            RunAuto(status);

        ImGui::SetNextWindowPos({420.0f, 12.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({620.0f, 520.0f}, ImGuiCond_FirstUseEver);
        ImGui::Begin("反応表");
        BuildHeader(status);
        if (!m_document) {
            ImGui::TextColored(ERROR_COLOR, "一覧を作れない: %s", m_documentError.c_str());
            ImGui::End();
            return;
        }

        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint("##search", "検索(名前・式)", m_search.data(), m_search.size());
        BuildEditor();
        BuildConservation();
        if (ImGui::BeginTabBar("##reaction_table")) {
            if (ImGui::BeginTabItem("規則")) {
                BuildRules();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("物質")) {
                BuildSpecies();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("元素")) {
                BuildElements();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void ReactionTablePanel::BuildHeader(const ReactionTableStatus& status) const {
        const std::string root = m_document && !m_document->packageRoot.empty()
                                     ? m_document->packageRoot.generic_string()
                                     : std::string("(書き戻すフォルダ無し)");
        ImGui::TextDisabled("版 %016llx・%s", static_cast<unsigned long long>(m_table ? m_table->tableVersion : 0),
                            root.c_str());
        if (!status.watching)
            ImGui::TextDisabled("ファイルを見ていない(再生中)。書いても世界には当たらない");
        else if (status.waiting)
            ImGui::TextUnformatted("読めた表が刻みの境界を待っている");

        if (status.lastFailed)
            ImGui::TextColored(ERROR_COLOR, "読み直せなかった(古い表のまま): %s", status.message.c_str());
    }

    // 保存則の検査: 元素の釣り合い(崩れていればベイクが落とす)・生成エンタルピーの差と書いた反応熱・ベイクの警告
    void ReactionTablePanel::BuildConservation() const {
        if (!ImGui::CollapsingHeader("保存則の検査", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        size_t balanced = 0;
        for (const script::RuleRow& rule : m_document->rules) {
            if (rule.elementImbalance.empty())
                ++balanced;
        }

        const bool allBalanced = balanced == m_document->rules.size();
        ImGui::TextColored(allBalanced ? GOOD_COLOR : ERROR_COLOR, "元素の釣り合い: %zu / %zu 本の規則", balanced,
                           m_document->rules.size());
        ImGui::TextDisabled("エネルギー: 反応熱は生成エンタルピーの差でベイクが決める(02 §2)。書いた反応熱は検査用");
        for (const script::RuleRow& rule : m_document->rules) {
            for (const std::string& imbalance : rule.elementImbalance)
                ImGui::TextColored(ERROR_COLOR, "%s: %s", rule.name.c_str(), imbalance.c_str());
        }

        for (const std::string& warning : m_document->warnings)
            ImGui::TextColored(WARNING_COLOR, "警告: %s", warning.c_str());

        if (m_document->warnings.empty())
            ImGui::TextColored(GOOD_COLOR, "ベイクの警告なし");
    }

    bool ReactionTablePanel::Matches(const std::string& name, const std::string& extra) const {
        const std::string query = Lower(m_search.data());
        if (query.empty())
            return true;

        return Lower(name).contains(query) || Lower(extra).contains(query);
    }

    // 値の 1 マス: 押すと選ぶ(書き戻せない値は灰色、理由は浮き出しで)
    void ReactionTablePanel::ValueCell(const script::ReactionValue& value) {
        const bool selected = value.keyPath == m_selected;
        const std::string id = std::format("{}##{}", value.text, JoinPath(value.keyPath));
        if (value.file.empty())
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

        if (ImGui::Selectable(id.c_str(), selected)) {
            m_selected = value.keyPath;
            std::snprintf(m_draft.data(), m_draft.size(), "%s", value.text.c_str());
            m_selectedRule = value.keyPath.size() > 1 && value.keyPath[0] == "reactions" ? value.keyPath[1] : "";
            m_startKelvin = 0;
        }

        if (value.file.empty()) {
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("書き戻せない: %s", value.readOnly.c_str());
        }
    }

    void ReactionTablePanel::BuildRules() {
        constexpr ImGuiTableFlags FLAGS = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
        if (!ImGui::BeginTable("##rules", 6, FLAGS, {0.0f, 260.0f}))
            return;

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("規則");
        ImGui::TableSetupColumn("A (/s)");
        ImGui::TableSetupColumn("Ea (J/mol)");
        ImGui::TableSetupColumn("始まる温度");
        ImGui::TableSetupColumn("反応熱(差)");
        ImGui::TableSetupColumn("書いた反応熱");
        ImGui::TableHeadersRow();
        for (const script::RuleRow& rule : m_document->rules) {
            if (!Matches(rule.name, rule.equation))
                continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(rule.name.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s%s", rule.equation.c_str(),
                                  rule.elementImbalance.empty() ? "" : "\n元素が釣り合わない");
            }

            ImGui::TableNextColumn();
            ValueCell(rule.preExponential);
            ImGui::TableNextColumn();
            ValueCell(rule.activationEnergy);
            ImGui::TableNextColumn();
            const auto start = StartKelvin(rule);
            ImGui::TextUnformatted(start ? std::format("{:.0f} K", *start).c_str() : "届かない");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(KiloJoules(rule.enthalpyJoulesPerMol).c_str());
            ImGui::TableNextColumn();
            if (rule.declaredEnthalpy)
                ValueCell(*rule.declaredEnthalpy);
            else
                ImGui::TextDisabled("—");
        }

        ImGui::EndTable();
    }

    void ReactionTablePanel::BuildSpecies() {
        constexpr ImGuiTableFlags FLAGS = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
        if (!ImGui::BeginTable("##species", 5, FLAGS, {0.0f, 260.0f}))
            return;

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("物質");
        ImGui::TableSetupColumn("組成");
        ImGui::TableSetupColumn("生成エンタルピー (J/mol)");
        ImGui::TableSetupColumn("比熱 (mJ/(mol·K))");
        ImGui::TableSetupColumn("熱伝導率 (mW/(m·K))");
        ImGui::TableHeadersRow();
        for (const script::SpeciesRow& species : m_document->species) {
            if (!Matches(species.name, species.composition))
                continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(species.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(species.composition.c_str());
            ImGui::TableNextColumn();
            ValueCell(species.formationEnthalpy);
            ImGui::TableNextColumn();
            ValueCell(species.heatCapacity);
            ImGui::TableNextColumn();
            ValueCell(species.thermalConductivity);
        }

        ImGui::EndTable();
    }

    void ReactionTablePanel::BuildElements() {
        constexpr ImGuiTableFlags FLAGS = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
        if (!ImGui::BeginTable("##elements", 2, FLAGS))
            return;

        ImGui::TableSetupColumn("元素");
        ImGui::TableSetupColumn("原子量 (mg/mol)");
        ImGui::TableHeadersRow();
        for (const script::ElementRow& element : m_document->elements) {
            if (!Matches(element.name))
                continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(element.name.c_str());
            ImGui::TableNextColumn();
            ValueCell(element.atomicMass);
        }

        ImGui::EndTable();
    }

    // 選んだ値を書く所(と、規則なら係数と始まる温度)
    void ReactionTablePanel::BuildEditor() {
        if (!m_message.empty())
            ImGui::TextColored(m_messageFailed ? ERROR_COLOR : GOOD_COLOR, "%s", m_message.c_str());

        const script::ReactionValue* value = m_selected.empty() ? nullptr
                                                                : script::FindReactionValue(*m_document, m_selected);
        if (value == nullptr) {
            ImGui::TextDisabled("一覧の値を押すと、ここで変えて Luau に書き戻せる");
            return;
        }

        ImGui::SeparatorText(JoinPath(value->keyPath).c_str());
        if (value->file.empty())
            ImGui::TextColored(ERROR_COLOR, "書き戻せない: %s", value->readOnly.c_str());
        else {
            ImGui::SetNextItemWidth(160.0f);
            const bool entered = ImGui::InputText("##draft", m_draft.data(), m_draft.size(),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("書き戻す") || entered)
                static_cast<void>(Write(value->keyPath, m_draft.data()));

            ImGui::SameLine();
            ImGui::TextDisabled("今 %s・%s", value->text.c_str(), value->file.filename().generic_string().c_str());
        }

        ImGui::BeginDisabled(m_undo.empty());
        if (ImGui::Button("最後の書き戻しを戻す"))
            static_cast<void>(Undo());

        ImGui::EndDisabled();
        const auto rule = std::ranges::find(m_document->rules, m_selectedRule, &script::RuleRow::name);
        if (rule != m_document->rules.end())
            BuildStartTemperature(*rule);
    }

    // 始まる温度で書く: Ea を k(T) = START_RATE_PER_SECOND になるように決めて書き戻す(A はそのまま)
    void ReactionTablePanel::BuildStartTemperature(const script::RuleRow& rule) {
        const auto start = StartKelvin(rule);
        if (!start)
            return;

        if (m_startKelvin == 0)
            m_startKelvin = static_cast<int>(std::lround(*start));

        ImGui::TextDisabled("%s: %s", rule.name.c_str(), rule.equation.c_str());
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputInt("始まる温度 (K)", &m_startKelvin, 10, 100);
        ImGui::SameLine();
        ImGui::BeginDisabled(rule.activationEnergy.file.empty());
        if (ImGui::Button("Ea を決めて書く")) {
            if (const auto activation = ActivationForStart(rule, m_startKelvin))
                static_cast<void>(Write(rule.activationEnergy.keyPath, std::format("{}", *activation)));
        }

        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("始まる温度 = 速度定数 k = A·exp(−Ea/RT) が %.2g /s になる温度(表示の定義)",
                              START_RATE_PER_SECOND);
        }
    }

    // --- 人がいない確認(--auto-table-edit)---

    void ReactionTablePanel::FailAuto(std::string why) {
        m_autoFailure = std::move(why);
        m_autoStage = AutoStage::Failed;
        Log(Channel::Tool, Level::Error, "--auto-table-edit: {}", m_autoFailure);
    }

    // 木の燃焼の速さを書く → 当たった表で値が変わった → 戻す → 元のファイル・元の版の表に戻った
    void ReactionTablePanel::RunAuto(const ReactionTableStatus& status) {
        if (!m_document)
            return;

        const std::vector<std::string> keyPath = AutoKeyPath();
        const script::ReactionValue* value = script::FindReactionValue(*m_document, keyPath);
        if (value == nullptr || value->file.empty()) {
            FailAuto(std::format("{} を書き戻せない: {}", JoinPath(keyPath), value ? value->readOnly : "無い"));
            return;
        }

        if (status.failures > m_autoFailures && m_autoStage != AutoStage::Start) {
            FailAuto(std::format("読み直しが落ちた: {}", status.message));
            return;
        }

        switch (m_autoStage) {
            case AutoStage::Start:
                m_autoInitialVersion = m_document->version;
                m_autoOriginal = ReadText(value->file);
                m_autoFailures = status.failures;
                if (!Write(keyPath, AUTO_TEXT)) {
                    FailAuto(m_message);
                    return;
                }

                m_autoStage = AutoStage::Written;
                Log(Channel::Tool, Level::Info, "--auto-table-edit: {} を {} に書いた", JoinPath(keyPath), AUTO_TEXT);
                break;
            case AutoStage::Written:
                if (m_document->version == m_autoInitialVersion)
                    return;

                if (value->text != AUTO_TEXT) {
                    FailAuto(std::format("当たった表の値が {}(期待 {})", value->text, AUTO_TEXT));
                    return;
                }

                Log(Channel::Tool, Level::Info, "--auto-table-edit: 版 {:016x} の表が当たった。元に戻す",
                    m_document->version);
                if (!Undo()) {
                    FailAuto(m_message);
                    return;
                }

                if (ReadText(value->file) != m_autoOriginal) {
                    FailAuto("戻したファイルが元の中身と違う");
                    return;
                }

                m_autoStage = AutoStage::Restoring;
                break;
            case AutoStage::Restoring:
                if (m_document->version != m_autoInitialVersion)
                    return;

                m_autoStage = AutoStage::Done;
                Log(Channel::Tool, Level::Info, "--auto-table-edit: 元の版 {:016x} の表に戻った。OK",
                    m_autoInitialVersion);
                break;
            default: break;
        }
    }

}  // namespace bicameral::editor
