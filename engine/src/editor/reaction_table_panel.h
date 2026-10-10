// reaction_table_panel.h — エディタの「反応表」のパネル(14 §2・T-0219・D-449)。物質と規則の一覧・検索・保存則の検査の結果を出し、
// 値(速さ・活性化エネルギー・始まる温度・反応熱・生成エンタルピー・比熱・係数など)をその場で変えて Luau のファイルに書き戻す。
//
// データの流れ:
//   フレームのループ → UseTable(世界が今使っている表。ホットリロードで当てた版)→ script::BuildReactionTableDocument で一覧を作り直す
//   → パネルで値を選んで書く → script::WriteReactionValue(そのリテラルの字面だけ差し替える)→ ファイル
//   → ホットリロード(frame/table_hot_reload。ADR-0047)が読み直し、型検査・ベイクの検査を通れば刻みの境界で世界に当てる
//   → 次のフレームの UseTable で新しい表が来る。落ちたら古い表のまま、理由をこのパネルにも出す(ReactionTableStatus)。
// 世界には触れない(ファイルを書くだけ。表を当てるのはホットリロードのコマンド。D-107)。書き戻すのは --packages のフォルダ
// (無ければ exe の横の data/packages。リポジトリの data/packages を直すなら --packages で渡す)。
// 人がいない確認(--auto-table-edit): 木の燃焼の速さを書き換え → 当たったのを見て → 元に戻す → 元のファイル・元の版に戻ったら終える。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "script/reaction_table_edit.h"

namespace bicameral::editor {

    struct ReactionTableStatus;

    class ReactionTablePanel {
    public:
        // 世界が今使っている表(版が変わった時だけ一覧を作り直す。作り直すのはパネルを開いている時だけ)。Build の前に呼ぶ
        void UseTable(std::shared_ptr<const script::LoadedReactionTable> table);

        // ImGui のフレームの中で呼ぶ。status はホットリロードの結果(状態のパネルと同じもの)
        void Build(const ReactionTableStatus& status);

        // --- 人がいない確認(--auto-table-edit)---
        void StartAuto() { m_autoStage = AutoStage::Start; }
        [[nodiscard]] bool AutoDone() const { return m_autoStage == AutoStage::Done; }
        [[nodiscard]] bool AutoFailed() const { return m_autoStage == AutoStage::Failed; }
        [[nodiscard]] bool AutoFinished() const { return AutoDone() || AutoFailed(); }
        [[nodiscard]] const std::string& AutoFailure() const { return m_autoFailure; }

    private:
        // --- 値を書く(パネルと自動の確認が同じ道を通る)---
        bool Write(const std::vector<std::string>& keyPath, const std::string& text);
        bool Undo();

        // --- パネル ---
        void BuildHeader(const ReactionTableStatus& status) const;
        void BuildConservation() const;
        void BuildElements();
        void BuildSpecies();
        void BuildRules();
        void BuildEditor();
        void BuildStartTemperature(const script::RuleRow& rule);
        void ValueCell(const script::ReactionValue& value);
        [[nodiscard]] bool Matches(const std::string& name, const std::string& extra = {}) const;

        // 一覧(m_document)を今の表から作り直す(古い時だけ)。debug 版では重いので、パネルを開いた時と自動の確認の時だけ呼ぶ
        void EnsureDocument();

        // --- 自動の確認 ---
        void RunAuto(const ReactionTableStatus& status);
        void FailAuto(std::string why);

        std::shared_ptr<const script::LoadedReactionTable> m_table;
        std::optional<script::ReactionTableDocument> m_document;
        std::string m_documentError;
        bool m_documentStale = false;  // m_table が替わって、一覧がまだ古い

        // --- 操作の状態(View。世界に入らない)---
        std::array<char, 64> m_search{};
        std::vector<std::string> m_selected;  // 選んだ値の鍵の並び(空なら選んでいない)
        std::array<char, 64> m_draft{};       // 書く前の字面
        std::string m_selectedRule;           // 始まる温度を出す規則(選んだ値の規則)
        int m_startKelvin = 0;                // 始まる温度の入力
        std::string m_message;                // 最後に書いた結果
        bool m_messageFailed = false;

        struct UndoEntry {
            std::vector<std::string> keyPath;
            std::string text;  // 書く前の値
        };

        std::vector<UndoEntry> m_undo;

        // --- 自動の確認 ---
        enum class AutoStage : uint8_t { Off, Start, Written, Restoring, Done, Failed };

        AutoStage m_autoStage = AutoStage::Off;
        uint64_t m_autoInitialVersion = 0;
        std::string m_autoOriginal;  // 書き換える前のファイルの中身
        std::string m_autoFailure;
        uint32_t m_autoFailures = 0;  // 始めた時の読み直しの失敗の数
    };

}  // namespace bicameral::editor
