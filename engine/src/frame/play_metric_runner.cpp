// play_metric_runner.cpp — 遊びの指標を本物の刻みの合間に測る(T-0432・ADR-0206)。約束は play_metric_runner.h。
#include "frame/play_metric_runner.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

#include "core/log.h"

namespace bicameral::frame {

    namespace {

        constexpr uint64_t MAX_TICKS_PER_FRAME = 64;  // 1 フレームの早送りの上限(写しの早送りと同じ)
        constexpr uint64_t TICKS_PER_SECOND = 60;

    }  // namespace

    void PlayMetricRunner::Update(gpu::Queue& compute, uint64_t tableVersion, bool autoMeasure,
                                  double spareMilliseconds, bool yield,
                                  const std::function<CreateMeasurer()>& makeCreate) {
        Collect(compute);
        if (m_measurer && m_measurer->Pending())
            return;  // GPU にある指標のリストは 1 組だけ

        // 測っている間に表が替わったら、測りかけを捨てる(GPU にリストは無い)
        if (m_measurer && m_measuringVersion != tableVersion) {
            m_measurer.reset();
            m_requested = true;
        }

        const bool stale = !m_measuredOnce || m_measuredVersion != tableVersion;
        const bool wanted = m_requested || (autoMeasure && stale);
        if (!m_measurer && !m_creating.valid() && !wanted)
            return;

        if (!Prepare(tableVersion, makeCreate))
            return;

        m_requested = false;
        if (yield) {
            ++m_progress.waitedFrames;
            return;
        }

        Advance(compute, spareMilliseconds);
    }

    // 測る物を作る(別のスレッドで)。作り終えて今の表のものなら true
    bool PlayMetricRunner::Prepare(uint64_t tableVersion, const std::function<CreateMeasurer()>& makeCreate) {
        if (m_measurer)
            return true;

        if (!m_creating.valid()) {
            m_creating = std::async(std::launch::async, makeCreate());
            m_measuringVersion = tableVersion;
            m_progress.running = true;
            m_progress.waitedFrames = 0;
            m_progress.message = "指標の世界を作っている(パイプラインの作成)";
            return false;
        }

        if (m_creating.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;

        auto created = m_creating.get();
        if (!created) {
            Log(Channel::Sim, Level::Error, "遊びの指標: {}", created.error());
            m_progress.running = false;
            m_progress.message = "指標の世界を作れない: " + created.error();
            m_measuredOnce = true;  // 同じ表で作り直し続けない
            m_measuredVersion = m_measuringVersion;
            return false;
        }

        if (m_measuringVersion != tableVersion)
            return false;  // 作っている間に表が替わった(次のフレームで作り直す)

        m_measurer.emplace(std::move(*created));
        m_progress.message.clear();
        m_progress.vramBytes = m_measurer->VramBytes();

        return true;
    }

    // 余りの予算に入る刻みの数だけ進める(測るまでは 1 刻み。余りが無ければ進めない。D-468)
    void PlayMetricRunner::Advance(gpu::Queue& compute, double spareMilliseconds) {
        const sim::WorldCopyResult& result = m_measurer->CurrentResult();
        uint64_t ticks = spareMilliseconds > 0.0 ? 1 : 0;
        if (result.ticksRun > 0 && result.gpuTimestampTicks > 0) {
            const double tickMilliseconds = static_cast<double>(result.gpuTimestampTicks) * 1000.0 /
                                            static_cast<double>(compute.TimestampFrequency()) /
                                            static_cast<double>(result.ticksRun);
            ticks = static_cast<uint64_t>(spareMilliseconds / tickMilliseconds);
        }

        ticks = std::min(ticks, MAX_TICKS_PER_FRAME);
        if (ticks == 0) {
            ++m_progress.waitedFrames;
            return;
        }

        const std::vector<ID3D12CommandList*> lists = m_measurer->RecordNext(ticks);
        if (!lists.empty())
            m_fence = compute.Submit(lists);
        else if (m_measurer->Finished())
            Finish();
    }

    void PlayMetricRunner::Collect(gpu::Queue& compute) {
        if (!m_measurer || !m_measurer->Pending() || !compute.IsComplete(m_fence))
            return;

        m_measurer->Complete();
        if (m_measurer->Finished())
            Finish();
        else
            RefreshProgress();
    }

    void PlayMetricRunner::RefreshProgress() {
        const size_t index = std::min(m_measurer->Values().size(), m_measurer->Metrics().size() - 1);
        const sim::PlayMetric& metric = m_measurer->Metrics()[index];
        m_progress.metricIndex = index;
        m_progress.metricCount = m_measurer->Metrics().size();
        m_progress.metricLabel = metric.label;
        m_progress.maxTicks = metric.maxTicks;
        m_progress.tick = m_measurer->CurrentTick();
    }

    // 測り終えた: 前に測った版と比べ、写しを捨てる
    void PlayMetricRunner::Finish() {
        const std::vector<sim::PlayMetricValue> values = m_measurer->Values();
        m_measurer.reset();

        m_progress.running = false;
        m_progress.hasResult = true;
        m_progress.previousTableVersion = m_measuredOnce ? m_measuredVersion : 0;
        m_progress.tableVersion = m_measuringVersion;
        const std::vector<sim::PlayMetricValue> none;  // 初めて測った(前の版なし)
        m_progress.diffs = sim::ComparePlayMetrics(values, m_measuredOnce ? m_lastValues : none);

        m_lastValues = values;
        m_measuredOnce = true;
        m_measuredVersion = m_measuringVersion;
        Log(Channel::Sim, Level::Info, "{}(表の版 {:016x}・待ったフレーム {})",
            sim::FormatPlayMetricReport(m_progress.diffs, TICKS_PER_SECOND), m_measuringVersion,
            m_progress.waitedFrames);
    }

}  // namespace bicameral::frame
