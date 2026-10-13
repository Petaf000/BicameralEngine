// play_metric_measure.cpp — 遊びの指標を測る(play_metric_measure.h。T-0432・ADR-0206)。
#include "sim/play_metric_measure.h"

#include <algorithm>
#include <format>
#include <utility>

#include "core/aliases.h"
#include "core/log.h"

namespace bicameral::sim {

    namespace {

        std::vector<ProbeCommand> MakePokeCommands(const PlayMetric& metric) {
            std::vector<ProbeCommand> commands;
            uint32_t sequence = 0;
            for (const PlayMetricPoke& poke : metric.pokes)
                commands.push_back(MakePokeCommand(poke.tick, sequence++, poke.x, poke.y, poke.z));

            return commands;
        }

        PlayMetricValue FailedValue(const PlayMetric& metric, std::string message) {
            return {.key = metric.key, .label = metric.label, .message = std::move(message)};
        }

    }  // namespace

    // --- 量と判定 ---

    uint64_t MeasurePlayMetricQuantity(const PlayMetric& metric, std::span<const reaction::RxCell> cells,
                                       std::span<const reaction::HcThermalCache> thermal) {
        switch (metric.quantity) {
            case PlayMetricQuantity::MaxTemperature: return ProbeMaxTemperature(thermal);
            case PlayMetricQuantity::CellsAtOrAbove: return ProbeCellsAtOrAbove(thermal, metric.temperatureMillikelvin);
            case PlayMetricQuantity::SpeciesAmount: return ProbeSpeciesAmount(cells, metric.species);
        }

        return 0;
    }

    bool PlayMetricObserver::Observe(uint64_t tick, std::span<const reaction::RxCell> cells,
                                     std::span<const reaction::HcThermalCache> thermal) {
        const uint64_t quantity = MeasurePlayMetricQuantity(m_metric, cells, thermal);
        m_samples.emplace_back(tick, quantity);

        // つつきを全部当てた後だけ判定する(火をつける前の「火が無い」で止まらない。play_metric.h)
        return tick > m_metric.JudgeAfterTick() && m_metric.Stops(quantity);
    }

    PlayMetricValue PlayMetricObserver::Value(bool reached, uint64_t ticks) const {
        PlayMetricValue value{.key = m_metric.key,
                              .label = m_metric.label,
                              .measured = true,
                              .reached = reached,
                              .ticks = ticks,
                              .samples = m_samples};
        const auto found = rng::find(m_samples, ticks, &std::pair<uint64_t, uint64_t>::first);
        if (found == m_samples.end())
            return FailedValue(m_metric, std::format("止めた刻み {} の量を読み戻していない", ticks));

        value.quantity = found->second;

        return value;
    }

    WorldCopyQuery MakePlayMetricQuery(const PlayMetric& metric, std::shared_ptr<PlayMetricObserver> observer) {
        return {.maxTicks = metric.maxTicks,
                .sampleIntervalTicks = metric.sampleIntervalTicks,
                .commands = MakePokeCommands(metric),
                .until = [observer = std::move(observer)](const WorldCopySample& sample) {
                    if (sample.cells.empty())
                        return false;

                    return observer->Observe(sample.tick, sample.cells, sample.thermal);
                }};
    }

    // --- CPU リファレンス ---

    PlayMetricValue MeasurePlayMetricReference(const BakedReactionTable& table, const PlayMetric& metric,
                                               std::vector<uint64_t>* cellHashes) {
        const std::vector<ProbeCommand> commands = MakePokeCommands(metric);
        ProbeReference reference(table);
        PlayMetricObserver observer(metric);
        size_t nextCommand = 0;

        for (uint64_t tick = 0;; ++tick) {
            if (tick % metric.sampleIntervalTicks == 0 &&
                observer.Observe(tick, reference.State(tick), reference.Caches(tick))) {
                return observer.Value(true, tick);
            }

            if (tick == metric.maxTicks)
                return observer.Value(false, tick);

            const size_t first = nextCommand;
            while (nextCommand < commands.size() && commands[nextCommand].targetTick == tick)
                ++nextCommand;

            reference.Advance(tick, std::span(commands).subspan(first, nextCommand - first));
            if (cellHashes != nullptr)
                cellHashes->push_back(reference.StateHash(tick + 1));
        }
    }

    // --- GPU ---

    std::expected<PlayMetricMeasurer, std::string> PlayMetricMeasurer::Create(ID3D12Device5* device,
                                                                              D3D12_COMMAND_LIST_TYPE listType,
                                                                              const BakedReactionTable& table,
                                                                              const ProbeSimOptions& options) {
        const ProbeSimOptions shape{.physicsScene = options.physicsScene,
                                    .physicsOptions = options.physicsOptions,
                                    .physicsParameters = options.physicsParameters};
        auto source = ProbeSim::Create(device, listType, table, shape);  // 初めの世界は表の世界の組み立て
        if (!source)
            return std::unexpected(std::format("指標の元の世界を作れない: {}", source.error()));

        auto copy = WorldCopy::Create(device, listType, source->Table(), shape);
        if (!copy)
            return std::unexpected(copy.error());

        PlayMetricMeasurer measurer;
        measurer.m_source = std::make_unique<ProbeSim>(std::move(*source));
        measurer.m_copy = std::make_unique<WorldCopy>(std::move(*copy));
        measurer.m_metrics = table.playMetrics;

        return measurer;
    }

    std::vector<ID3D12CommandList*> PlayMetricMeasurer::RecordNext(uint64_t maxTicks) {
        if (m_pending || Finished() || maxTicks == 0)
            return {};

        std::vector<ID3D12CommandList*> lists;
        if (m_stage == Stage::Initialize) {
            ID3D12CommandList* source = m_source->RecordFrame(0, {.firstTick = 0, .unitCount = 0});
            lists = m_copy->RecordInitialization();
            if (source == nullptr || lists.empty()) {
                Log(Channel::Sim, Level::Error, "遊びの指標: 元の世界か写しの初期化を記録できない");
                return {};
            }

            lists.insert(lists.begin(), source);
        } else if (m_stage == Stage::Start) {
            const PlayMetric& metric = m_metrics[m_values.size()];
            m_observer = std::make_shared<PlayMetricObserver>(metric);
            lists = m_copy->RecordStart(*m_source, 0, MakePlayMetricQuery(metric, m_observer));
            if (lists.empty()) {
                m_values.push_back(FailedValue(metric, "写しを取れない(理由はログ)"));
                m_results.push_back(m_copy->Result());
                return {};
            }

            m_stage = Stage::Run;
        } else
            lists = m_copy->RecordNext(maxTicks);

        m_pending = !lists.empty();

        return lists;
    }

    void PlayMetricMeasurer::Complete() {
        if (!m_pending)
            return;

        m_pending = false;
        if (m_stage == Stage::Initialize) {
            static_cast<void>(m_source->ReadFrame(0));
            m_copy->Complete();
            m_stage = Stage::Start;
            return;
        }

        m_copy->Complete();
        if (m_copy->Finished())
            FinishMetric();
    }

    void PlayMetricMeasurer::FinishMetric() {
        const WorldCopyResult& result = m_copy->Result();
        const PlayMetric& metric = m_metrics[m_values.size()];
        m_values.push_back(result.failed ? FailedValue(metric, "早送りに失敗した(理由はログ)")
                                         : m_observer->Value(result.reached, result.endTick - result.startTick));
        m_results.push_back(result);
        m_observer.reset();
        m_stage = Stage::Start;
    }

}  // namespace bicameral::sim
