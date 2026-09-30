// tick_pacer.h — 1 フレームに投げる刻みの数を決める(T-0004、docs/design/06-simulation-loop.md §4・D-202・D-409)。
//
// 目標は「現実の経過時間 × 60 刻み/秒 × ゲーム側の速さ」。GPU の時間(数フレーム前のタイムスタンプを待たずに読んだもの)で
// 1 フレームに使ってよい時間を超えないように減らし、追いつけない分は捨てる → 世界の時間が遅くなる。
// 刻みの中身は刻みの数の分け方に依存しない(06 §1)ので、ここの判断は世界の結果に入らない(View と Controller の仕事)。
// GPU を知らない純粋な計算なので、CPU だけのテスト(tests/tick_pacer_test.cpp)で確かめる。
#pragma once

#include <cstdint>

namespace bicameral::frame {

    struct TickPacerSettings {
        double ticksPerSecond = 60.0;          // 世界時間 1 秒 = 60 刻み(06 §1)
        uint32_t maxTicksPerBatch = 8;         // 1 回に投げられる刻みの上限(記録済みのリストの枠の数)
        double simBudgetFraction = 0.75;       // 1 フレームの時間のうち、シミュの GPU の時間に使ってよい割合
        double minFrameSeconds = 1.0 / 240.0;  // フレームの間隔の見積もりの範囲(極端な値で予算が壊れないように)
        double maxFrameSeconds = 1.0 / 30.0;   // 描画の目標 30fps 以上(D-201)
        double smoothing = 0.1;                // 計測の平均の重み(指数移動平均)
    };

    class TickPacer {
    public:
        explicit TickPacer(const TickPacerSettings& settings = {}) : m_settings(settings) {}

        // 現実の経過時間を足す。speedScale はゲーム側の指定の速さ(スローの演出。1 = 等速。D-409)
        void AddRealTime(double seconds, double speedScale = 1.0);

        // 終わったバッチの GPU の時間を知らせる(数フレーム遅れで届く)
        void ReportGpuTime(double gpuMilliseconds, uint32_t tickCount);

        // 今のフレームで投げる刻みの数(0 もある)。呼んだ分だけ未処理の刻みが減る
        [[nodiscard]] uint32_t TakeTicks();

        [[nodiscard]] double PendingTicks() const { return m_pendingTicks; }
        [[nodiscard]] uint64_t DroppedTicks() const { return m_droppedTicks; }  // 追いつけず捨てた刻み(世界が遅れた分)
        [[nodiscard]] double GpuMillisecondsPerTick() const { return m_gpuMillisecondsPerTick; }  // 0 = まだ計測なし
        [[nodiscard]] double FrameSeconds() const { return m_frameSeconds; }

    private:
        TickPacerSettings m_settings;
        double m_pendingTicks = 0.0;
        double m_frameSeconds = 0.0;            // フレームの間隔の平均
        double m_gpuMillisecondsPerTick = 0.0;  // 1 刻みあたりの GPU の時間の平均
        uint64_t m_droppedTicks = 0;
    };

}  // namespace bicameral::frame
