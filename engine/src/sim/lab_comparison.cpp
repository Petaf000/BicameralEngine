// lab_comparison.cpp — 実験室で 2 つの実験を並べて比べる(lab_comparison.h)。
#include "sim/lab_comparison.h"

#include <format>

namespace bicameral::sim {

    namespace {

        struct BranchResult {
            std::vector<LabGaugeSample> gauge;
            LabRecording recording;
        };

        // 段取りの片方を初めの箱から流す。保存点までのハッシュが元の実験と違う・GPU と CPU が食い違う、は失敗
        std::expected<BranchResult, std::string> RunBranch(LabSession& session, const LabComparisonPlan& plan,
                                                           bool withChange) {
            const char* name = withChange ? "B" : "A";
            if (auto replayed = session.Replay(MakeLabBranchRecording(plan, withChange)); !replayed)
                return std::unexpected(std::format("実験 {} を流せない: {}", name, replayed.error()));

            if (const auto& mismatch = session.Mismatch(); mismatch)
                return std::unexpected(std::format("実験 {} の刻み {} で GPU と CPU が食い違った: {}", name,
                                                   mismatch->tick, mismatch->what));

            if (const auto divergence = session.ReplayDivergence(); divergence)
                return std::unexpected(
                    std::format("実験 {} が保存点までに元の実験と違った(刻み {})", name, *divergence));

            return BranchResult{.gauge = session.Gauge(), .recording = session.Recording()};
        }

    }  // namespace

    std::expected<LabComparison, std::string> RunLabComparison(LabSession& session, const LabComparisonPlan& plan) {
        if (plan.tickCount == 0)
            return std::unexpected("流す刻みの数が 0");

        auto a = RunBranch(session, plan, false);
        if (!a)
            return std::unexpected(a.error());

        auto b = RunBranch(session, plan, true);
        if (!b)
            return std::unexpected(b.error());

        LabComparison comparison = {.savePointTick = plan.savePoint.tickCount,
                                    .gaugeCell = session.GaugeCell(),
                                    .a = std::move(a->gauge),
                                    .b = std::move(b->gauge),
                                    .recordingA = std::move(a->recording),
                                    .recordingB = std::move(b->recording)};
        comparison.firstDifference = FirstGaugeDifference(comparison.a, comparison.b);

        return comparison;
    }

}  // namespace bicameral::sim
