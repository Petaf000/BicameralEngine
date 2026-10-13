// mixer_panel.h — エディタの「環境ミキサー」のパネル(14 §2・D-457・T-0452)。UE の Environment Light Mixer のように、
// 標準パッケージ(現実の元素・物質・反応・地球の環境など。data/standard)をボタンで世界に足す・外す。
//
// データの流れ:
//   棚(script::ReadStandardCatalog。パネルを作った時に 1 回読む)→ パネルで「足す」「外す」→ 依存をたどった新しい足す一覧
//   (script::MixAdd・MixRemove)→ TakeRequest(フレームのループが取る)→ frame::TableHotReload::ReplaceSource
//   (ほかのパッケージと同じ読み込み・型検査・ベイク・当てられるかの検査)→ 次の刻みの境界で世界の表を差し替える(ADR-0047・ADR-0055)
//   → 結果を Result で受けて出す。差し替えは記録に印のコマンドと表の中身が残る(再生は棚なしで同じ表を作る)。
// 中身は持たない(D-454): 何があるかは棚のパッケージが決める。世界の状態には触れない(表を当てるのはホットリロードのコマンド。D-107)。
// 足した環境の値(重力)と初めのセル(空気で満たす)は、動いている世界には効かない(表の法則だけ替わる。世界を作り直す T-0453)。
// 人がいない確認(--auto-mixer): ブランクの世界に全部を足す → 当たったのを見る → 元素を外す(依存するものも外れる)→ 空の表に戻ったら終える。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/aliases.h"
#include "script/reaction_table_loader.h"
#include "script/standard_packages.h"

namespace bicameral::editor {

    struct ReactionTableStatus;

    // パネルの依頼: 新しい足す一覧と読むフォルダ
    struct MixRequest {
        std::vector<std::string> mix;
        std::vector<fs::path> folders;
    };

    class MixerPanel {
    public:
        MixerPanel();

        // 起動時に足した一覧(--mix)
        void SetMix(std::vector<std::string> mix) { m_mix = std::move(mix); }

        // 世界が今使っている表(自動の確認が見る)
        void UseTable(std::shared_ptr<const script::LoadedReactionTable> table) { m_table = std::move(table); }

        // ImGui のフレームの中で呼ぶ。replaying なら押せない(再生は記録の表を当てる)
        void Build(const ReactionTableStatus& status, bool replaying);

        // 押されたボタンの依頼(無ければ空)。フレームのループが取って読む元を替え、Result で結果を返す
        [[nodiscard]] std::optional<MixRequest> TakeRequest() { return std::exchange(m_request, std::nullopt); }
        void Result(const MixRequest& request, bool applied, std::string message);

        // --- 人がいない確認(--auto-mixer)---
        void StartAuto() { m_autoStage = AutoStage::Start; }
        [[nodiscard]] bool AutoDone() const { return m_autoStage == AutoStage::Done; }
        [[nodiscard]] bool AutoFinished() const { return AutoDone() || m_autoStage == AutoStage::Failed; }
        [[nodiscard]] const std::string& AutoFailure() const { return m_autoFailure; }

    private:
        void BuildPackage(const script::StandardPackage& package, bool replaying);
        void Request(std::vector<std::string> mix);

        void RunAuto(const ReactionTableStatus& status);
        void FailAuto(std::string why);

        std::optional<script::StandardCatalog> m_catalog;
        std::string m_catalogError;
        std::vector<std::string> m_mix;  // 世界に足した一覧(当たった・当てる途中のもの。名前の順)
        std::shared_ptr<const script::LoadedReactionTable> m_table;
        std::optional<MixRequest> m_request;
        std::string m_message;
        bool m_messageFailed = false;

        // --- 自動の確認 ---
        enum class AutoStage : uint8_t { Off, Start, Added, Removed, Done, Failed };

        AutoStage m_autoStage = AutoStage::Off;
        uint64_t m_autoInitialVersion = 0;
        uint64_t m_autoAddedVersion = 0;
        uint32_t m_autoWaitFrames = 0;
        std::string m_autoFailure;
    };

}  // namespace bicameral::editor
