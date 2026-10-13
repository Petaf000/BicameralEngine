// weight_check_test.cpp — 重さの検査(frame/weight_check の判定と sim/tick_cost の集め方。T-0413・ADR-0204・D-468)を CPU だけで確かめる。
// 判定: 予算・注意の割合・演出のスローの窓・続けて超えた区間・安全網の見込み。集め方: 刻みがフレームをまたいでも全部の刻みの重さが
// 要約と組になる・読み落としたフレームの後は途中の刻みを数えない。失敗すると条件と行を表示して 1 を返す(ctest が落ちる)。
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "frame/weight_check.h"
#include "sim/tick_cost.h"

namespace {

    using namespace bicameral;
    using frame::WeightBudget;
    using frame::WeightTick;
    using frame::WeightVerdict;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // 刻み 1〜count の重さ(milliseconds[i] が刻み i + 1。ブロックは刻みの番号)
    std::vector<WeightTick> MakeTicks(std::span<const double> milliseconds) {
        std::vector<WeightTick> ticks;
        for (size_t index = 0; index < milliseconds.size(); ++index) {
            ticks.push_back({.tick = index + 1,
                             .gpuMilliseconds = milliseconds[index],
                             .scheduledBlocks = static_cast<uint32_t>(index + 1)});
        }

        return ticks;
    }

    // --- 判定 ---

    void TestFits() {
        const std::vector<double> milliseconds = {2.0, 3.0, 4.0, 3.0};
        const frame::WeightReport report = frame::CheckWeight(MakeTicks(milliseconds), {.tickMilliseconds = 10.0});
        EXPECT(report.verdict == WeightVerdict::Fits);
        EXPECT(report.ticks == 4 && report.overTicks == 0 && report.cautionTicks == 0);
        EXPECT(report.peakTick == 3 && report.peakBlocks == 3);
        EXPECT(report.blockSum == 10 && report.maxBlocks == 4 && report.maxBlocksTick == 4);
        EXPECT(report.meanMilliseconds == 3.0 && report.lagTicks == 0.0 && report.spanCount == 0);
    }

    void TestCautionAndOver() {
        // 注意(8.5 > 8)・超えた区間 2 つ(刻み 3〜4 と 6)
        const std::vector<double> milliseconds = {2.0, 8.5, 12.0, 15.0, 3.0, 11.0};
        const frame::WeightReport report = frame::CheckWeight(MakeTicks(milliseconds), {.tickMilliseconds = 10.0});
        EXPECT(report.verdict == WeightVerdict::Over);
        EXPECT(report.overTicks == 3 && report.cautionTicks == 4);
        EXPECT(report.spanCount == 2 && report.spans.size() == 2);
        EXPECT(report.spans[0].firstTick == 3 && report.spans[0].lastTick == 4 && report.spans[0].peakTick == 4);
        EXPECT(report.spans[0].peakBlocks == 4 && report.spans[0].peakRatio == 1.5);
        EXPECT(report.spans[1].firstTick == 6 && report.spans[1].lastTick == 6);
        EXPECT(report.peakTick == 4 && report.peakMilliseconds == 15.0);

        const std::vector<double> light = {2.0, 8.5};
        EXPECT(frame::CheckWeight(MakeTicks(light), {.tickMilliseconds = 10.0}).verdict == WeightVerdict::Caution);
    }

    // 演出のスローの窓の中は 予算 ÷ 速さ まで使ってよい(D-468: 長さは前もってゲームが決める)
    void TestSlowWindow() {
        const std::vector<double> milliseconds = {5.0, 30.0, 35.0, 30.0, 5.0};
        WeightBudget budget{.tickMilliseconds = 10.0};
        EXPECT(frame::CheckWeight(MakeTicks(milliseconds), budget).overTicks == 3);

        budget.slows.push_back({.firstTick = 2, .tickCount = 3, .speedScale = 0.25});
        const frame::WeightReport slowed = frame::CheckWeight(MakeTicks(milliseconds), budget);
        EXPECT(slowed.overTicks == 0 && slowed.verdict == WeightVerdict::Caution);
        EXPECT(budget.AllowanceMilliseconds(1) == 10.0 && budget.AllowanceMilliseconds(2) == 40.0);
        EXPECT(budget.AllowanceMilliseconds(4) == 40.0 && budget.AllowanceMilliseconds(5) == 10.0);

        // 窓が短すぎると、はみ出した刻みが超える
        budget.slows[0].tickCount = 2;
        const frame::WeightReport shortSlow = frame::CheckWeight(MakeTicks(milliseconds), budget);
        EXPECT(shortSlow.overTicks == 1 && shortSlow.spans.size() == 1 && shortSlow.spans[0].firstTick == 4);
    }

    // 安全網の見込み: 溜め(backlogTicks × 予算)を超えた仕事が、捨てる刻み(世界の時間の遅れ)になる
    void TestLag() {
        const std::vector<double> milliseconds(10, 20.0);  // 毎刻み 10 ms はみ出す
        const frame::WeightReport report = frame::CheckWeight(MakeTicks(milliseconds),
                                                              {.tickMilliseconds = 10.0, .backlogTicks = 2.0});
        // はみ出しの溜め: 10, 20(上限 20), 30 → 1 刻み捨てる … 刻み 3〜10 の 8 回
        EXPECT(report.lagTicks == 8.0);
        EXPECT(report.spanCount == 1 && report.spans[0].firstTick == 1 && report.spans[0].lastTick == 10);
    }

    // 刻みが飛ぶ(数えられなかった刻み)と区間を分ける
    void TestGap() {
        std::vector<WeightTick> ticks = {{.tick = 1, .gpuMilliseconds = 20.0}, {.tick = 3, .gpuMilliseconds = 20.0}};
        const frame::WeightReport report = frame::CheckWeight(ticks, {.tickMilliseconds = 10.0});
        EXPECT(report.spanCount == 2);
    }

    void TestWatch() {
        frame::WeightWatch watch;
        watch.Note({.tick = 5, .gpuMilliseconds = 5.0}, 10.0);
        frame::WeightSpan span;
        uint64_t over = 0;
        EXPECT(!watch.TakeSpan(span, over));

        watch.Note({.tick = 6, .gpuMilliseconds = 12.0, .scheduledBlocks = 7}, 10.0);
        watch.Note({.tick = 7, .gpuMilliseconds = 30.0, .scheduledBlocks = 9}, 10.0);
        watch.Note({.tick = 8, .gpuMilliseconds = 3.0}, 10.0);
        EXPECT(watch.TakeSpan(span, over));
        EXPECT(over == 2 && span.firstTick == 6 && span.lastTick == 7 && span.peakTick == 7 && span.peakBlocks == 9);
        EXPECT(!watch.TakeSpan(span, over));
        EXPECT(watch.MeasuredTicks() == 4 && watch.OverTicks() == 2 && watch.LastSpan().peakTick == 7);
    }

    // --- 集め方 ---

    sim::ProbeTickHash Hash(uint64_t tick, uint32_t blocks) {
        return {.tick = tick, .scheduledBlocks = blocks};
    }

    // 1 刻み 3 単位。フレームの切れ目が刻みの途中に来ても、刻みごとの和が要約と組になる
    void TestAccumulator() {
        sim::TickCostAccumulator accumulator(3);
        std::vector<sim::TickCost> costs;

        // 刻み 0 の単位 0〜1(終えた刻みなし)
        const std::vector<uint64_t> first = {1, 2};
        EXPECT(accumulator.Add(0, first, {}, costs) && costs.empty());

        // 刻み 0 の単位 2・刻み 1 の単位 0〜2・刻み 2 の単位 0 → S(1)・S(2)
        const std::vector<uint64_t> second = {3, 10, 20, 30, 100};
        const std::vector<sim::ProbeTickHash> hashes = {Hash(1, 4), Hash(2, 5)};
        EXPECT(accumulator.Add(2, second, hashes, costs));
        EXPECT(costs.size() == 2);
        EXPECT(costs[0].tick == 1 && costs[0].gpuTimestampTicks == 6 && costs[0].scheduledBlocks == 4);
        EXPECT(costs[1].tick == 2 && costs[1].gpuTimestampTicks == 60 && costs[1].scheduledBlocks == 5);

        // 刻み 2 の単位 1 から始まるはずが単位 2 から届いた(読み落とし)→ 刻み 2 は数えない。刻み 3 は数える
        costs.clear();
        const std::vector<uint64_t> third = {7, 1, 1, 1};
        const std::vector<sim::ProbeTickHash> hashes3 = {Hash(3, 6), Hash(4, 7)};
        EXPECT(accumulator.Add(2, third, hashes3, costs));
        EXPECT(costs.size() == 1 && costs[0].tick == 4 && costs[0].gpuTimestampTicks == 3);

        // 終えた刻みと要約の数が食い違えば何も足さない
        costs.clear();
        const std::vector<uint64_t> fourth = {1, 1, 1};
        EXPECT(!accumulator.Add(0, fourth, {}, costs) && costs.empty());

        // 戻した後は刻みの始めから
        accumulator.Reset();
        const std::vector<sim::ProbeTickHash> hashes5 = {Hash(9, 1)};
        EXPECT(accumulator.Add(0, fourth, hashes5, costs) && costs.size() == 1 && costs[0].gpuTimestampTicks == 3);
    }

}  // namespace

int main() {
    TestFits();
    TestCautionAndOver();
    TestSlowWindow();
    TestLag();
    TestGap();
    TestWatch();
    TestAccumulator();

    if (failureCount > 0) {
        std::printf("weight_check: %d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("weight_check: OK\n");
    return 0;
}
