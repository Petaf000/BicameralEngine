// weight_check.cpp — 重さの検査の判定(T-0413・ADR-0204)。約束は weight_check.h。
#include "frame/weight_check.h"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace bicameral::frame {

    namespace {

        // 重さ ÷ 使ってよい時間(予算が 0 以下なら、少しでも重さがあれば超えたと見なす)
        double Ratio(double milliseconds, double allowanceMilliseconds) {
            if (allowanceMilliseconds > 0.0)
                return milliseconds / allowanceMilliseconds;

            return milliseconds > 0.0 ? 1.0e9 : 0.0;
        }

        // 区間を刻みで伸ばす(いちばん重い刻みを比で選ぶ)
        void Extend(WeightSpan& span, const WeightTick& tick, double ratio) {
            span.lastTick = tick.tick;
            if (ratio <= span.peakRatio)
                return;

            span.peakTick = tick.tick;
            span.peakMilliseconds = tick.gpuMilliseconds;
            span.peakRatio = ratio;
            span.peakBlocks = tick.scheduledBlocks;
        }

        WeightSpan Open(const WeightTick& tick, double ratio) {
            WeightSpan span{.firstTick = tick.tick};
            Extend(span, tick, ratio);

            return span;
        }

        const char* VerdictText(WeightVerdict verdict) {
            switch (verdict) {
                case WeightVerdict::Fits: return "収まる";
                case WeightVerdict::Caution: return "注意";
                case WeightVerdict::Over: return "予算を超える";
            }

            return "?";
        }

    }  // namespace

    double WeightBudget::AllowanceMilliseconds(uint64_t tick) const {
        double allowance = tickMilliseconds;
        for (const WeightSlowWindow& slow : slows) {
            const bool inside = tick >= slow.firstTick && tick - slow.firstTick < slow.tickCount;
            if (inside && slow.speedScale > 0.0)
                allowance = std::max(allowance, tickMilliseconds / std::min(slow.speedScale, 1.0));
        }

        return allowance;
    }

    // --- 写しの早送りの重さの列を判定する ---

    WeightReport CheckWeight(std::span<const WeightTick> ticks, const WeightBudget& budget) {
        WeightReport report;
        std::optional<WeightSpan> open;  // 今続いている超えた区間
        double totalMilliseconds = 0.0;
        double debtMilliseconds = 0.0;  // 予算からはみ出して溜まった仕事(安全網の溜め)

        const auto close = [&] {
            if (!open)
                return;

            if (report.spans.size() < WeightReport::MAX_SPANS)
                report.spans.push_back(*open);

            ++report.spanCount;
            open.reset();
        };

        for (const WeightTick& tick : ticks) {
            const double allowance = budget.AllowanceMilliseconds(tick.tick);
            const double ratio = Ratio(tick.gpuMilliseconds, allowance);
            ++report.ticks;
            totalMilliseconds += tick.gpuMilliseconds;
            report.blockSum += tick.scheduledBlocks;

            // --- いちばん重い刻み・いちばん広い刻み ---
            if (report.ticks == 1 || ratio > report.peakRatio) {
                report.peakRatio = ratio;
                report.peakMilliseconds = tick.gpuMilliseconds;
                report.peakTick = tick.tick;
                report.peakBlocks = tick.scheduledBlocks;
            }

            if (report.ticks == 1 || tick.scheduledBlocks > report.maxBlocks) {
                report.maxBlocks = tick.scheduledBlocks;
                report.maxBlocksTick = tick.tick;
            }

            // --- 超えた・注意 ---
            if (ratio > budget.cautionRatio)
                ++report.cautionTicks;

            if (ratio > 1.0) {
                ++report.overTicks;
                if (open && tick.tick != open->lastTick + 1)
                    close();

                if (open)
                    Extend(*open, tick, ratio);
                else
                    open = Open(tick, ratio);
            } else {
                close();
            }

            // --- 安全網の見込み: 溜めを超えた仕事は捨てる刻み(世界の時間の遅れ)になる ---
            debtMilliseconds = std::max(debtMilliseconds + tick.gpuMilliseconds - allowance, 0.0);
            const double capacity = budget.backlogTicks * allowance;
            if (debtMilliseconds > capacity && allowance > 0.0) {
                report.lagTicks += (debtMilliseconds - capacity) / allowance;
                debtMilliseconds = capacity;
            }
        }

        close();

        if (report.ticks > 0)
            report.meanMilliseconds = totalMilliseconds / static_cast<double>(report.ticks);

        report.verdict = report.overTicks > 0      ? WeightVerdict::Over
                         : report.cautionTicks > 0 ? WeightVerdict::Caution
                                                   : WeightVerdict::Fits;

        return report;
    }

    std::string DescribeWeight(const WeightReport& report, double ticksPerSecond) {
        const double seconds = ticksPerSecond > 0.0 ? static_cast<double>(report.peakTick) / ticksPerSecond : 0.0;

        return std::format(
            "重さ: {}({} 刻み・予算を超えた刻み {}・注意 {}・続けて超えた区間 {})・"
            "いちばん重い刻み {}({:.1f} 秒){:.2f} ms(予算の {:.0f}%・ブロック {})・平均 {:.2f} ms・"
            "ブロックの和 {}・最大 {}(刻み {})・遅れの見込み {:.1f} 刻み",
            VerdictText(report.verdict), report.ticks, report.overTicks, report.cautionTicks, report.spanCount,
            report.peakTick, seconds, report.peakMilliseconds, report.peakRatio * 100.0, report.peakBlocks,
            report.meanMilliseconds, report.blockSum, report.maxBlocks, report.maxBlocksTick, report.lagTicks);
    }

    // --- 本物の刻みの見張り ---

    void WeightWatch::Note(const WeightTick& tick, double allowanceMilliseconds) {
        ++m_measuredTicks;
        const double ratio = Ratio(tick.gpuMilliseconds, allowanceMilliseconds);
        if (ratio <= 1.0)
            return;

        ++m_overTicks;
        if (m_pendingOver++ == 0)
            m_pending = Open(tick, ratio);
        else
            Extend(m_pending, tick, ratio);
    }

    bool WeightWatch::TakeSpan(WeightSpan& span, uint64_t& overTicks) {
        if (m_pendingOver == 0)
            return false;

        span = m_pending;
        overTicks = std::exchange(m_pendingOver, 0);
        m_lastSpan = m_pending;

        return true;
    }

}  // namespace bicameral::frame
