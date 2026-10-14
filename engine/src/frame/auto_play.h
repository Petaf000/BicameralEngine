// auto_play.h — 人がいない確かめ --auto-play(T-0497・D-491。UE と同じ形のエディタ)の手順と判定。GPU を知らない。
//
// 手順(--world sample --editor。条件が揃ったら次へ進む。フレームの数には頼らない)。編集の状態では世界の時間が進まない(T-0509):
//   1. 起動から HOLD_FRAMES フレーム、世界が刻み 0 の前 (0, 0) で止まっている(編集の状態)
//   2. 木の球 A → 木炭 B を置く。どちらも刻み 0 の適用の単位だけで当たり、カーソルは (0, 1) のまま(刻みの番号が変わらない)。
//      STILL_FRAMES フレーム動かない
//   3. プレイ 1(始めた刻み P = 0)→ STROKE_C_AFTER 刻み後にプレイ中に木の球 C → P + STOP_AFTER でやめる
//   4. 編集の状態へ戻った: カーソルが (P, 1)(プレイ前に置いた A・B を当て直した)・STILL_FRAMES フレーム動かない
//   5. 元に戻す(B)→ (P, 1)・元に戻す(A)→ (P, 0)・やり直す(A)→ (P, 1)。どれも刻みの番号は P のまま
//   6. プレイ 2(A だけ)→ 刻み P + 1〜P + SAME_TICKS のどこかがプレイ 1 と違う(B を元に戻した分だけ世界が違う)→ やめる
//   7. やり直す(B)→ プレイ 3 → 刻み P + 1〜P + SAME_TICKS がプレイ 1 と同じ(A・B の世界に戻った)・
//      P + STROKE_C_AFTER〜P + STOP_AFTER のどこかが違う(C が消えた)→ 終える(記録はプレイ 3 の流れ。別の起動で再生して同じハッシュ列)
// 表の手順(--auto-play-table。T-0548・ADR-0246。UseTableScript。--packages の写しの表を書き換える):
//   1. (0, 0) で止まっている → 木の球 A を置く → プレイ 1(表 T0)→ RUN_TICKS 刻みでやめる
//   2. 編集の状態で表を書き換える(物質と重力を足す = T1)→ 時間を進めずに当たる((0, 1) のまま・S(0) へ戻して当て直す)
//   3. 元に戻す(A)→ やり直す(A)。どちらも表は T1 のまま(保存点を捨てない・差し替えの印も当て直す)
//   4. プレイ 2(T1 + A)はプレイ 1 と違う → やめる → プレイ 3 は REVERT_AFTER 刻みまでプレイ 2 と同じ。そこで表を元に戻す(プレイ中の差し替え)
//   5. やめると表はファイルの今の中身(T0)・世界はプレイ前 → もう一度 T1 に書き換える → プレイ 4 はプレイ 2 と同じ → 終える
//      (記録はプレイ 4 の流れ: 刻み 0 の始めに表の印と A。別の起動で再生して同じハッシュ列)
// フレームのループ(frame_loop)は毎フレーム Update に今の様子を渡し、返った操作をボタン・キーと同じ道で行う。
// ハッシュは読み戻した刻みごとに NoteHash へ。プレイを押してから、次に世界を保存点へ戻すまで(やめると戻す)をそのプレイの分にする。
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "frame/auto_reload.h"
#include "frame/sim_scheduler.h"

namespace bicameral::frame {

    // 1 フレームの様子
    struct AutoPlayState {
        uint64_t frame = 0;
        SimCursor cursor;
        bool editing = true;         // 編集の状態
        bool settling = false;       // 巻き戻し・流し直し・置いたものを当てる途中(コマンドが待っている)
        uint64_t playStartTick = 0;  // プレイを始めた刻み(プレイ中)
        uint64_t tableVersion = 0;   // 世界が今使っている反応表の版(表の手順。T-0548)
    };

    // そのフレームに行う操作
    enum class AutoPlayAction : uint8_t {
        None,
        StrokeA,  // 木の球
        StrokeB,  // 木炭を足す
        StrokeC,  // プレイ中に木の球(別の場所)
        Undo,
        Redo,
        Play,
        Stop,
        EditTable,    // 表の手順: パッケージの写しを書き換える(WriteTable)
        RevertTable,  // 表の手順: 元の中身に戻す
    };

    class AutoPlay {
    public:
        static constexpr uint64_t HOLD_FRAMES = 30;     // 起動から止まっていることを見るフレーム
        static constexpr uint64_t STROKE_C_AFTER = 60;  // プレイを始めてから C を置くまでの刻み
        static constexpr uint64_t STOP_AFTER = 100;     // プレイを始めてからやめるまでの刻み
        static constexpr uint64_t SAME_TICKS = 50;      // 2 回のプレイで比べる初めの刻みの数
        static constexpr uint64_t STILL_FRAMES = 20;    // 編集の状態で動かないことを見るフレーム
        static constexpr uint64_t ACTION_FRAMES = 3;    // 操作がフレームのループに届くまで待つフレーム
        static constexpr uint64_t RUN_TICKS = 60;       // 表の手順: 1 回のプレイで比べる刻み
        static constexpr uint64_t REVERT_AFTER = 20;    // 表の手順: プレイ 3 で表を元に戻すまでの刻み

        // 表の手順にする(--auto-play-table。edits はパッケージの写しの書き換え。T-0548)
        void UseTableScript(std::vector<PackageEdit> edits) {
            m_tableEdits = std::move(edits);
            m_tableScript = true;
        }

        // 表の手順の EditTable(edited)/ RevertTable を行う(書けなければ失敗にする)
        void WriteTable(bool edited);

        [[nodiscard]] AutoPlayAction Update(const AutoPlayState& state);

        // 読み戻した刻み tick の状態のハッシュ
        void NoteHash(uint64_t tick, uint64_t worldHash);

        // 世界を保存点へ戻した(戻す前に読み戻しを全部読んだ後。これより後に届くハッシュは今のプレイのものではない)
        void NoteRewound() {
            ++m_rewinds;
            m_recording = 0;
        }

        [[nodiscard]] bool Finished() const { return m_step == Step::Done || m_failed; }
        [[nodiscard]] bool Passed() const { return m_step == Step::Done && !m_failed; }
        [[nodiscard]] const std::string& Summary() const { return m_summary; }

    private:
        enum class Step : uint8_t {
            Hold,
            PlaceA,
            PlaceB,
            EditStill,
            Playing,
            PlayingAfterC,
            Stopped,
            UndoB,
            UndoA,
            RedoA,
            PlayingUndone,
            StoppedUndone,
            RedoB,
            Replaying,
            Done,
        };

        // 編集の状態で、操作の後に止まった(カーソルが編集の境界 (E, unit)・待っているものが無い・要るなら保存点へ戻した)
        [[nodiscard]] bool EditSettled(const AutoPlayState& state, uint32_t unit, bool rewound) const;
        // 編集の状態でカーソルが STILL_FRAMES フレーム動かない(まだなら false。動いたら失敗)
        [[nodiscard]] bool StayedStill(const AutoPlayState& state);
        // プレイ other の刻み from〜to をプレイ first と比べる(同じ数・違う数)
        [[nodiscard]] std::pair<uint32_t, uint32_t> Compare(uint32_t first, uint32_t other, uint64_t from,
                                                            uint64_t to) const;
        AutoPlayAction Act(Step next, AutoPlayAction action, uint64_t frame);
        AutoPlayAction StartPlay(Step next, uint32_t play, uint64_t frame);
        void Fail(std::string reason);
        [[nodiscard]] AutoPlayAction UpdateEdit(const AutoPlayState& state);
        [[nodiscard]] AutoPlayAction UpdatePlay(const AutoPlayState& state);
        [[nodiscard]] AutoPlayAction UpdateAfterStop(const AutoPlayState& state);

        // --- 表の手順(auto_play_table.cpp)---
        enum class TableStep : uint8_t {
            Hold,
            PlaceA,
            Play1,
            Stopped1,
            Edited,
            UndoA,
            RedoA,
            Play2,
            Stopped2,
            Play3,
            Play3Reverted,
            Stopped3,
            EditedAgain,
            Play4,
        };

        [[nodiscard]] AutoPlayAction UpdateTable(const AutoPlayState& state);
        [[nodiscard]] AutoPlayAction UpdateTableEdit(const AutoPlayState& state);
        [[nodiscard]] AutoPlayAction UpdateTablePlay(const AutoPlayState& state);
        // プレイ中なら始めてからの刻み(始めた刻みが編集の境界でなければ失敗)
        [[nodiscard]] std::optional<uint64_t> PlayElapsed(const AutoPlayState& state);
        AutoPlayAction ActTable(TableStep next, AutoPlayAction action, uint64_t frame);

        Step m_step = Step::Hold;
        bool m_failed = false;
        std::string m_summary;

        // --- 操作の待ち ---
        uint64_t m_actionFrame = 0;      // 最後に操作したフレーム
        uint32_t m_rewinds = 0;          // 世界を保存点へ戻した回数
        uint32_t m_rewindsAtAction = 0;  // 最後の操作の時の m_rewinds

        // --- プレイのハッシュ(プレイ 1〜3。刻み → ハッシュ)---
        std::array<std::map<uint64_t, uint64_t>, 5> m_plays;
        uint32_t m_recording = 0;  // 今ハッシュを控えているプレイ(0 = 控えない)

        uint64_t m_editTick = 0;    // 編集の境界 E(= プレイを始める刻み P)
        uint64_t m_stillFrame = 0;  // 止まりを見始めたフレーム(0 = まだ)
        SimCursor m_stillCursor;

        // --- 表の手順 ---
        bool m_tableScript = false;
        TableStep m_tableStep = TableStep::Hold;
        std::vector<PackageEdit> m_tableEdits;
        uint64_t m_baseVersion = 0;    // 起動時の表 T0
        uint64_t m_editedVersion = 0;  // 書き換えた表 T1
    };

}  // namespace bicameral::frame
