// time_control.cpp — エディタの時間の操作(T-0023)。考え方は time_control.h。
#include "editor/time_control.h"

#include <algorithm>

namespace bicameral::editor {

    void TimeControl::Apply(const TimeRequest& request, frame::SimScheduler& scheduler) {
        // --- 速さ ---
        if (request.resetSpeed)
            m_speedIndex = NORMAL_SPEED_INDEX;

        const auto lastIndex = static_cast<int32_t>(TIME_SPEEDS.size()) - 1;
        m_speedIndex = static_cast<uint32_t>(
            std::clamp(static_cast<int32_t>(m_speedIndex) + request.speedSteps, 0, lastIndex));

        // --- プレイ・やめる・一時停止(D-491。やめるが先)---
        if (request.stopPlay)
            StopPlay(scheduler);
        else if (request.play)
            StartPlay();
        else if (request.togglePause)
            TogglePause(scheduler);

        // --- 1 刻み進める(プレイの一時停止の間だけ。編集の状態では時間を進めない)---
        if (m_mode == EditorMode::Play && m_paused && request.stepTicks > 0) {
            scheduler.AddTicks(request.stepTicks);
            m_steppedTicks += request.stepTicks;
        }
    }

    // 編集の状態からプレイを始める。一時停止中なら再開する
    void TimeControl::StartPlay() {
        m_mode = EditorMode::Play;
        m_paused = false;
    }

    // プレイをやめて編集の状態へ(時間を止め、溜まっている刻みを捨てる)。編集の状態なら何もしない
    void TimeControl::StopPlay(frame::SimScheduler& scheduler) {
        if (m_mode == EditorMode::Edit)
            return;

        m_mode = EditorMode::Edit;
        m_paused = true;
        scheduler.ClearBacklog();
    }

    // プレイ中は一時停止 / 再開。編集の状態ではプレイを始める(Space。仮。T-0497)
    void TimeControl::TogglePause(frame::SimScheduler& scheduler) {
        if (m_mode == EditorMode::Edit) {
            StartPlay();
            return;
        }

        m_paused = !m_paused;
        // 止めた時に溜まっている刻み(最大で約 133 ms 分)を捨てる。残すと止めた後も数刻み進んでしまう
        if (m_paused)
            scheduler.ClearBacklog();
    }

    void TimeControl::AdvanceRealTime(double seconds, frame::SimScheduler& scheduler, double gameSpeed) const {
        if (m_paused)
            return;

        scheduler.AddRealTime(seconds, Speed() * gameSpeed);
    }

}  // namespace bicameral::editor
