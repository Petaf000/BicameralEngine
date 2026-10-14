// time_control.h — エディタの時間の操作: 止める・1 刻み進める・速さ(T-0023、docs/design/14 §2「時間の操作」・ADR-0035)。
//
// データの流れ: エディタのパネル(editor/editor_overlay)とキー → TimeRequest → TimeControl → frame::SimScheduler
// (「現実の時間からいくつ刻みを始めてよいか」)。フレームのループは毎フレーム AdvanceRealTime で経過時間を渡す。
// 時間の操作は「いつ刻みを投げるか」だけを変える Controller の仕事で、世界のコマンドには入らない(D-107・ADR-0035)。
// 世界の結果は刻みの番号と、刻みに付いたコマンドだけで決まる(06 §1・§3)ので、止めても速くしても再生ファイルのハッシュ列は同じになる。
// 形は UE と同じ(D-491・T-0497): 起動は止まった「編集の状態」→ プレイで動かす(動かしたままも編集できる。一時停止・1 刻みはプレイの中)
// → プレイをやめると編集の状態(プレイ前の世界)へ戻る。世界を戻すのはフレームのループ(frame_loop の StopPlay。保存点から戻す)。
// ここは GPU を知らない(CPU だけのテスト tests/time_control_test.cpp)。
#pragma once

#include <array>
#include <cstdint>

#include "frame/sim_scheduler.h"

namespace bicameral::editor {

    // 速さの段(等速 = 1 を真ん中に 1/8〜8 倍)。速くしても 1 フレームの予算(ADR-0011)は変わらないので、
    // 重い世界では上の段にしても追いつかない(追いつけない分は捨てる。D-202)
    inline constexpr std::array<double, 7> TIME_SPEEDS = {0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
    inline constexpr uint32_t NORMAL_SPEED_INDEX = 3;

    // エディタの状態(T-0497・D-491)。エディタなしの起動とプレイの再生はいつも Play
    enum class EditorMode : uint8_t {
        Edit,  // 編集の状態: 時間は進まない(置いたものを当てる刻みだけフレームのループが進める)
        Play,  // プレイ中(一時停止もここ)
    };

    // 1 フレームぶんの時間の操作の依頼(パネルのボタンとキーから)
    struct TimeRequest {
        bool play = false;         // プレイ(編集の状態から始める。一時停止中なら再開)
        bool stopPlay = false;     // プレイをやめる(編集の状態へ。play・togglePause より先に効く)
        bool togglePause = false;  // プレイ中: 一時停止 / 再開。編集の状態: プレイを始める(Space。仮)
        uint32_t stepTicks = 0;    // 止めている間に進める刻みの数(動いている間は無視)
        int32_t speedSteps = 0;    // 速さの段を上げる(+)/ 下げる(−)
        bool resetSpeed = false;   // 等速に戻す(speedSteps より先に効く)
        // この刻みの保存点へ巻き戻す(UINT64_MAX = しない。T-0143)。TimeControl は見ない(フレームのループが保存点から戻す)
        uint64_t rewindTick = UINT64_MAX;

        [[nodiscard]] bool Any() const {
            return play || stopPlay || togglePause || stepTicks > 0 || speedSteps != 0 || resetSpeed ||
                   rewindTick != UINT64_MAX;
        }
    };

    class TimeControl {
    public:
        // 起動を編集の状態にする(--editor で再生でない時。フレームのループが最初に 1 回)
        void StartInEdit() {
            m_mode = EditorMode::Edit;
            m_paused = true;
        }

        // 依頼を受ける。止めたら未処理の刻みを捨てる(始めた刻みは最後まで進む)。
        // 状態が Edit ↔ Play に変わったかはフレームのループが Mode() の前後で見る(世界を戻すのはフレームのループ)
        void Apply(const TimeRequest& request, frame::SimScheduler& scheduler);

        // 現実の経過時間を渡す。止めている間は進めない。gameSpeed はゲーム側の速さ(D-409。1 = 等速)
        void AdvanceRealTime(double seconds, frame::SimScheduler& scheduler, double gameSpeed = 1.0) const;

        // 時間が止まっている(編集の状態か、プレイの一時停止)
        [[nodiscard]] bool Paused() const { return m_paused; }
        [[nodiscard]] EditorMode Mode() const { return m_mode; }
        [[nodiscard]] bool Editing() const { return m_mode == EditorMode::Edit; }
        [[nodiscard]] uint32_t SpeedIndex() const { return m_speedIndex; }
        [[nodiscard]] double Speed() const { return TIME_SPEEDS[m_speedIndex]; }
        [[nodiscard]] uint64_t SteppedTicks() const { return m_steppedTicks; }  // 1 刻みずつ進めた数の合計(表示用)

    private:
        void StartPlay();
        void StopPlay(frame::SimScheduler& scheduler);
        void TogglePause(frame::SimScheduler& scheduler);

        EditorMode m_mode = EditorMode::Play;
        bool m_paused = false;
        uint32_t m_speedIndex = NORMAL_SPEED_INDEX;
        uint64_t m_steppedTicks = 0;
    };

}  // namespace bicameral::editor
