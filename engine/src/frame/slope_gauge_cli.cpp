// slope_gauge_cli.cpp — bicameral --slope-gauge(slope_gauge_cli.h。T-0518・ADR-0230)。
#include "frame/slope_gauge_cli.h"

#include <algorithm>

#include "core/log.h"
#include "sim/play_metric_measure.h"
#include "sim/slope_gauge.h"

namespace bicameral::frame {

    namespace {

        // 1 つの指標の場面を流して数える
        sim::SlopeGaugeTally GaugeMetric(const sim::BakedReactionTable& table, const sim::SlopeGaugeTable& gauge,
                                         sim::PlayMetric metric, uint64_t maxTicks) {
            metric.maxTicks = std::min(metric.maxTicks, maxTicks);
            sim::SlopeGaugeTally tally;
            const auto watch = [&](uint64_t tick, std::span<const uint64_t> marksBefore,
                                   const sim::ProbeReference& reference) {
                const uint64_t uphillBefore = tally.uphillTicks;
                sim::CountProbeSlopeTick(table, gauge, reference.State(tick), reference.Caches(tick),
                                         reference.State(tick + 1), marksBefore, tick, tally);
                if (tally.uphillTicks != uphillBefore)
                    Log(Channel::Sim, Level::Debug, "坂の計器: 指標 {} の刻み {} で坂を上った反応があった", metric.key,
                        tick);
            };

            const sim::PlayMetricValue value = sim::MeasurePlayMetricReference(table, metric, nullptr, watch);
            Log(Channel::Sim, Level::Info, "坂の計器: 指標 {}({})の場面を {} 刻み流した", metric.key, metric.label,
                value.ticks);

            return tally;
        }

    }  // namespace

    int RunSlopeGaugeCommand(const FrameLoopOptions& options, uint64_t maxTicks) {
        const auto loaded = LoadLoopReactionTable(options);
        if (!loaded) {
            Log(Channel::Sim, Level::Error, "坂の計器: 表を読めない: {}", loaded.error());
            return 1;
        }

        const sim::BakedReactionTable& table = (*loaded)->table;
        if (table.playMetrics.empty()) {
            Log(Channel::Sim, Level::Warning, "坂の計器: 遊びの指標(play_metrics)が無いので流す場面が無い");
            return 0;
        }

        const sim::SlopeGaugeTable gauge = sim::MakeSlopeGaugeTable(table);
        bool mismatched = false;
        for (const sim::PlayMetric& metric : table.playMetrics) {
            const sim::SlopeGaugeTally tally = GaugeMetric(table, gauge, metric, maxTicks);
            const bool uphill = tally.uphillTicks != 0;
            for (const std::string& line : sim::SlopeGaugeReport(table, tally))
                Log(Channel::Sim, uphill ? Level::Warning : Level::Info, "{}", line);

            mismatched = mismatched || tally.replayMismatches != 0;
        }

        return mismatched ? 1 : 0;
    }

}  // namespace bicameral::frame
