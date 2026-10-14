// play_metric_measure.h — 遊びの指標を測る(T-0432・ADR-0206。指標の形は sim/play_metric.h)。
//
// 指標の場面 = 表の世界の組み立ての初めの世界 S(0)。それを持つ「元の世界」(ProbeSim。刻み 0 のまま進めない)を 1 つ作り、
// 写しの早送り(sim::WorldCopy・ADR-0198)で指標ごとに S(0) を写して、つつきを当てて止め方の条件まで早送りする。
// 同じ写しを指標の数だけ使い直す(VRAM は世界 2 つ分)。本物の世界には触らない(D-403。元の世界は本物と別)。
// CPU リファレンス(MeasurePlayMetricReference)は ProbeReference で同じ場面を全部のセルで刻み、同じ判定(PlayMetricObserver)をする。
// GPU と CPU の値・読み戻した量の道のり・刻みごとのセルのハッシュがビット一致すること(tests/gpu_play_metric_test)。
//
// 流れ(GPU。WorldCopy と同じ約束: 返したリストを同じキューに順に 1 回で投げ、GPU が終えたら Complete()、Finished() まで繰り返す):
//   Create → RecordNext(この回に進める刻みの上限) → 投げる → Complete() → …
//   1 回目は元の世界と写しの初期化、続いて指標ごとに「写しを取る」「早送り」を必要な回数。
//   エディタは本物の刻みの合間に予算の余りの分だけ(frame/play_metric_runner)、コマンドラインと試験は続けて回す。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "sim/play_metric.h"
#include "sim/probe_sim.h"
#include "sim/world_copy.h"

namespace bicameral::sim {

    // 指標の量を 1 つの読み戻しから測る
    [[nodiscard]] uint64_t MeasurePlayMetricQuantity(const PlayMetric& metric, std::span<const reaction::RxCell> cells,
                                                     std::span<const reaction::HcThermalCache> thermal);

    // 読み戻した刻みごとに量を測り、止めるかを決める(GPU の写しの条件と CPU リファレンスが同じものを使う)
    class PlayMetricObserver {
    public:
        explicit PlayMetricObserver(const PlayMetric& metric) : m_metric(metric) {}

        // 始めの刻みからの刻み tick の読み戻し。止めるなら true
        bool Observe(uint64_t tick, std::span<const reaction::RxCell> cells,
                     std::span<const reaction::HcThermalCache> thermal);

        // 止めた刻み(始めからの刻み)と着いたかで値を作る。量は止めた刻みの読み戻しの量
        [[nodiscard]] PlayMetricValue Value(bool reached, uint64_t ticks) const;

    private:
        PlayMetric m_metric;
        std::vector<std::pair<uint64_t, uint64_t>> m_samples;
    };

    // 指標の写しの問い(つつきのコマンド・読み戻しの間隔・条件)。始めの刻みは 0。observer は条件が書く
    [[nodiscard]] WorldCopyQuery MakePlayMetricQuery(const PlayMetric& metric,
                                                     std::shared_ptr<PlayMetricObserver> observer);

    // CPU リファレンスの刻みごとの見張り(見るだけ。世界に何も返さない)。Advance(tick) の後に呼ぶ。
    // marksBefore = Advance の前のブロックの印(ChangedMarks の写し)。reference.State(tick)・Caches(tick)・State(tick + 1) が読める
    using PlayMetricReferenceWatch = std::function<void(uint64_t tick, std::span<const uint64_t> marksBefore,
                                                        const ProbeReference& reference)>;

    // CPU リファレンスで 1 つの指標を測る(全部のセルを毎刻み。遅い: debug で 1 刻み約 1 秒)。
    // cellHashes があれば S(1)〜S(止めた刻み) のセルのハッシュを足す(GPU の ProbeTickHash::hash と比べる)。
    // watch があれば刻みごとに呼ぶ(坂の計器 sim/slope_gauge。T-0518)
    [[nodiscard]] PlayMetricValue MeasurePlayMetricReference(const BakedReactionTable& table, const PlayMetric& metric,
                                                             std::vector<uint64_t>* cellHashes = nullptr,
                                                             const PlayMetricReferenceWatch& watch = {});

    class PlayMetricMeasurer {
    public:
        // table: 測る表(その表の世界の組み立てと指標)。options: 物理の場面と値(呼ぶ側が本物の世界と同じ形で渡す。
        // 元の世界と写しは同じ形)。initialWorld・重さの試験・トレースは使わない
        [[nodiscard]] static std::expected<PlayMetricMeasurer, std::string> Create(ID3D12Device5* device,
                                                                                   D3D12_COMMAND_LIST_TYPE listType,
                                                                                   const BakedReactionTable& table,
                                                                                   const ProbeSimOptions& options);

        // 次のリスト(maxTicks: この回に早送りする刻みの上限。1 以上)。走らせる物が無ければ空
        [[nodiscard]] std::vector<ID3D12CommandList*> RecordNext(uint64_t maxTicks);

        // 返したリストを GPU が終えた後に呼ぶ
        void Complete();

        [[nodiscard]] bool Pending() const { return m_pending; }
        [[nodiscard]] bool Finished() const { return m_values.size() == m_metrics.size() && !m_pending; }
        [[nodiscard]] const std::vector<PlayMetric>& Metrics() const { return m_metrics; }
        // 測り終えた指標の値(Metrics() の順)と、写しの結果(刻みごとの要約・GPU の時間)
        [[nodiscard]] const std::vector<PlayMetricValue>& Values() const { return m_values; }
        [[nodiscard]] const std::vector<WorldCopyResult>& Results() const { return m_results; }
        // 測っている指標の進み(始めからの刻み)
        [[nodiscard]] uint64_t CurrentTick() const { return m_copy->Tick(); }
        // 測っている指標の写しの結果(早送りした刻みと GPU の時間。予算の余りに入る刻みの数を見積もる)
        [[nodiscard]] const WorldCopyResult& CurrentResult() const { return m_copy->Result(); }
        [[nodiscard]] uint64_t VramBytes() const { return m_source->DefaultHeapBytes() + m_copy->VramBytes(); }

    private:
        PlayMetricMeasurer() = default;

        enum class Stage : uint8_t { Initialize, Start, Run };

        void FinishMetric();

        std::unique_ptr<ProbeSim> m_source;  // 初めの世界 S(0)(進めない)
        std::unique_ptr<WorldCopy> m_copy;
        std::vector<PlayMetric> m_metrics;
        std::vector<PlayMetricValue> m_values;
        std::vector<WorldCopyResult> m_results;
        std::shared_ptr<PlayMetricObserver> m_observer;
        Stage m_stage = Stage::Initialize;
        bool m_pending = false;
    };

}  // namespace bicameral::sim
