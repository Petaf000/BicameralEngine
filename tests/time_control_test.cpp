// time_control_test.cpp — editor/time_control(止める・1 刻み進める・速さ。T-0023・ADR-0035)を CPU だけで確かめる。
// frame::SimScheduler を本物のまま使い、単位の GPU 時間は小さい値に決めておく(予算で止まらないように)。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "editor/time_control.h"

#include <cstdint>
#include <cstdio>

namespace {

    using namespace bicameral;
    using editor::TimeControl;
    using editor::TimeRequest;
    using frame::SimScheduler;

    constexpr uint32_t UNITS_PER_TICK = 3;
    constexpr double FRAME_SECONDS = 1.0 / 60.0;
    constexpr double UNIT_MILLISECONDS = 0.1;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // frames 回、1/60 秒ずつ時間を進めて単位を投げる。始めた刻みの数(カーソルの刻みの進み)を返す
    uint64_t RunFrames(TimeControl& time, SimScheduler& scheduler, int frames,
                       double unitMilliseconds = UNIT_MILLISECONDS) {
        const uint64_t firstTick = scheduler.Cursor().tick;
        for (int frame = 0; frame < frames; ++frame) {
            time.AdvanceRealTime(FRAME_SECONDS, scheduler);
            static_cast<void>(scheduler.TakeUnits());  // 投げた単位の数はここでは見ない
            for (uint32_t unit = 0; unit < UNITS_PER_TICK; ++unit)
                scheduler.ReportUnitTime(unit, unitMilliseconds);

            scheduler.ReportRenderTime(1.0);
        }

        return scheduler.Cursor().tick - firstTick;
    }

    SimScheduler MakeScheduler(double unitMilliseconds = UNIT_MILLISECONDS) {
        SimScheduler scheduler(UNITS_PER_TICK);
        for (uint32_t unit = 0; unit < UNITS_PER_TICK; ++unit)
            scheduler.ReportUnitTime(unit, unitMilliseconds);

        return scheduler;
    }

    // 止めると刻みの境界で止まり、止めている間は進まない。動かすとまた 60 刻み/秒
    void TestPauseStopsAtTickBoundary() {
        TimeControl time;
        SimScheduler scheduler = MakeScheduler();
        RunFrames(time, scheduler, 30);

        time.Apply({.togglePause = true}, scheduler);
        EXPECT(time.Paused());
        RunFrames(time, scheduler, 5);  // 始めた刻みの残りの単位が終わる
        EXPECT(scheduler.Cursor().unit == 0);

        const uint64_t pausedTick = scheduler.Cursor().tick;
        EXPECT(RunFrames(time, scheduler, 120) == 0);
        EXPECT(scheduler.Cursor().tick == pausedTick);

        time.Apply({.togglePause = true}, scheduler);
        EXPECT(!time.Paused());

        const uint64_t ticks = RunFrames(time, scheduler, 60);
        EXPECT(ticks >= 59 && ticks <= 61);
    }

    // 刻みがフレームをまたぐ重さ(1 フレームに 1 単位)で刻みの途中に止めても、始めた刻みは最後まで進んで境界で止まる
    void TestPauseInsideTick() {
        constexpr double HEAVY_UNIT_MILLISECONDS = 10.0;
        TimeControl time;
        SimScheduler scheduler = MakeScheduler(HEAVY_UNIT_MILLISECONDS);
        RunFrames(time, scheduler, 4, HEAVY_UNIT_MILLISECONDS);
        EXPECT(scheduler.Cursor().unit != 0);

        const uint64_t tickInProgress = scheduler.Cursor().tick;
        time.Apply({.togglePause = true}, scheduler);
        RunFrames(time, scheduler, 30, HEAVY_UNIT_MILLISECONDS);
        EXPECT(scheduler.Cursor().tick == tickInProgress + 1);
        EXPECT(scheduler.Cursor().unit == 0);
    }

    // 止めている間の 1 刻みは、ちょうど 1 刻み。動いている間の 1 刻みは無視する
    void TestStepAdvancesExactlyOneTick() {
        TimeControl time;
        SimScheduler scheduler = MakeScheduler();
        time.Apply({.stepTicks = 1}, scheduler);  // 動いている間は無視
        EXPECT(time.SteppedTicks() == 0);

        time.Apply({.togglePause = true}, scheduler);
        RunFrames(time, scheduler, 5);
        const uint64_t pausedTick = scheduler.Cursor().tick;

        for (uint64_t step = 1; step <= 4; ++step) {
            time.Apply({.stepTicks = 1}, scheduler);
            RunFrames(time, scheduler, 3);
            EXPECT(scheduler.Cursor().tick == pausedTick + step);
            EXPECT(scheduler.Cursor().unit == 0);
        }

        time.Apply({.stepTicks = 10}, scheduler);
        RunFrames(time, scheduler, 20);
        EXPECT(scheduler.Cursor().tick == pausedTick + 14);
        EXPECT(time.SteppedTicks() == 14);
    }

    // 速さの段: 2 倍で 120 刻み/秒、1/2 で 30 刻み/秒。端で止まり、等速に戻せる
    void TestSpeed() {
        TimeControl time;
        SimScheduler scheduler = MakeScheduler();
        time.Apply({.speedSteps = 1}, scheduler);
        EXPECT(time.Speed() == 2.0);

        uint64_t ticks = RunFrames(time, scheduler, 60);
        EXPECT(ticks >= 118 && ticks <= 121);

        time.Apply({.speedSteps = -1, .resetSpeed = true}, scheduler);
        EXPECT(time.Speed() == 0.5);

        ticks = RunFrames(time, scheduler, 60);
        EXPECT(ticks >= 29 && ticks <= 31);

        time.Apply({.speedSteps = -100}, scheduler);
        EXPECT(time.SpeedIndex() == 0);
        time.Apply({.speedSteps = 100}, scheduler);
        EXPECT(time.SpeedIndex() == editor::TIME_SPEEDS.size() - 1);
        time.Apply({.resetSpeed = true}, scheduler);
        EXPECT(time.SpeedIndex() == editor::NORMAL_SPEED_INDEX);
    }

}  // namespace

int main() {
    TestPauseStopsAtTickBoundary();
    TestPauseInsideTick();
    TestStepAdvancesExactlyOneTick();
    TestSpeed();

    if (failureCount == 0)
        std::printf("time_control: all passed\n");

    return failureCount == 0 ? 0 : 1;
}
