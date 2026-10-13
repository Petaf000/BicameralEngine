// play_metric_runner.h — エディタの「遊びの指標」を本物の刻みの合間に測る(T-0432・ADR-0206。D-482 ④・D-468)。
//
// フレームのループが毎フレーム、本物のシミュのリストを投げた後に Update を呼ぶ。指標を測る写し(sim::PlayMetricMeasurer。
// 元の世界 S(0) と写しの 2 つ。本物とは別)のリストは、本物と同じ compute キューの本物のリストの後ろへ投げる。
// 早送りの量はそのフレームのシミュの予算の余りに入る刻みの数だけ(本物を遅らせない。写しの早送りが走っている間は譲る)。
// 測るのは、パネルのボタンか、「表を変えたら測る」が入っていて世界の表の版が最後に測った版と違う時。測り終えたら写しを捨て
// (VRAM を返す)、前に測った版との差をパネルへ出す。作る(パイプラインの作成)のは別のスレッドで、フレームのループを止めない。
// 測っている間に表が替わったら、測りかけを捨てて新しい表で測り直す(新しい表の前の版は、その前に測り終えた版)。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editor/play_metric_panel.h"
#include "gpu/queue.h"
#include "sim/play_metric_measure.h"

namespace bicameral::frame {

    class PlayMetricRunner {
    public:
        // 測る物を作る関数(別のスレッドで呼ぶ。今の表と場面は作る時に写して持つ)
        using CreateMeasurer = std::function<std::expected<sim::PlayMetricMeasurer, std::string>()>;

        // 次の Update で今の表を測る(測っていれば何もしない)
        void Request() { m_requested = true; }

        // 本物のシミュのリストを投げた後に呼ぶ。tableVersion = 世界が今使っている表の版。autoMeasure = 表を変えたら測る。
        // yield = ほかの道具(写しの早送り)が今の余りを使う。makeCreate は作る時だけ呼ぶ
        void Update(gpu::Queue& compute, uint64_t tableVersion, bool autoMeasure, double spareMilliseconds, bool yield,
                    const std::function<CreateMeasurer()>& makeCreate);

        [[nodiscard]] const editor::PlayMetricProgress& Progress() const { return m_progress; }

    private:
        void Collect(gpu::Queue& compute);
        [[nodiscard]] bool Prepare(uint64_t tableVersion, const std::function<CreateMeasurer()>& makeCreate);
        void Advance(gpu::Queue& compute, double spareMilliseconds);
        void Finish();
        void RefreshProgress();

        std::optional<sim::PlayMetricMeasurer> m_measurer;
        std::future<std::expected<sim::PlayMetricMeasurer, std::string>> m_creating;
        uint64_t m_measuringVersion = 0;  // 作っている・測っている表の版
        uint64_t m_fence = 0;
        bool m_requested = false;
        bool m_measuredOnce = false;
        uint64_t m_measuredVersion = 0;  // 最後に測り終えた表の版
        std::vector<sim::PlayMetricValue> m_lastValues;
        editor::PlayMetricProgress m_progress;
    };

}  // namespace bicameral::frame
