// tick_pacer_test.cpp — frame/tick_pacer(1 フレームに投げる刻みの数。06 §4・D-202)を CPU だけで確かめる。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include "frame/tick_pacer.h"

#include <cstdint>
#include <cstdio>

namespace {

    using bicameral::frame::TickPacer;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition) return;
        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

    // frames 回、seconds ずつ時間を進めて、毎フレーム TakeTicks した合計
    uint64_t RunFrames(TickPacer& pacer, int frames, double seconds, double gpuMillisecondsPerTick,
                       double speedScale = 1.0) {
        uint64_t total = 0;
        for (int frame = 0; frame < frames; ++frame) {
            pacer.AddRealTime(seconds, speedScale);
            const uint32_t ticks = pacer.TakeTicks();
            if (ticks > 0 && gpuMillisecondsPerTick > 0.0) pacer.ReportGpuTime(gpuMillisecondsPerTick * ticks, ticks);
            total += ticks;
        }
        return total;
    }

}  // namespace

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

namespace {

    // 軽いときは、フレームレートに関係なく 1 秒 60 刻み(世界と現実が同じ速さ)
    void TestRealTimeRates() {
        TickPacer at60;
        EXPECT(RunFrames(at60, 600, 1.0 / 60.0, 0.1) >= 598);
        EXPECT(at60.DroppedTicks() == 0);

        TickPacer at144;  // 1 フレーム 0 か 1 刻み
        const uint64_t total144 = RunFrames(at144, 1440, 1.0 / 144.0, 0.1);
        EXPECT(total144 >= 598 && total144 <= 600);

        TickPacer at30;  // 1 フレーム 2 刻み
        const uint64_t total30 = RunFrames(at30, 300, 1.0 / 30.0, 0.1);
        EXPECT(total30 >= 598 && total30 <= 600);

        TickPacer slow;  // ゲーム側のスロー(速さ 0.5): 1 秒で 30 刻み(D-409)
        const uint64_t totalSlow = RunFrames(slow, 600, 1.0 / 60.0, 0.1, 0.5);
        EXPECT(totalSlow >= 298 && totalSlow <= 300);
    }

    // 重い: 1 刻み 25ms(予算より重い)→ 1 フレーム 1 刻みに抑え、残りは捨てる(D-202)
    void TestHeavy() {
        TickPacer pacer;
        const uint64_t total = RunFrames(pacer, 600, 1.0 / 20.0, 25.0);  // GPU に引きずられて 20fps になった
        EXPECT(total >= 600 && total <= 603);     // 最初の計測までの数刻みの後は 1 フレーム 1 刻み(1 つは必ず進める)
        EXPECT(pacer.DroppedTicks() > 0);         // 20fps × 1 刻み < 60 刻み/秒 → 世界が遅くなる
        EXPECT(pacer.PendingTicks() <= 2.0 * 8);  // 未処理は 2 回分で頭打ち

        TickPacer light;  // 計測の平均
        RunFrames(light, 10, 1.0 / 30.0, 1.0);
        EXPECT(light.GpuMillisecondsPerTick() > 0.9 && light.GpuMillisecondsPerTick() < 1.1);
    }

    // 投げられないフレームが続いても(シミュが終わっていない)、戻ったら上限の 8 刻みずつ取り返す
    void TestCatchUp() {
        TickPacer pacer;
        for (int frame = 0; frame < 10; ++frame) {
            pacer.AddRealTime(1.0 / 48.0);  // 1.25 刻みずつ → 12.5
        }
        EXPECT(pacer.TakeTicks() == 8);
        EXPECT(pacer.TakeTicks() == 4);
        EXPECT(pacer.TakeTicks() == 0);
    }

}  // namespace

int main() {
    TestRealTimeRates();
    TestHeavy();
    TestCatchUp();
    if (failureCount == 0) std::printf("tick_pacer_test: OK\n");
    return failureCount == 0 ? 0 : 1;
}
