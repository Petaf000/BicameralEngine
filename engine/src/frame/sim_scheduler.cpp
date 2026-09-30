// sim_scheduler.cpp — 1 フレームに投げるシミュの単位の数(T-0012、ADR-0011)。考え方は sim_scheduler.h。
#include "frame/sim_scheduler.h"

#include <algorithm>
#include <cmath>

namespace bicameral::frame {
    namespace {

        double Blend(double average, double sample, double weight) {
            return average <= 0.0 ? sample : average + (sample - average) * weight;
        }

    }  // namespace

    SimScheduler::SimScheduler(uint32_t unitsPerTick, const SimSchedulerSettings& settings)
        : m_settings(settings), m_unitsPerTick(std::max(unitsPerTick, 1u)), m_unitMilliseconds(m_unitsPerTick, 0.0) {}

    void SimScheduler::AddRealTime(double seconds, double speedScale) {
        if (seconds <= 0.0)
            return;

        m_pendingTicks += seconds * m_settings.ticksPerSecond * std::max(speedScale, 0.0);
        // 未処理が上限を超えたら、超えた分は捨てる(追いつこうとして重くなり続けるのを防ぐ。世界が遅くなる。D-202)
        if (m_pendingTicks > m_settings.maxBacklogTicks) {
            const double dropped = std::floor(m_pendingTicks - m_settings.maxBacklogTicks);
            m_droppedTicks += static_cast<uint64_t>(dropped);
            m_pendingTicks -= dropped;
        }
    }

    void SimScheduler::ReportUnitTime(uint32_t unit, double gpuMilliseconds) {
        if (unit >= m_unitsPerTick || gpuMilliseconds <= 0.0)
            return;

        m_unitMilliseconds[unit] = Blend(m_unitMilliseconds[unit], gpuMilliseconds, m_settings.smoothing);
    }

    void SimScheduler::ReportRenderTime(double gpuMilliseconds) {
        if (gpuMilliseconds <= 0.0)
            return;

        m_renderMilliseconds = Blend(m_renderMilliseconds, gpuMilliseconds, m_settings.smoothing);
    }

    double SimScheduler::BudgetMilliseconds() const {
        const double frameMilliseconds = 1000.0 / m_settings.targetFps;
        return std::max(frameMilliseconds - m_renderMilliseconds - m_settings.marginMilliseconds,
                        m_settings.minBudgetMilliseconds);
    }

    // まだ測っていない単位は予算いっぱいと見なす(1 フレームに 1 つだけ投げて、測ってから増やす)
    double SimScheduler::EstimateMilliseconds(uint32_t unit, double budgetMilliseconds) const {
        const double measured = m_unitMilliseconds[unit];
        return measured > 0.0 ? measured : budgetMilliseconds;
    }

    uint32_t SimScheduler::TakeUnits() {
        const double budget = BudgetMilliseconds();
        double used = 0.0;
        uint32_t count = 0;

        while (count < m_settings.maxUnitsPerFrame) {
            const bool startsTick = m_cursor.unit == 0;
            // 現実の時間より先へは進めない
            if (startsTick && m_pendingTicks < 1.0)
                break;

            const double estimate = EstimateMilliseconds(m_cursor.unit, budget);
            if (count > 0 && used + estimate > budget)
                break;

            if (startsTick)
                m_pendingTicks -= 1.0;

            used += estimate;
            ++count;
            if (++m_cursor.unit == m_unitsPerTick) {
                m_cursor.unit = 0;
                ++m_cursor.tick;
            }
        }

        return count;
    }

}  // namespace bicameral::frame
