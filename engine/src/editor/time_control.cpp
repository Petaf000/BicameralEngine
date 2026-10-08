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

        // --- 止める / 動かす ---
        if (request.togglePause) {
            m_paused = !m_paused;
            // 止めた時に溜まっている刻み(最大で約 133 ms 分)を捨てる。残すと止めた後も数刻み進んでしまう
            if (m_paused)
                scheduler.ClearBacklog();
        }

        // --- 1 刻み進める(止めている間だけ)---
        if (m_paused && request.stepTicks > 0) {
            scheduler.AddTicks(request.stepTicks);
            m_steppedTicks += request.stepTicks;
        }
    }

    void TimeControl::AdvanceRealTime(double seconds, frame::SimScheduler& scheduler, double gameSpeed) const {
        if (m_paused)
            return;

        scheduler.AddRealTime(seconds, Speed() * gameSpeed);
    }

}  // namespace bicameral::editor
