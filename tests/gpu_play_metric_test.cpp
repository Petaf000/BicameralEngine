// gpu_play_metric_test.cpp — 遊びの指標を写しの早送りで測る(sim/play_metric_measure。T-0432・ADR-0206。D-482 ④)。
//
// サンプルの世界のパッケージ(data/packages/combustion_test)の指標「密室の火が消えるまで」(sealed_fire_out)と、同じ場面を
// CPU リファレンスで追える短さに切った指標(fire_prefix: 600 K 以上のセルの数を上限まで測る)を測る。確かめること:
//   1. 本物の世界を流しながら(同じキュー・本物のフレームの後ろ)測っても、本物のハッシュ列が本物だけで流した基準と全部一致(D-403)
//   2. 同じ表をもう一度測ると、同じ値(刻み・量・読み戻した量の道のり)になる(決定性)
//   3. 固体の熱の伝導率を 10 倍にした表では fire_peak(同じ場面の 1 秒後の最高温度)の値が変わり、前の版との差が出る(ComparePlayMetrics)。
//      (前は「木の燃焼の速さを 10 倍にした表で sealed_fire_out が変わる」だった。現実の伝導率〔T-1066〕では殻 1 セルの木箱の火は
//       広がらず、火をつけたセルは熱分解して上限の 600 秒まで 1000 K を超えたままなので、燃焼の速さでは値が変わらない。T-1163。
//       燃え広がりの場面は T-1156)
//   4. fire_prefix の値・量の道のり・刻みごとのセルのハッシュが CPU リファレンス(MeasurePlayMetricReference)とビット一致。
//      sealed_fire_out も CPU_FULL_LIMIT 刻み以内に止まれば CPU で測って同じ値か確かめる(長ければ飛ばしてログに書く)
//   5. シェーダーの assert と debug layer のエラーが 0 件
// 測る世界は物理なし(セルの結果は物と独立。指標の量はセルだけから測る)。1 刻みの GPU の時間をログに出す(docs/perf.md)。
// 引数: gpu_test_options.h(--warp・--no-gbv)
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/device.h"
#include "gpu/queue.h"
#include "gpu_test_options.h"
#include "reaction_test_table.h"
#include "sim/play_metric_measure.h"
#include "sim/probe_sim.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    constexpr uint64_t REAL_TICKS = 90;    // 本物の世界を流す刻み(測るのと並べる)
    constexpr uint64_t PREFIX_TICKS = 60;  // CPU リファレンスで追う指標の上限(debug で 1 刻み約 1 秒)
    constexpr uint32_t PREFIX_INTERVAL = 10;
    constexpr uint64_t CPU_FULL_LIMIT = 600;       // sealed_fire_out をこの刻み以内なら CPU でも測る
    constexpr uint64_t COPY_TICKS_PER_FRAME = 64;  // 本物の 1 フレームの後ろで進める刻み
    constexpr uint64_t ALONE_TICKS = 2000;         // 本物が止まった後に 1 回で進める刻み
    constexpr std::array<uint32_t, 4> FRAME_UNITS = {5, 9, 3, 7};
    constexpr std::string_view FIRE_KEY = "sealed_fire_out";
    constexpr std::string_view PREFIX_KEY = "fire_prefix";
    constexpr std::string_view PEAK_KEY = "fire_peak";
    constexpr size_t METRIC_COUNT = 3;  // sealed_fire_out・fire_prefix・fire_peak

    struct Failures {
        int count = 0;

        void Check(bool condition, std::string_view what) {
            if (condition)
                return;

            Log(Channel::Sim, Level::Error, "失敗: {}", what);
            ++count;
        }
    };

    // 本物の世界に当てるコマンド(木箱の別の壁に火をつける。指標の場面とは別の世界)
    std::vector<ProbeCommand> RealCommands() {
        return {MakePokeCommand(0, 0, 35, 32, 32)};
    }

    // 試験の表に、CPU で追える短い指標を足す(sealed_fire_out と同じ場面。止めずに上限まで):
    // fire_prefix = 同じ量(1000 K 以上のセルの数)・fire_peak = 最高温度(表の物の値を変えると必ず変わる量)
    BakedReactionTable WithPrefixMetric(BakedReactionTable table) {
        const auto fire = rng::find(table.playMetrics, FIRE_KEY, &PlayMetric::key);
        if (fire == table.playMetrics.end())
            return table;

        PlayMetric prefix = *fire;
        prefix.key = PREFIX_KEY;
        prefix.label = "火の始まり(試験)";
        prefix.maxTicks = PREFIX_TICKS;
        prefix.sampleIntervalTicks = PREFIX_INTERVAL;
        prefix.stop = PlayMetricStop::AtLimit;
        table.playMetrics.push_back(prefix);

        PlayMetric peak = prefix;
        peak.key = PEAK_KEY;
        peak.label = "火の 1 秒後の最高温度(試験)";
        peak.quantity = PlayMetricQuantity::MaxTemperature;
        table.playMetrics.push_back(peak);

        return table;
    }

    // 固体の熱の伝導率を 10 倍にした表(火をつけたセルの冷め方が変わるので、前の版との差が出るはず)
    std::optional<BakedReactionTable> MakeChangedTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        uint32_t solids = 0;
        for (SpeciesDefinition& species : definition.species) {
            if (species.phase != SpeciesPhase::Solid)
                continue;

            species.thermalConductivity *= 10;
            ++solids;
        }

        if (solids == 0)
            return std::nullopt;

        auto baked = BakeReactionTable(definition);
        if (!baked)
            return std::nullopt;

        return WithPrefixMetric(std::move(*baked));
    }

    const PlayMetricValue* Find(const std::vector<PlayMetricValue>& values, std::string_view key) {
        const auto found = rng::find(values, key, &PlayMetricValue::key);
        return found == values.end() ? nullptr : &*found;
    }

    // 測った結果(値と、指標ごとのセルのハッシュ列)
    struct Measured {
        std::vector<PlayMetricValue> values;
        std::map<std::string, std::vector<uint64_t>, std::less<>> cellHashes;
        uint64_t gpuTimestampTicks = 0;
        uint64_t ticksRun = 0;
        uint32_t debugAssertCount = 0;
        bool ok = false;
    };

    Measured Collect(const PlayMetricMeasurer& measurer) {
        Measured measured{.values = measurer.Values(), .ok = measurer.Finished()};
        for (size_t index = 0; index < measurer.Results().size(); ++index) {
            const WorldCopyResult& result = measurer.Results()[index];
            std::vector<uint64_t>& hashes = measured.cellHashes[measurer.Metrics()[index].key];
            for (const ProbeTickHash& hash : result.hashes)
                hashes.push_back(hash.hash);

            measured.gpuTimestampTicks += result.gpuTimestampTicks;
            measured.ticksRun += result.ticksRun;
            measured.debugAssertCount += result.debugAssertCount;
        }

        return measured;
    }

    // 測る物だけを続けて回す(本物の世界なし)
    bool RunAlone(gpu::Queue& queue, PlayMetricMeasurer& measurer) {
        for (uint32_t round = 0; round < 100000 && !measurer.Finished(); ++round) {
            const std::vector<ID3D12CommandList*> lists = measurer.RecordNext(ALONE_TICKS);
            if (!lists.empty() && !queue.WaitCpu(queue.Submit(lists)))
                return false;

            measurer.Complete();
        }

        return measurer.Finished();
    }

    std::optional<Measured> MeasureAlone(ID3D12Device5* device, gpu::Queue& queue, const BakedReactionTable& table) {
        auto measurer = PlayMetricMeasurer::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, {});
        if (!measurer) {
            Log(Channel::Sim, Level::Error, "{}", measurer.error());
            return std::nullopt;
        }

        if (!RunAlone(queue, *measurer))
            return std::nullopt;

        return Collect(*measurer);
    }

    // 本物の世界を流しながら(本物のフレームの後ろに指標のリスト)、本物が REAL_TICKS に着いたら残りを続けて測る。
    // 本物の刻みごとのハッシュを hashes に
    std::optional<Measured> MeasureBesideReal(ID3D12Device5* device, gpu::Queue& queue, const BakedReactionTable& table,
                                              ProbeSim* real, std::map<uint64_t, uint64_t>& hashes,
                                              uint32_t& realAsserts) {
        std::optional<PlayMetricMeasurer> measurer;
        if (table.playMetrics.empty() == false) {
            auto created = PlayMetricMeasurer::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, {});
            if (!created) {
                Log(Channel::Sim, Level::Error, "{}", created.error());
                return std::nullopt;
            }

            measurer.emplace(std::move(*created));
        }

        const std::vector<ProbeCommand> commands = RealCommands();
        const uint32_t unitsPerTick = real->UnitsPerTick();
        const uint64_t end = REAL_TICKS * unitsPerTick;
        uint64_t position = 0;
        for (uint64_t frame = 0; position < end; ++frame) {
            const uint64_t units = std::min<uint64_t>(FRAME_UNITS[frame % FRAME_UNITS.size()], end - position);
            const ProbeFrameInput input{
                .firstTick = position / unitsPerTick,
                .firstUnit = static_cast<uint32_t>(position % unitsPerTick),
                .unitCount = static_cast<uint32_t>(units),
                .commands = frame == 0 ? std::span<const ProbeCommand>(commands) : std::span<const ProbeCommand>()};
            ID3D12CommandList* list = real->RecordFrame(0, input);
            if (list == nullptr)
                return std::nullopt;

            queue.Execute(list);
            position += units;

            std::vector<ID3D12CommandList*> lists;
            if (measurer)
                lists = measurer->RecordNext(COPY_TICKS_PER_FRAME);

            if (!queue.WaitCpu(queue.Submit(lists)))
                return std::nullopt;

            if (measurer)
                measurer->Complete();

            const ProbeFrameReadback readback = real->ReadFrame(0);
            for (const ProbeTickHash& hash : readback.hashes)
                hashes[hash.tick] = hash.WorldHash();

            realAsserts += readback.debugAssertCount;
        }

        if (!measurer)
            return Measured{.ok = true};

        if (!RunAlone(queue, *measurer))
            return std::nullopt;

        return Collect(*measurer);
    }

    std::optional<ProbeSim> CreateReal(ID3D12Device5* device, const BakedReactionTable& table) {
        auto simulation = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, table, {});
        if (!simulation) {
            Log(Channel::Sim, Level::Error, "{}", simulation.error());
            return std::nullopt;
        }

        return std::move(*simulation);
    }

    void LogValue(std::string_view label, const PlayMetricValue* value) {
        if (value == nullptr) {
            Log(Channel::Sim, Level::Error, "{}: 値が無い", label);
            return;
        }

        Log(Channel::Sim, Level::Info, "{} {}: 測れた {}・{} 刻み({})・量 {}・読み戻し {} 回", label, value->key,
            value->measured, value->ticks, value->reached ? "着いた" : "上限", value->quantity, value->samples.size());
    }

    // CPU リファレンスと比べる(値・道のり・セルのハッシュ)
    void CheckReference(Failures& failures, const BakedReactionTable& table, const Measured& gpu,
                        std::string_view key) {
        const auto metric = rng::find(table.playMetrics, key, &PlayMetric::key);
        const PlayMetricValue* value = Find(gpu.values, key);
        if (metric == table.playMetrics.end() || value == nullptr) {
            failures.Check(false, "CPU と比べる指標がある");
            return;
        }

        std::vector<uint64_t> cpuHashes;
        const PlayMetricValue cpu = MeasurePlayMetricReference(table, *metric, &cpuHashes);
        LogValue("CPU", &cpu);
        failures.Check(cpu == *value, "指標の値と量の道のりが CPU リファレンスとビット一致");
        failures.Check(cpuHashes == gpu.cellHashes.at(std::string(key)),
                       "刻みごとのセルのハッシュが CPU リファレンスと一致");
    }

    int RunTest(ID3D12Device5* device, const BakedReactionTable& table, const BakedReactionTable& changedTable) {
        Failures failures;
        auto queue = gpu::Queue::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"PlayMetricTest");
        failures.Check(queue.has_value() && table.playMetrics.size() == METRIC_COUNT &&
                           changedTable.playMetrics.size() == METRIC_COUNT,
                       "キューと指標(sealed_fire_out・fire_prefix・fire_peak)がある");
        if (!queue || table.playMetrics.size() != METRIC_COUNT)
            return failures.count;

        // --- 基準: 本物だけ ---
        std::map<uint64_t, uint64_t> baseline;
        std::map<uint64_t, uint64_t> besideHashes;
        uint32_t realAsserts = 0;
        std::optional<ProbeSim> base = CreateReal(device, table);
        std::optional<ProbeSim> real = CreateReal(device, table);
        failures.Check(base && real, "本物の世界を作れた");
        if (!base || !real)
            return failures.count;

        BakedReactionTable none = table;
        none.playMetrics.clear();
        failures.Check(MeasureBesideReal(device, *queue, none, &*base, baseline, realAsserts).has_value(),
                       "基準を流せた");

        // --- 1. 本物と並べて測る ---
        const std::optional<Measured> first = MeasureBesideReal(device, *queue, table, &*real, besideHashes,
                                                                realAsserts);
        failures.Check(first && first->ok, "本物と並べて指標を測れた");
        if (!first)
            return failures.count;

        failures.Check(baseline.size() == REAL_TICKS && besideHashes == baseline,
                       "指標を測っても本物のハッシュ列が変わらない(D-403)");
        const PlayMetricValue* fire = Find(first->values, FIRE_KEY);
        LogValue("1 回目", fire);
        LogValue("1 回目", Find(first->values, PREFIX_KEY));
        LogValue("1 回目", Find(first->values, PEAK_KEY));
        failures.Check(fire != nullptr && fire->measured, "sealed_fire_out を測れた");

        // --- 2. 同じ表をもう一度 ---
        const std::optional<Measured> again = MeasureAlone(device, *queue, table);
        failures.Check(again && again->values == first->values && again->cellHashes == first->cellHashes,
                       "同じ表なら同じ値(刻み・量・道のり・セルのハッシュ)");

        // --- 3. 物の値を変えた表 ---
        const std::optional<Measured> changed = MeasureAlone(device, *queue, changedTable);
        failures.Check(changed && changed->ok, "伝導率を変えた表で測れた");
        if (changed) {
            LogValue("伝導率を変えた表", Find(changed->values, FIRE_KEY));
            LogValue("伝導率を変えた表", Find(changed->values, PEAK_KEY));
            const std::vector<PlayMetricDiff> diffs = ComparePlayMetrics(changed->values, first->values);
            Log(Channel::Sim, Level::Info, "{}", FormatPlayMetricReport(diffs, 60));
            const auto peakDiff = rng::find(diffs, PEAK_KEY, &PlayMetricDiff::key);
            failures.Check(peakDiff != diffs.end() && peakDiff->changed && peakDiff->previous && peakDiff->current,
                           "伝導率を変えた表では fire_peak の値が変わり、差が出る");
        }

        // --- 4. CPU リファレンス ---
        CheckReference(failures, table, *first, PREFIX_KEY);
        CheckReference(failures, table, *first, PEAK_KEY);
        if (fire != nullptr && fire->ticks <= CPU_FULL_LIMIT)
            CheckReference(failures, table, *first, FIRE_KEY);
        else if (fire != nullptr) {
            Log(Channel::Sim, Level::Info,
                "sealed_fire_out は {} 刻み(CPU で追う上限 {} より長いので CPU は fire_prefix だけ)", fire->ticks,
                CPU_FULL_LIMIT);
        }

        // --- 重さ・assert ---
        const double frequency = static_cast<double>(queue->TimestampFrequency());
        Log(Channel::Sim, Level::Info, "指標の早送り: {} 刻み・1 刻みの GPU 時間 {:.1f} µs", first->ticksRun,
            static_cast<double>(first->gpuTimestampTicks) * 1'000'000.0 / frequency /
                static_cast<double>(std::max<uint64_t>(1, first->ticksRun)));
        failures.Check(realAsserts == 0 && first->debugAssertCount == 0 && (!again || again->debugAssertCount == 0) &&
                           (!changed || changed->debugAssertCount == 0),
                       "シェーダーの assert が 0 件");

        return failures.count;
    }

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> arguments(argv, argv + argc);
    const std::optional<test::GpuTestOptions> options = test::ParseGpuTestOptions(std::span(arguments));
    const auto table = BakeReactionTable(MakeCombustionTestTable());
    const std::optional<BakedReactionTable> changedTable = MakeChangedTable();
    if (!options || !table || !changedTable) {
        Log(Channel::Sim, Level::Error, "使い方: gpu_play_metric_test [--warp] [--no-gbv](試験の表が作れない時も)");
        bicameral::SingletonFinalizer::Finalize();

        return 2;
    }

    auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
    if (!device) {
        Log(Channel::Gpu, Level::Error, "{}", device.error());
        bicameral::SingletonFinalizer::Finalize();

        return 1;
    }

    const int failures = RunTest(device->Get(), WithPrefixMetric(*table), *changedTable);
    const bool passed = failures == 0 && test::PassesValidation(*device, "gpu_play_metric_test");
    bicameral::SingletonFinalizer::Finalize();

    return passed ? 0 : 1;
}
