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
// フレームのループ(frame_loop)は毎フレーム Update に今の様子を渡し、返った操作をボタン・キーと同じ道で行う。
// ハッシュは読み戻した刻みごとに NoteHash へ。プレイを押してから、次に世界を保存点へ戻すまで(やめると戻す)をそのプレイの分にする。
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "frame/sim_scheduler.h"

namespace bicameral::frame {

    // 1 フレームの様子
    struct AutoPlayState {
        uint64_t frame = 0;
        SimCursor cursor;
        bool editing = true;         // 編集の状態
        bool settling = false;       // 巻き戻し・流し直し・置いたものを当てる途中(コマンドが待っている)
        uint64_t playStartTick = 0;  // プレイを始めた刻み(プレイ中)
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
    };

    class AutoPlay {
    public:
        static constexpr uint64_t HOLD_FRAMES = 30;     // 起動から止まっていることを見るフレーム
        static constexpr uint64_t STROKE_C_AFTER = 60;  // プレイを始めてから C を置くまでの刻み
        static constexpr uint64_t STOP_AFTER = 100;     // プレイを始めてからやめるまでの刻み
        static constexpr uint64_t SAME_TICKS = 50;      // 2 回のプレイで比べる初めの刻みの数
        static constexpr uint64_t STILL_FRAMES = 20;    // 編集の状態で動かないことを見るフレーム
        static constexpr uint64_t ACTION_FRAMES = 3;    // 操作がフレームのループに届くまで待つフレーム

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
        // プレイ n の刻み P + 1〜P + SAME_TICKS をプレイ 1 と比べる(同じ数・違う数)
        [[nodiscard]] std::pair<uint32_t, uint32_t> CompareWithFirst(uint32_t play, uint64_t from, uint64_t to) const;
        AutoPlayAction Act(Step next, AutoPlayAction action, uint64_t frame);
        AutoPlayAction StartPlay(Step next, uint32_t play, uint64_t frame);
        void Fail(std::string reason);
        [[nodiscard]] AutoPlayAction UpdateEdit(const AutoPlayState& state);
        [[nodiscard]] AutoPlayAction UpdatePlay(const AutoPlayState& state);
        [[nodiscard]] AutoPlayAction UpdateAfterStop(const AutoPlayState& state);

        Step m_step = Step::Hold;
        bool m_failed = false;
        std::string m_summary;

        // --- 操作の待ち ---
        uint64_t m_actionFrame = 0;      // 最後に操作したフレーム
        uint32_t m_rewinds = 0;          // 世界を保存点へ戻した回数
        uint32_t m_rewindsAtAction = 0;  // 最後の操作の時の m_rewinds

        // --- プレイのハッシュ(プレイ 1〜3。刻み → ハッシュ)---
        std::array<std::map<uint64_t, uint64_t>, 4> m_plays;
        uint32_t m_recording = 0;  // 今ハッシュを控えているプレイ(0 = 控えない)

        uint64_t m_editTick = 0;    // 編集の境界 E(= プレイを始める刻み P)
        uint64_t m_stillFrame = 0;  // 止まりを見始めたフレーム(0 = まだ)
        SimCursor m_stillCursor;
    };

}  // namespace bicameral::frame
