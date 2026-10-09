// reaction_hot_reload.h — 反応表のホットリロード(エディタと実験室。T-0139・ADR-0047・13 §2)。
//
// データの流れ: パッケージのフォルダ(と型の定義)のファイルの中身の指紋を Poll のたびに取る
//   → 変わって、次の Poll でも同じ指紋なら(書きかけを読まない)読み直す: LoadReactionTable(型検査 → 殻 → ベイクの検査)
//   → 今の世界に当てられるか(物質の一覧が同じか。CheckHotReloadCompatible)
//   → 通れば新しい表を返す(呼び手が刻みの境界で世界のコマンドとして当てる。sim::ProbeFrameInput::tableSwap)。
//   落ちたら古い表のまま、誤り(どのファイルの何行目か・どの物質か)を返す。
// 指紋はファイルの相対パスと中身だけで決める(時刻を使わない)。ここは CPU だけ(原則 2: 表は CPU でベイクして GPU へ渡す)。
// 読み直しは呼んだスレッドで同期して行う(今の表で数十 ms。エディタのフレームが 1 回止まる)。
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "core/aliases.h"
#include "script/reaction_table_loader.h"
#include "sim/reaction_table.h"

namespace bicameral::script {

    // フォルダの下の全部のファイルの指紋(相対パスのバイト順に、パスと中身を混ぜる)。読めないファイル・フォルダが無い時は
    // 中身の代わりにその印を混ぜる(読めるようになれば変わる)
    [[nodiscard]] uint64_t DirectoryFingerprint(const fs::path& root);

    // 新しい表を今の世界に当てられるか: 物質の一覧(名前・並び = ID・元素の組み立て)が同じなら当てられる。
    // 違えば、セルの物質 ID の意味が変わるので当てない(足した・消した物質の名前を理由に書く。世界を作り直す道は ADR-0047)
    [[nodiscard]] std::expected<void, std::string> CheckHotReloadCompatible(const sim::BakedReactionTable& current,
                                                                            const sim::BakedReactionTable& next);

    enum class HotReloadState : uint8_t {
        Unchanged,  // ファイルが変わっていない(か、変わったが表の版が同じ)
        Waiting,    // 変わったのを見た。次の Poll で同じなら読み直す
        Reloaded,   // 新しい表を読めた(table)。今の表はこれになった
        Failed,     // 読めない・型検査・ベイクの検査・当てられない(message)。今の表のまま
    };

    struct HotReloadResult {
        HotReloadState state = HotReloadState::Unchanged;
        std::shared_ptr<const LoadedReactionTable> table;  // Reloaded のとき
        std::string message;  // Failed のときの理由(Reloaded でも読まなかった Mod があれば)
    };

    class ReactionTableHotReload {
    public:
        // current: 今の世界の表(起動時に読んだもの)。指紋はここで取る(作った時の中身を「見た」とする)
        ReactionTableHotReload(ReactionTableSource source, std::shared_ptr<const LoadedReactionTable> current);

        [[nodiscard]] HotReloadResult Poll();

        [[nodiscard]] const std::shared_ptr<const LoadedReactionTable>& Current() const { return m_current; }

    private:
        [[nodiscard]] uint64_t Fingerprint() const;
        [[nodiscard]] HotReloadResult Reload();

        ReactionTableSource m_source;
        std::shared_ptr<const LoadedReactionTable> m_current;
        uint64_t m_seen = 0;     // 最後に読み直した(か作った時の)指紋
        uint64_t m_pending = 0;  // 変わったのを見た指紋(次の Poll で同じなら読み直す)
    };

}  // namespace bicameral::script
