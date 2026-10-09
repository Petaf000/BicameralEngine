// sim_scheduler.h — 1 フレームに投げるシミュの「単位」の数を決める(T-0012、ADR-0011、docs/design/06-simulation-loop.md §4・§4.2)。
//
// 1 刻みは決まった数の単位(Dispatch のまとまり。sim/probe_sim の単位の列)でできている。CPU は毎フレーム、
// 「そのフレームの予算に入る数の単位」だけを compute キューに投げ、その後ろに描画を投げる(描画は compute に先に積まれた仕事を待つため。R-LOOP-2)。
// 刻みはフレームをまたいでよい。ここはその数を決めるだけの純粋な計算(GPU を知らない)で、CPU だけのテスト(tests/sim_scheduler_test.cpp)で確かめる。
//
//   予算(ms)= 1000 / 目標 fps − 描画の GPU 時間 − 余裕(下限あり)。目標 fps は設定で既定 60、30 より下げない(ADR-0011・D-201)
//   単位の見積もり = その単位の GPU 時間の平均(数フレーム遅れで届くタイムスタンプ)。まだ測っていない単位は予算いっぱいと見なす
//   刻みを始めてよいのは、現実の時間 × 60 刻み/秒 × ゲーム側の速さ(D-409)で「進めてよい刻み」が 1 以上あるときだけ
//   追いつけない分(未処理が上限を超えた分)は捨てる → 世界の時間が遅くなる(D-202)
// どう分けて投げても世界の結果は変わらない(06 §1)ので、ここの判断は世界の結果に入らない(View と Controller の仕事。D-107)。
#pragma once

#include <cstdint>
#include <vector>

namespace bicameral::frame {

    struct SimSchedulerSettings {
        // --- 速さ ---
        double ticksPerSecond = 60.0;  // 世界時間 1 秒 = 60 刻み(06 §1)
        double targetFps = 60.0;       // 重いときに描画が保つ fps(ADR-0011。30 未満は呼ぶ側が拒否する)

        // --- 1 フレームの予算 ---
        double marginMilliseconds = 1.0;     // 予算から引く余裕(見積もりの外れ・フレームの切れ目の隙間)
        double minBudgetMilliseconds = 0.5;  // 描画が重すぎても、これだけはシミュに回す
        double smoothing = 0.1;              // 計測の平均の重み(指数移動平均)

        // --- 上限 ---
        double maxBacklogTicks = 8.0;     // 未処理の刻みの上限(約 133 ms 分)。超えた分は捨てる(世界が遅くなる)
        uint32_t maxUnitsPerFrame = 256;  // 1 フレームのリストに積める単位の上限(タイムスタンプの数)
    };

    // 次に投げる単位の場所
    struct SimCursor {
        uint64_t tick = 0;  // 刻みの番号
        uint32_t unit = 0;  // 刻みの中の単位の番号(0 = 刻みの始め)

        friend bool operator==(const SimCursor&, const SimCursor&) = default;
    };

    class SimScheduler {
    public:
        SimScheduler(uint32_t unitsPerTick, const SimSchedulerSettings& settings = {});

        // 現実の経過時間を足す。speedScale はゲーム側の指定の速さ(スローの演出。1 = 等速。D-409)
        void AddRealTime(double seconds, double speedScale = 1.0);

        // 時間の操作(エディタ。T-0023): 未処理の刻みを捨てる(止める)・決まった数の刻みを足す(1 刻み進める)。
        // 刻みの途中で止めても、始めた刻みは最後の単位まで進む(TakeUnits は刻みの始めでしか止まらない)。
        // どちらも「いつ投げるか」だけを変え、世界の結果は変えない(06 §1)
        void ClearBacklog() { m_pendingTicks = 0.0; }
        void AddTicks(uint32_t ticks) { m_pendingTicks += static_cast<double>(ticks); }

        // この刻みの始めで止まる(これより先の刻みを始めない。--screenshot-tick。T-0025)。世界の結果は変えない
        void SetStopTick(uint64_t tick) { m_stopTick = tick; }
        [[nodiscard]] bool ReachedStopTick() const { return m_cursor.tick >= m_stopTick && m_cursor.unit == 0; }

        // 巻き戻し(T-0143): 刻みの番号が ticks の倍数の境界で、そのフレームの単位を切る(次のフレームがその境界から始まり、
        // 先頭で保存点へ写せる)。0 なら切らない。世界の結果は変えない
        void SetBreakInterval(uint64_t ticks) { m_breakInterval = ticks; }
        // カーソルを刻み tick の境界へ戻す(保存点へ戻した後。未処理の刻みはそのまま)
        void Rewind(uint64_t tick) { m_cursor = {.tick = tick, .unit = 0}; }

        // 終わった単位の GPU の時間(数フレーム遅れで届く)。unit は刻みの中の番号
        void ReportUnitTime(uint32_t unit, double gpuMilliseconds);

        // 終わった描画の GPU の時間
        void ReportRenderTime(double gpuMilliseconds);

        // 今のフレームで投げる単位の数(0 もある)。カーソルと未処理の刻みを進める。
        // 刻みの途中なら、見積もりが予算を超えても 1 つは進める(世界を止めない。遅くなるだけ)
        [[nodiscard]] uint32_t TakeUnits();

        [[nodiscard]] SimCursor Cursor() const { return m_cursor; }
        [[nodiscard]] uint32_t UnitsPerTick() const { return m_unitsPerTick; }

        // --- 見積もり(ログと確認用)---
        [[nodiscard]] double BudgetMilliseconds() const;
        [[nodiscard]] double UnitMilliseconds(uint32_t unit) const { return m_unitMilliseconds[unit]; }  // 0 = 未計測
        [[nodiscard]] double RenderMilliseconds() const { return m_renderMilliseconds; }
        [[nodiscard]] double PendingTicks() const { return m_pendingTicks; }
        [[nodiscard]] uint64_t DroppedTicks() const { return m_droppedTicks; }  // 追いつけず捨てた刻み(世界が遅れた分)

    private:
        [[nodiscard]] double EstimateMilliseconds(uint32_t unit, double budgetMilliseconds) const;

        SimSchedulerSettings m_settings;
        uint32_t m_unitsPerTick = 1;

        // --- 進み ---
        SimCursor m_cursor;
        uint64_t m_stopTick = UINT64_MAX;  // この刻みより先を始めない(既定: 止まらない)
        uint64_t m_breakInterval = 0;      // この倍数の刻みの境界でフレームを切る(0 = 切らない)
        double m_pendingTicks = 0.0;
        uint64_t m_droppedTicks = 0;

        // --- 計測の平均 ---
        double m_renderMilliseconds = 0.0;
        std::vector<double> m_unitMilliseconds;  // 刻みの中の単位ごとの GPU 時間の平均
    };

}  // namespace bicameral::frame
