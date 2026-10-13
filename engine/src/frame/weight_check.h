// weight_check.h — 重さの検査: 1 刻みの重さの列が、予算と演出のスローの長さに収まるかを判定する(T-0413・ADR-0204。
// D-468・D-470・06 §6・14 §2「重さの検査と写しの早送り」)。
//
// 負荷で遅れることは一律にダメ(D-468)。収めるのはゲームの責任で、エンジンは測る道具と警告を出す。ここはその判定だけの純粋な計算
// (GPU を知らない。tests/weight_check_test で確かめる)。データの流れ:
//   写しの早送り(sim/world_copy の刻みごとの GPU の時間 + 活性のブロック数)→ CheckWeight → エディタのパネル(editor/copy_panel)
//   本物の刻み(sim/tick_cost で集めた重さ)→ WeightWatch → 開発ビルドとエディタの警告(frame/frame_loop)
// 1 刻みの予算 = 等速でその刻みに使ってよい GPU の時間(1 フレームのシミュの予算 × 目標 fps ÷ 60 刻み/秒)。
// 演出のスローの窓(ゲームが前もって長さを決めたもの)の刻みは、予算 ÷ 速さ(速さ 0.25 なら 4 倍)まで使ってよい。
// GPU の時間は機械と揺れで変わるので、決定的に比べる値は刻みの数と活性のブロック数(試験はそれで比べる)。
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bicameral::frame {

    // 演出のスロー(ゲームが前もって長さを決めた遅さ。負荷で伸び縮みさせない。D-468)
    struct WeightSlowWindow {
        uint64_t firstTick = 0;   // 窓の始めの刻み(WeightTick::tick と同じ数え方)
        uint64_t tickCount = 0;   // 窓の長さ(世界の刻みの数)
        double speedScale = 1.0;  // 世界の速さ(0 < 速さ ≤ 1。0.25 = 4 倍のスロー)
    };

    struct WeightBudget {
        double tickMilliseconds = 0.0;  // 等速で 1 刻みに使ってよい GPU の時間
        double cautionRatio = 0.8;      // 予算のこの割合を超えた刻みは「注意」(仮。ADR-0204)
        double backlogTicks = 8.0;      // 安全網が溜められる刻み(SimSchedulerSettings::maxBacklogTicks)
        std::vector<WeightSlowWindow> slows;

        // 刻み tick に使ってよい GPU の時間(スローの窓の中なら 予算 ÷ 速さ)
        [[nodiscard]] double AllowanceMilliseconds(uint64_t tick) const;
    };

    // 1 刻みの重さ
    struct WeightTick {
        uint64_t tick = 0;  // S(tick) を作った刻みの重さ(sim::TickCost と同じ数え方)
        double gpuMilliseconds = 0.0;
        uint32_t scheduledBlocks = 0;  // 伝導と反応を計算したブロックの数(活性の広さ)
    };

    // 続けて予算を超えた区間(どこが重いか)
    struct WeightSpan {
        uint64_t firstTick = 0;
        uint64_t lastTick = 0;
        uint64_t peakTick = 0;  // 区間でいちばん重い刻み
        double peakMilliseconds = 0.0;
        double peakRatio = 0.0;  // その刻みの重さ ÷ 使ってよい時間
        uint32_t peakBlocks = 0;
    };

    enum class WeightVerdict : uint8_t {
        Fits,     // すべての刻みが注意の割合より軽い
        Caution,  // 予算には収まるが、注意の割合を超えた刻みがある
        Over,     // 予算を超えた刻みがある(直すべき不具合。D-468)
    };

    struct WeightReport {
        uint64_t ticks = 0;         // 判定した刻みの数
        uint64_t overTicks = 0;     // 予算を超えた刻み
        uint64_t cautionTicks = 0;  // 注意の割合を超えた刻み(超えた刻みを含む)

        // --- 重さ ---
        double meanMilliseconds = 0.0;
        double peakMilliseconds = 0.0;  // いちばん重い刻み(使ってよい時間との比が最大の刻み)
        double peakRatio = 0.0;
        uint64_t peakTick = 0;
        uint32_t peakBlocks = 0;

        // --- 活性の広さ(決定的。試験で比べる値)---
        uint64_t blockSum = 0;
        uint32_t maxBlocks = 0;
        uint64_t maxBlocksTick = 0;

        // --- 安全網の見込み(D-202): 溜めを超えて捨て、世界の時間が遅れる刻みの数 ---
        double lagTicks = 0.0;

        std::vector<WeightSpan> spans;  // 続けて超えた区間(先頭から MAX_SPANS まで)
        uint64_t spanCount = 0;         // 区間の数(spans に入らなかった分を含む)

        WeightVerdict verdict = WeightVerdict::Fits;

        static constexpr size_t MAX_SPANS = 16;
    };

    [[nodiscard]] WeightReport CheckWeight(std::span<const WeightTick> ticks, const WeightBudget& budget);

    // 1 行の要約(ログと自動の確かめ。ticksPerSecond は刻みを秒に直す)
    [[nodiscard]] std::string DescribeWeight(const WeightReport& report, double ticksPerSecond);

    // 本物の刻みの見張り(安全網の警告。D-468): 予算を超えた刻みを溜め、区切りごとに 1 行の警告にする(ログを溢れさせない)
    class WeightWatch {
    public:
        // 刻みの重さと、その刻みに使ってよい時間
        void Note(const WeightTick& tick, double allowanceMilliseconds);

        // 前の区切りから予算を超えた刻みがあれば区間(最初〜最後の刻みと、いちばん重い刻み)を返して空にする
        [[nodiscard]] bool TakeSpan(WeightSpan& span, uint64_t& overTicks);

        [[nodiscard]] uint64_t MeasuredTicks() const { return m_measuredTicks; }
        [[nodiscard]] uint64_t OverTicks() const { return m_overTicks; }  // 累計
        [[nodiscard]] const WeightSpan& LastSpan() const { return m_lastSpan; }

    private:
        uint64_t m_measuredTicks = 0;
        uint64_t m_overTicks = 0;
        uint64_t m_pendingOver = 0;  // 区切りの中で超えた刻み
        WeightSpan m_pending;        // 区切りの中の区間
        WeightSpan m_lastSpan;       // 最後に返した区間
    };

}  // namespace bicameral::frame
