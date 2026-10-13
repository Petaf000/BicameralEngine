// play_metric_test.cpp — 遊びの指標の形(分類 play_metrics・ベイク・判定・前の版との差・値のファイル。T-0432・ADR-0206)を CPU だけで確かめる。
// 見るもの: サンプルの世界のパッケージ(data/packages/combustion_test)の指標「密室の火が消えるまで」が読めてベイクされる /
//           分類が無い世界は空 / 形の誤り(知らない欄・量と欄の組み合わせ・at_most と at_least の両方・列でないつつき)を
//           鍵と書いたパッケージの名前付きで落とす / ベイクの誤り(知らない物質・上限が間隔の倍数でない・格子の外・上限より後のつつき)/
//           判定はつつきを全部当てた後の刻みだけ / 前の版との差(同じ・変わった・初めて・消えた)/ 値のファイルの往復と壊れたファイル。
// GPU と CPU リファレンスで測った値が同じことは tests/gpu_play_metric_test。失敗すると条件と行を表示して 1 を返す。
#include <cstdio>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "reaction_test_table.h"
#include "script/luau_sandbox.h"
#include "script/play_metrics_package.h"
#include "sim/play_metric.h"
#include "sim/play_metric_measure.h"
#include "sim/reaction_table.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::script;
    using namespace bicameral::sim;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    bool Contains(std::string_view text, std::string_view part) {
        if (text.find(part) != std::string_view::npos)
            return true;

        std::printf("  '%.*s' に '%.*s' が無い\n", static_cast<int>(text.size()), text.data(),
                    static_cast<int>(part.size()), part.data());

        return false;
    }

    std::expected<std::vector<PlayMetricDefinition>, std::string> ReadInit(std::string init) {
        PackageSource package{.name = "world", .files = {}};
        package.files.emplace("package.luau", "return { format = 1 }");
        package.files.emplace("init.luau", std::move(init));

        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        if (!sandbox)
            return std::unexpected(sandbox.error());

        (*sandbox)->Seal();
        const std::vector<PackageSource> packages{std::move(package)};
        const auto loaded = LoadPackages(**sandbox, packages, PackageLoadOptions{});
        if (!loaded)
            return std::unexpected(loaded.error());

        if (!loaded->rejected.empty())
            return std::unexpected(loaded->rejected.front().reason);

        return ReadPlayMetrics(*loaded);
    }

    // 1 つの指標の欄だけを書いた init
    std::string MetricInit(std::string_view fields) {
        return std::format("return {{ play_metrics = {{ m = {{ {} }} }} }}", fields);
    }

    void ExpectReadError(std::string_view fields, std::string_view part) {
        const auto read = ReadInit(MetricInit(fields));
        EXPECT(!read.has_value());
        if (!read)
            EXPECT(Contains(read.error(), part));
    }

    // --- サンプルの世界 ---

    void TestSamplePackage() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        EXPECT(table.has_value());
        if (!table)
            return;

        EXPECT(table->playMetrics.size() == 1);
        if (table->playMetrics.empty())
            return;

        const PlayMetric& fire = table->playMetrics.front();
        EXPECT(fire.key == "sealed_fire_out");
        EXPECT(fire.label == "密室の火が消えるまで");
        EXPECT(fire.quantity == PlayMetricQuantity::CellsAtOrAbove);
        EXPECT(fire.temperatureMillikelvin == 1000000);
        EXPECT(fire.stop == PlayMetricStop::AtMost && fire.threshold == 0);
        EXPECT(fire.maxTicks % fire.sampleIntervalTicks == 0);
        const std::vector<PlayMetricPoke> pokes = {{.tick = 0, .x = 28, .y = 32, .z = 32}};
        EXPECT(fire.pokes == pokes);
    }

    // --- 読む ---

    void TestRead() {
        const auto none = ReadInit("return {}");
        EXPECT(none && none->empty());

        const auto read = ReadInit(
            MetricInit("label = 'O2', max_ticks = 120, sample_interval_ticks = 10, quantity = 'species_amount', "
                       "species = 'oxygen', at_least = 5, pokes = { { tick = 3, cell = { 1, 2, 3 } }, { tick = 0, cell "
                       "= { 4, 5, 6 } } }"));
        EXPECT(read && read->size() == 1);
        if (read && read->size() == 1) {
            const PlayMetricDefinition& metric = read->front();
            EXPECT(metric.key == "m" && metric.label == "O2" && metric.species == "oxygen");
            EXPECT(metric.stop == PlayMetricStop::AtLeast && metric.threshold == 5);
            EXPECT(metric.pokes.size() == 2 && metric.pokes[0].tick == 3 && metric.pokes[1].z == 6);
        }

        const std::string base = "max_ticks = 120, sample_interval_ticks = 10, ";
        ExpectReadError(base + "quantity = 'max_temperature', colour = 1", "colour");
        ExpectReadError(base + "quantity = 'heat'", "知らない量 heat");
        ExpectReadError(base + "quantity = 'max_temperature', temperature_mk = 1", "cells_at_or_above のときだけ");
        ExpectReadError(base + "quantity = 'cells_at_or_above'", "temperature_mk");
        ExpectReadError(base + "quantity = 'species_amount'", "species");
        ExpectReadError(base + "quantity = 'max_temperature', at_most = 1, at_least = 2", "どちらか 1 つ");
        ExpectReadError(base + "quantity = 'max_temperature', at_most = 1.5", "at_most");
        ExpectReadError(base + "quantity = 'max_temperature', pokes = { a = { tick = 0, cell = { 1, 2, 3 } } }", "列");
        ExpectReadError(base + "quantity = 'max_temperature', pokes = { { tick = 0, cell = { 1, 2 } } }", "x, y, z");
        ExpectReadError("sample_interval_ticks = 10, quantity = 'max_temperature'", "max_ticks");

        const auto named = ReadInit(
            MetricInit("max_ticks = 0, sample_interval_ticks = 1, quantity = 'max_temperature'"));
        EXPECT(!named && Contains(named.error(), "パッケージ world: play_metrics.m"));
    }

    // --- ベイク ---

    void ExpectBakeError(const BakedReactionTable& table, PlayMetricDefinition metric, std::string_view part) {
        const auto baked = BakePlayMetrics({std::move(metric)}, table);
        EXPECT(!baked.has_value());
        if (!baked)
            EXPECT(Contains(baked.error(), part));
    }

    void TestBake() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table)
            return;

        const PlayMetricDefinition base{.key = "m", .maxTicks = 120, .sampleIntervalTicks = 10};
        PlayMetricDefinition species = base;
        species.quantity = PlayMetricQuantity::SpeciesAmount;
        species.species = "oxygen";
        const auto baked = BakePlayMetrics({species}, *table);
        EXPECT(baked && baked->front().species == table->SpeciesId("oxygen") && baked->front().label == "m");

        species.species = "phlogiston";
        ExpectBakeError(*table, species, "知らない物質 phlogiston");

        PlayMetricDefinition uneven = base;
        uneven.maxTicks = 125;
        ExpectBakeError(*table, uneven, "倍数");

        PlayMetricDefinition outside = base;
        outside.pokes = {{.tick = 0, .x = 64, .y = 0, .z = 0}};
        ExpectBakeError(*table, outside, "格子");

        PlayMetricDefinition late = base;
        late.pokes = {{.tick = 120, .x = 1, .y = 1, .z = 1}};
        ExpectBakeError(*table, late, "上限");

        // 鍵の順に並ぶ・つつきは刻みの順
        PlayMetricDefinition second = base;
        second.key = "a";
        second.pokes = {{.tick = 7, .x = 1, .y = 1, .z = 1}, {.tick = 2, .x = 2, .y = 2, .z = 2}};
        const auto ordered = BakePlayMetrics({base, second}, *table);
        EXPECT(ordered && ordered->size() == 2 && ordered->front().key == "a" && ordered->front().pokes[0].tick == 2 &&
               ordered->front().JudgeAfterTick() == 7);
    }

    // --- 判定(つつきを全部当てた後だけ)---

    void TestObserver() {
        const PlayMetric metric{.key = "fire",
                                .label = "fire",
                                .maxTicks = 40,
                                .sampleIntervalTicks = 10,
                                .pokes = {{.tick = 10, .x = 0, .y = 0, .z = 0}},
                                .quantity = PlayMetricQuantity::CellsAtOrAbove,
                                .temperatureMillikelvin = 600000,
                                .stop = PlayMetricStop::AtMost,
                                .threshold = 0};
        std::vector<reaction::HcThermalCache> thermal(4);
        PlayMetricObserver observer(metric);
        EXPECT(!observer.Observe(0, {}, thermal));   // 火をつける前(0 セル)は判定しない
        EXPECT(!observer.Observe(10, {}, thermal));  // つつきの刻みもまだ
        thermal[1].temperature = 900000;
        EXPECT(!observer.Observe(20, {}, thermal));
        thermal[1].temperature = 500000;
        EXPECT(observer.Observe(30, {}, thermal));

        const PlayMetricValue value = observer.Value(true, 30);
        EXPECT(value.measured && value.reached && value.ticks == 30 && value.quantity == 0 &&
               value.samples.size() == 4);
        const std::pair<uint64_t, uint64_t> third = {20, 1};
        EXPECT(value.samples[2] == third);
        EXPECT(!observer.Value(false, 35).measured);  // 読み戻していない刻み
    }

    // --- 前の版との差・値のファイル ---

    PlayMetricValue Value(std::string key, bool reached, uint64_t ticks, uint64_t quantity) {
        return {.key = key, .label = key, .measured = true, .reached = reached, .ticks = ticks, .quantity = quantity};
    }

    void TestCompare() {
        const std::vector<PlayMetricValue> previous = {Value("same", true, 600, 0), Value("faster", true, 900, 0),
                                                       Value("gone", false, 60, 3)};
        const std::vector<PlayMetricValue> current = {Value("same", true, 600, 0), Value("faster", true, 540, 0),
                                                      Value("new", true, 60, 0)};
        const std::vector<PlayMetricDiff> diffs = ComparePlayMetrics(current, previous);
        EXPECT(diffs.size() == 4);
        if (diffs.size() != 4)
            return;

        EXPECT(diffs[0].key == "same" && !diffs[0].changed && diffs[0].deltaTicks == 0);
        EXPECT(diffs[1].key == "faster" && diffs[1].changed && diffs[1].deltaTicks == -360);
        EXPECT(diffs[2].key == "new" && diffs[2].changed && !diffs[2].previous);
        EXPECT(diffs[3].key == "gone" && diffs[3].changed && !diffs[3].current);

        const std::string report = FormatPlayMetricReport(diffs, 60);
        EXPECT(Contains(report, "4 個") && Contains(report, "前の版と同じ") && Contains(report, "刻み -360") &&
               Contains(report, "前の版なし") && Contains(report, "今の表に無い"));
        EXPECT(FormatTicksAsSeconds(750, 60) == "12.50 秒" && FormatTicksAsSeconds(61, 60) == "1.01 秒");

        // 往復(道のりと表示の名前は残さない。名前は鍵)
        std::vector<PlayMetricValue> withSpace = current;
        withSpace.push_back(Value("key with space", false, 36000, 12));
        const auto parsed = ParsePlayMetricValues(SerializePlayMetricValues(withSpace));
        EXPECT(parsed && *parsed == withSpace);
        const auto crlf = ParsePlayMetricValues("bicameral-play-metrics 1\r\n1 1 600 0 same\r\n");
        EXPECT(crlf && crlf->size() == 1 && crlf->front() == Value("same", true, 600, 0));

        EXPECT(!ParsePlayMetricValues(""));
        EXPECT(!ParsePlayMetricValues("other 1\n"));
        const auto broken = ParsePlayMetricValues("bicameral-play-metrics 1\n1 1 six 0 same\n");
        EXPECT(!broken && Contains(broken.error(), "2 行目"));
    }

}  // namespace

int main() {
    TestSamplePackage();
    TestRead();
    TestBake();
    TestObserver();
    TestCompare();
    if (failureCount != 0) {
        std::printf("play_metric_test: %d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("play_metric_test: 通過\n");
    return 0;
}
