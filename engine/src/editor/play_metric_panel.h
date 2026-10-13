// play_metric_panel.h — エディタの「遊びの指標」のパネル(T-0432・ADR-0206。D-482 ④・14 §2「調整の負担を減らす道具」)。
//
// パッケージの分類 play_metrics に登録した指標(密室の火が何秒で消えるか など)を、表を変えるたびに写しの早送りで測り、
// 前の版(この起動で前に測った表)との差を並べる。測るのは frame/play_metric_runner(本物の刻みの合間の余りの予算で。D-468)。
// データの流れ: パネルの「今の表で測る」・「表を変えたら測る」→ TakeRequest / AutoMeasure(フレームのループが取る)
//   → frame/play_metric_runner → 進みと差(PlayMetricProgress)を EditorStatus で受けて出す。
// 中身は持たない(D-454): 何を測るかはパッケージが決め、パネルは値と差を出すだけ。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sim/play_metric.h"

namespace bicameral::editor {

    // フレームのループが渡す進みと結果
    struct PlayMetricProgress {
        bool running = false;
        size_t metricIndex = 0;  // 測っている指標(running の時)
        size_t metricCount = 0;
        std::string metricLabel;
        uint64_t tick = 0;  // 測っている指標の進み(始めからの刻み)
        uint64_t maxTicks = 0;
        uint64_t tableVersion = 0;          // 測っている(測り終えた)表の版
        uint64_t previousTableVersion = 0;  // 前の版(0 = まだ無い)
        bool hasResult = false;
        std::vector<sim::PlayMetricDiff> diffs;  // 最後に測り終えた表と前の版の差
        uint64_t vramBytes = 0;
        uint64_t waitedFrames = 0;  // 余りの予算が無くて進めなかったフレーム(D-468)
        std::string message;
    };

    class PlayMetricPanel {
    public:
        // ImGui のフレームの中で呼ぶ
        void Build(const PlayMetricProgress& progress, uint64_t ticksPerSecond);

        [[nodiscard]] bool TakeRequest() { return std::exchange(m_request, false); }
        [[nodiscard]] bool AutoMeasure() const { return m_autoMeasure; }

    private:
        void BuildDiffs(const PlayMetricProgress& progress, uint64_t ticksPerSecond) const;

        bool m_request = false;
        bool m_autoMeasure = true;  // 表を変えたら測る(仮。T-0432 の判断待ち)
    };

}  // namespace bicameral::editor
