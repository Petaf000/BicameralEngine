// sim_scheduler_test.cpp — frame/sim_scheduler(1 フレームに投げる単位の数。ADR-0011・06 §4・D-202)を CPU だけで確かめる。
// GPU の代わりに「単位ごとの GPU 時間」を決めておき、投げた単位の時間をそのまま報告する(数フレームの遅れは無視する)。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "frame/sim_scheduler.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "core/aliases.h"

namespace {

    using namespace bicameral;
    using frame::SimCursor;
    using frame::SimScheduler;
    using frame::SimSchedulerSettings;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

    struct RunTotals {
        uint64_t ticks = 0;  // 終わった刻み(カーソルの刻みの番号の進み)
        uint32_t maxUnitsPerFrame = 0;
        double maxSimMilliseconds = 0.0;  // 1 フレームに投げた単位の GPU 時間の最大
    };

    // frames 回、seconds ずつ時間を進め、毎フレーム TakeUnits して、その単位の時間(unitMilliseconds)を報告する
    RunTotals RunFrames(SimScheduler& scheduler, int frames, double seconds, span<const double> unitMilliseconds,
                        double renderMilliseconds = 2.0, double speedScale = 1.0) {
        RunTotals totals;
        const uint64_t firstTick = scheduler.Cursor().tick;
        for (int frame = 0; frame < frames; ++frame) {
            scheduler.AddRealTime(seconds, speedScale);
            SimCursor cursor = scheduler.Cursor();
            const uint32_t count = scheduler.TakeUnits();
            double simMilliseconds = 0.0;

            for (uint32_t index = 0; index < count; ++index) {
                scheduler.ReportUnitTime(cursor.unit, unitMilliseconds[cursor.unit]);
                simMilliseconds += unitMilliseconds[cursor.unit];
                if (++cursor.unit == scheduler.UnitsPerTick())
                    cursor = {.tick = cursor.tick + 1, .unit = 0};
            }

            Expect(cursor == scheduler.Cursor(), "cursor == scheduler.Cursor()", __LINE__);
            scheduler.ReportRenderTime(renderMilliseconds);
            totals.maxUnitsPerFrame = std::max(totals.maxUnitsPerFrame, count);
            if (frame >= 10)
                totals.maxSimMilliseconds = std::max(totals.maxSimMilliseconds, simMilliseconds);
        }

        totals.ticks = scheduler.Cursor().tick - firstTick;

        return totals;
    }

}  // namespace

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

namespace {

    // 軽いときは、フレームレートに関係なく 1 秒 60 刻み(世界と現実が同じ速さ)
    void TestRealTimeRates() {
        const std::vector<double> light = {0.01, 0.02, 0.01};
        SimScheduler at60(3);
        EXPECT(RunFrames(at60, 600, 1.0 / 60.0, light).ticks >= 598);
        EXPECT(at60.DroppedTicks() == 0);

        SimScheduler at165(3);  // 1 フレーム 0 か 1 刻み
        const uint64_t ticks165 = RunFrames(at165, 1650, 1.0 / 165.0, light).ticks;
        EXPECT(ticks165 >= 598 && ticks165 <= 600);

        SimScheduler at30(3);  // 1 フレーム 2 刻み
        const uint64_t ticks30 = RunFrames(at30, 300, 1.0 / 30.0, light).ticks;
        EXPECT(ticks30 >= 598 && ticks30 <= 600);

        SimScheduler slow(3);  // ゲーム側のスロー(速さ 0.5): 1 秒で 30 刻み(D-409)
        const uint64_t ticksSlow = RunFrames(slow, 600, 1.0 / 60.0, light, 2.0, 0.5).ticks;
        EXPECT(ticksSlow >= 298 && ticksSlow <= 300);
    }

    // 予算: 1000 / 目標 fps − 描画 − 余裕(下限あり)
    void TestBudget() {
        SimScheduler scheduler(1, {.targetFps = 60.0, .marginMilliseconds = 1.0});
        EXPECT(scheduler.BudgetMilliseconds() > 15.66 && scheduler.BudgetMilliseconds() < 15.67);
        scheduler.ReportRenderTime(4.0);
        EXPECT(scheduler.BudgetMilliseconds() > 11.66 && scheduler.BudgetMilliseconds() < 11.67);
        scheduler.ReportRenderTime(100.0);  // 描画が重すぎても下限は残す
        EXPECT(scheduler.BudgetMilliseconds() >= 0.5);

        SimScheduler at30(1, {.targetFps = 30.0});
        EXPECT(at30.BudgetMilliseconds() > 32.3 && at30.BudgetMilliseconds() < 32.4);
    }

    // 重い: 1 刻み = 適用 0.1 + 拡散 0.1 + 重さ 3 ms × 12 + ハッシュ 0.1 ≈ 36 ms(予算より重い)
    // → 刻みはフレームをまたぎ、1 フレームに投げる時間は予算以下(1 単位の分だけ超えうる)、世界は遅くなる
    void TestHeavySpansFrames() {
        std::vector<double> heavy = {0.1, 0.1};
        heavy.insert(heavy.end(), 12, 3.0);
        heavy.push_back(0.1);
        SimScheduler scheduler(static_cast<uint32_t>(heavy.size()));
        const RunTotals totals = RunFrames(scheduler, 600, 1.0 / 60.0, heavy);  // 10 秒・描画 2 ms
        const double budget = scheduler.BudgetMilliseconds();                   // 16.67 − 2 − 1
        EXPECT(totals.maxSimMilliseconds <= budget);
        // 1 フレーム約 12 ms × 600 フレーム / 36.3 ms ≈ 200 刻み(世界は 60 → 約 20 刻み/秒)
        EXPECT(totals.ticks >= 180 && totals.ticks <= 210);
        EXPECT(scheduler.DroppedTicks() > 300);   // 追いつけない分は捨てる(D-202)
        EXPECT(scheduler.PendingTicks() <= 8.0);  // 未処理は上限で頭打ち
    }

    // 1 単位が予算より重くても、1 フレームに 1 つは進める(世界を止めない)
    void TestUnitHeavierThanBudget() {
        const std::vector<double> units = {40.0, 40.0};
        SimScheduler scheduler(2);
        const RunTotals totals = RunFrames(scheduler, 60, 1.0 / 60.0, units);
        EXPECT(totals.maxUnitsPerFrame == 1);
        EXPECT(totals.ticks == 30);
    }

    // まだ測っていない単位は予算いっぱいと見なす: 最初のフレームは 1 単位だけ
    void TestUnmeasuredIsCautious() {
        SimScheduler scheduler(4);
        scheduler.AddRealTime(1.0 / 60.0);
        EXPECT(scheduler.TakeUnits() == 1);
        EXPECT((scheduler.Cursor() == SimCursor{.tick = 0, .unit = 1}));
    }

    // 投げられないフレームが続いても(シミュの枠が空いていない)、戻ったら未処理の上限の分だけ取り返す
    void TestCatchUpLimit() {
        SimScheduler scheduler(2);
        for (uint32_t unit = 0; unit < 2; ++unit)
            scheduler.ReportUnitTime(unit, 0.1);

        for (int frame = 0; frame < 10; ++frame)
            scheduler.AddRealTime(1.0 / 48.0);  // 1.25 刻みずつ → 12.5(上限 8 を超えた分は捨てる)

        EXPECT(scheduler.PendingTicks() < 9.0);
        EXPECT(scheduler.TakeUnits() == 16);  // 8 刻み分(残りは捨てた)
        EXPECT(scheduler.TakeUnits() == 0);
    }

}  // namespace

int main() {
    TestRealTimeRates();
    TestBudget();
    TestHeavySpansFrames();
    TestUnitHeavierThanBudget();
    TestUnmeasuredIsCautious();
    TestCatchUpLimit();
    if (failureCount == 0)
        std::printf("sim_scheduler_test: OK\n");

    return failureCount == 0 ? 0 : 1;
}
