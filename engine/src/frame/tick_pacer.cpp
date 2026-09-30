// tick_pacer.cpp — 1 フレームに投げる刻みの数(T-0004)。考え方は tick_pacer.h。
#include "frame/tick_pacer.h"

#include <algorithm>
#include <cmath>

namespace bicameral::frame {
    namespace {

        double Blend(double average, double sample, double weight) {
            return average <= 0.0 ? sample : average + (sample - average) * weight;
        }

    }  // namespace

    void TickPacer::AddRealTime(double seconds, double speedScale) {
        if (seconds <= 0.0) return;
        const double clampedSeconds = std::clamp(seconds, m_settings.minFrameSeconds, m_settings.maxFrameSeconds);
        m_frameSeconds = Blend(m_frameSeconds, clampedSeconds, m_settings.smoothing);

        // 未処理が 2 回分を超えたら、超えた分は捨てる(追いつこうとして重くなり続けるのを防ぐ。世界が遅くなる。D-202)
        m_pendingTicks += seconds * m_settings.ticksPerSecond * std::max(speedScale, 0.0);
        const double backlogLimit = 2.0 * m_settings.maxTicksPerBatch;
        if (m_pendingTicks > backlogLimit) {
            const double dropped = std::floor(m_pendingTicks - backlogLimit);
            m_droppedTicks += static_cast<uint64_t>(dropped);
            m_pendingTicks -= dropped;
        }
    }

    void TickPacer::ReportGpuTime(double gpuMilliseconds, uint32_t tickCount) {
        if (tickCount == 0 || gpuMilliseconds <= 0.0) return;
        m_gpuMillisecondsPerTick = Blend(m_gpuMillisecondsPerTick, gpuMilliseconds / tickCount, m_settings.smoothing);
    }

    uint32_t TickPacer::TakeTicks() {
        auto count = static_cast<uint32_t>(std::floor(m_pendingTicks));
        count = std::min(count, m_settings.maxTicksPerBatch);
        if (m_gpuMillisecondsPerTick > 0.0 && m_frameSeconds > 0.0) {
            // 予算に入る数。1 刻みが予算より重くても 1 つは進める(世界を止めない。遅くなるだけ)
            const double budgetMilliseconds = m_frameSeconds * 1000.0 * m_settings.simBudgetFraction;
            const auto affordable =
                static_cast<uint32_t>(std::max(1.0, std::floor(budgetMilliseconds / m_gpuMillisecondsPerTick)));
            count = std::min(count, affordable);
        }
        m_pendingTicks -= count;
        return count;
    }

}  // namespace bicameral::frame
