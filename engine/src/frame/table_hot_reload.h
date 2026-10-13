// table_hot_reload.h — 反応表のホットリロードを、世界の刻みの境界へつなぐ(T-0139・ADR-0047・13 §2)。
//
// データの流れ(フレームのループ frame/frame_loop が毎フレーム呼ぶ):
//   Poll(数フレームごと): script::ReactionTableHotReload がファイルの変更を見て読み直す(型検査 → ベイク → 物質の一覧が同じか)
//   → 読めたら「当てたい表」として待つ。落ちたら古い表のまま、誤りをエディタに出す(LastMessage)
//   → TakeSwaps(シミュのリストを記録するフレーム): 適用の単位があるフレームなら、その刻みに差し替えの印のコマンド
//     (sim::MakeTableCommand。版を持つ)を足し、表を sim::ProbeFrameInput::tableSwaps で渡す
//   → 記録(--record)はコマンドの列に印が入り、表の中身は再生ファイルに残る(Find で引く。T-0193)。再生(--replay)は印の版の表を、
//     知っている表(起動時の表・読み直した表・再生ファイルの表〔AddKnown〕)から探して同じ刻みに当てる(持っていなければ止まる)。
// CPU は表(法則のデータ)をベイクして渡すだけで、世界の状態は GPU が新しい表で作り直す(原則 1・ADR-0047)。
// ファイルは再生でなければ通常のゲームの起動でも見る(D-438・T-0243)。読み直した表は「改造された世界」の印を付けて当てる
// (プレイ中に法則が変わった世界。D-437)。印は記録の表にも残る。
#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "script/reaction_hot_reload.h"
#include "script/reaction_table_loader.h"
#include "sim/probe_sim.h"

namespace bicameral::frame {

    class TableHotReload {
    public:
        // watch: ファイルを見るか(再生でなければ通常の起動でも。再生中は見ない)。initial は起動時に読んだ表。
        // policy: 物質を足す・消す表を当てるか(Remap なら世界・覗き窓・実験室が名前で付け替える。T-0242)。既定は Remap
        // (エディタも通常の起動も。D-438・T-0266・ADR-0065)
        TableHotReload(script::ReactionTableSource source, std::shared_ptr<const script::LoadedReactionTable> initial,
                       bool watch, script::SpeciesChangePolicy policy = script::SpeciesChangePolicy::Remap);

        // 知っている表に足す(再生ファイルに残っていた表。T-0193)
        void AddKnown(const std::shared_ptr<const script::LoadedReactionTable>& table) { Remember(table); }

        // 版 version の知っている表(無ければ nullptr)と、起動時の表の版(記録に残す。T-0170)
        [[nodiscard]] std::shared_ptr<const script::LoadedReactionTable> Find(uint64_t version) const;
        [[nodiscard]] uint64_t InitialVersion() const { return m_initialVersion; }

        // 数フレームごとにファイルを見る(読み直しはこのスレッドで同期。エディタのフレームが 1 回止まる)
        void Poll(uint64_t frameNumber);

        // 読む元を替えてすぐ読み直す(環境ミキサーで標準パッケージを足す・外す。T-0452)。読めたら Poll と同じく
        // 次の刻みの境界で当て、以後は替えた読む元のファイルを見る。読めない・当てられなければ false(読む元も今の表も元のまま。
        // 理由は LastMessage)。再生中は呼ばない(呼び手が止める)
        [[nodiscard]] bool ReplaceSource(script::ReactionTableSource source);
        [[nodiscard]] const script::ReactionTableSource& Source() const { return m_reload->Source(); }

        // このフレームの単位 [firstUnit, firstUnit + unitCount)(通しの単位の番号)で当てる差し替え。
        // 生の操作(再生でない): 待っている表があり、このフレームに適用の単位 applyTick があれば、印のコマンドを commands に足す
        // (sequence は nextSequence から。commands は (targetTick, sequence) の順に並べ直す。canAddCommand でなければ次のフレームへ)。
        // 再生: commands の中の印を予定に積む(版を知らなければ error)。返す差し替えはこのフレームの分だけ
        [[nodiscard]] std::expected<std::vector<sim::ProbeTableSwap>, std::string> TakeSwaps(
            uint64_t firstUnit, uint32_t unitCount, uint32_t unitsPerTick, bool replaying, bool canAddCommand,
            uint32_t& nextSequence, std::vector<sim::ProbeCommand>& commands);

        // 当てた表(差し替えの後に GPU が使っている表。表示用)と、最後の読み直しの結果
        [[nodiscard]] uint64_t AppliedVersion() const { return m_appliedVersion; }
        [[nodiscard]] uint32_t AppliedCount() const { return m_appliedCount; }
        [[nodiscard]] uint32_t FailedCount() const { return m_failedCount; }
        [[nodiscard]] bool Watching() const { return m_watchFiles; }
        [[nodiscard]] bool Waiting() const { return m_waiting != nullptr; }
        [[nodiscard]] const std::string& LastMessage() const { return m_lastMessage; }
        [[nodiscard]] bool LastFailed() const { return m_lastFailed; }

        // 世界が「改造された世界」か(起動時の表か、当てた表に印がある。D-437・D-438)
        [[nodiscard]] bool WorldModified() const { return m_worldModified; }

    private:
        struct Scheduled {
            uint64_t tick = 0;
            std::shared_ptr<const script::LoadedReactionTable> table;
        };

        void Remember(const std::shared_ptr<const script::LoadedReactionTable>& table);
        [[nodiscard]] bool Accept(script::HotReloadResult result);  // 読み直しの結果を受ける(Poll・ReplaceSource)
        [[nodiscard]] std::expected<void, std::string> ScheduleReplayed(std::span<const sim::ProbeCommand> commands);

        std::optional<script::ReactionTableHotReload>
            m_reload;  // 読む元と今の表(ファイルを見るのは m_watchFiles の時だけ)
        bool m_watchFiles = false;
        std::map<uint64_t, std::shared_ptr<const script::LoadedReactionTable>> m_known;  // 版 → 表(再生が探す)
        std::shared_ptr<const script::LoadedReactionTable> m_waiting;                    // 読めて、まだ当てていない表
        std::vector<Scheduled> m_scheduled;  // 印を GPU のキューへ足した、まだ記録していない差し替え(刻みの順)
        std::vector<std::shared_ptr<const script::LoadedReactionTable>>
            m_inFlight;  // 返した差し替えの表(RecordFrame が写すまで)

        uint64_t m_initialVersion = 0;
        uint64_t m_appliedVersion = 0;
        uint32_t m_appliedCount = 0;
        uint32_t m_failedCount = 0;
        std::string m_lastMessage;
        bool m_lastFailed = false;
        bool m_worldModified = false;
    };

}  // namespace bicameral::frame
