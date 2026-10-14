// auto_play.h — 人がいない確かめ --auto-play(T-0497・D-491。UE と同じ形のエディタ)の手順と判定。GPU を知らない。
//
// 手順(--world sample --editor。条件が揃ったら次へ進む。フレームの数には頼らない):
//   1. 起動から AUTO_PLAY_HOLD_FRAMES フレーム、世界が刻み 0 の前で止まっている(編集の状態)
//   2. 編集の状態で木の球 A → 木炭 B を置く(置いた分の刻みだけ進む)。最後の刻み E の状態のハッシュ h(AB) を控える
//   3. 元に戻す(B)→ h(E) が h(AB) と違う。元に戻す(A)→ さらに違う。やり直す(A)→ 1 回目の元に戻す後と同じ。
//      やり直す(B)→ h(AB) と同じ(止まっている間の Undo/Redo を何回でも)
//   4. プレイ(始めた刻み P)→ 60 刻み後にプレイ中に木の球 C を置く → 刻み P + 100 でやめる
//   5. 編集の状態へ戻った: カーソルが刻み P の境界・その後 20 フレーム動かない
//   6. もう一度プレイ → 刻み P〜P+50 のハッシュが 1 回目のプレイと同じ(プレイ前の世界に戻った)・P+60〜P+100 のどこかが違う
//      (C が消えた)→ やめて終える
// フレームのループ(frame_loop)は毎フレーム Update に今の様子を渡し、返った操作をボタンと同じ道で行う。
// ハッシュは読み戻した刻みごとに NoteHash へ(巻き戻しの流し直しで同じ刻みがまた届く。届いた時の「世代」で見分ける)。
#pragma once

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
        static constexpr uint64_t SAME_TICKS = 50;      // 2 回のプレイで同じはずの刻みの数
        static constexpr uint64_t STILL_FRAMES = 20;    // やめた後に動かないことを見るフレーム

        [[nodiscard]] AutoPlayAction Update(const AutoPlayState& state);

        // 読み戻した刻み tick の状態のハッシュ
        void NoteHash(uint64_t tick, uint64_t worldHash);

        // 世界を保存点へ戻した(戻す前に読み戻しを全部読んだ後。これより後に届くハッシュは流し直した世界のもの)
        void NoteRewound() { ++m_generation; }

        [[nodiscard]] bool Finished() const { return m_step == Step::Done || m_failed; }
        [[nodiscard]] bool Passed() const { return m_step == Step::Done && !m_failed; }
        [[nodiscard]] const std::string& Summary() const { return m_summary; }

    private:
        enum class Step : uint8_t {
            Hold,
            PlaceA,
            PlaceB,
            UndoB,
            UndoA,
            RedoA,
            RedoB,
            Check,
            Playing,
            PlayingAfterC,
            Stopped,
            Replaying,
            Done,
        };

        // 編集の状態で、操作の後の流し直しが終わり、刻み tick の状態が今の世代で届いた
        [[nodiscard]] bool Settled(const AutoPlayState& state, uint64_t tick) const;
        [[nodiscard]] uint64_t HashAt(uint64_t tick) const;
        AutoPlayAction Act(Step next, AutoPlayAction action);
        void Fail(std::string reason);
        [[nodiscard]] AutoPlayAction UpdateEdit(const AutoPlayState& state);
        [[nodiscard]] AutoPlayAction UpdatePlay(const AutoPlayState& state);

        Step m_step = Step::Hold;
        bool m_failed = false;
        std::string m_summary;
        uint32_t m_generation = 0;                                   // 操作を 1 つ行うごとに増やす
        std::map<uint64_t, std::pair<uint64_t, uint32_t>> m_hashes;  // 刻み → (ハッシュ, 届いた時の世代)
        std::map<uint64_t, uint64_t> m_firstPlay;                    // 1 回目のプレイの刻み → ハッシュ
        std::map<uint64_t, uint64_t> m_secondPlay;
        bool m_recordFirst = false;
        bool m_recordSecond = false;

        uint64_t m_editTick = 0;  // 編集の状態で比べる刻み E(最後に置いた刻み)
        uint64_t m_hashAB = 0;
        uint64_t m_hashUndoB = 0;
        uint64_t m_hashUndoA = 0;
        uint64_t m_playStart = 0;   // 1 回目のプレイを始めた刻み P
        uint64_t m_stillFrame = 0;  // やめた後に止まりを見始めたフレーム
        SimCursor m_stillCursor;
    };

}  // namespace bicameral::frame
