// play_metric.cpp — 遊びの指標のベイク・前の版との差・前の版の値のファイル(play_metric.h。T-0432・ADR-0206)。
// 測る所(GPU の写し・CPU リファレンス)は sim/play_metric_measure。浮動小数点は使わない。
#include "sim/play_metric.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <span>

#include "common/probe_sim.hlsli"
#include "core/aliases.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    namespace {

        constexpr std::string_view FILE_HEADER = "bicameral-play-metrics 1";

        std::expected<PlayMetric, std::string> BakeOne(const PlayMetricDefinition& definition,
                                                       const BakedReactionTable& table) {
            const std::string path = std::format("play_metrics.{}", definition.key);
            if (definition.maxTicks == 0 || definition.sampleIntervalTicks == 0)
                return std::unexpected(std::format("{}: max_ticks と sample_interval_ticks は 1 以上", path));

            if (definition.maxTicks % definition.sampleIntervalTicks != 0) {
                return std::unexpected(std::format("{}: max_ticks({})は sample_interval_ticks({})の倍数で書く", path,
                                                   definition.maxTicks, definition.sampleIntervalTicks));
            }

            PlayMetric metric{.key = definition.key,
                              .label = definition.label.empty() ? definition.key : definition.label,
                              .maxTicks = definition.maxTicks,
                              .sampleIntervalTicks = definition.sampleIntervalTicks,
                              .pokes = definition.pokes,
                              .quantity = definition.quantity,
                              .temperatureMillikelvin = definition.temperatureMillikelvin,
                              .stop = definition.stop,
                              .threshold = definition.threshold};
            rng::stable_sort(metric.pokes, {}, &PlayMetricPoke::tick);
            if (!metric.pokes.empty() && metric.pokes.back().tick >= metric.maxTicks) {
                return std::unexpected(std::format("{}: つつきの刻み {} が上限 max_ticks({})以上", path,
                                                   metric.pokes.back().tick, metric.maxTicks));
            }

            for (const PlayMetricPoke& poke : metric.pokes) {
                if (poke.x >= PROBE_GRID_SIZE || poke.y >= PROBE_GRID_SIZE || poke.z >= PROBE_GRID_SIZE) {
                    return std::unexpected(std::format("{}.pokes: セル ({}, {}, {}) が格子({}³)の外", path, poke.x,
                                                       poke.y, poke.z, PROBE_GRID_SIZE));
                }
            }

            if (definition.quantity == PlayMetricQuantity::SpeciesAmount) {
                metric.species = table.SpeciesId(definition.species);
                if (metric.species == 0)
                    return std::unexpected(std::format("{}.species: 知らない物質 {}", path, definition.species));
            }

            return metric;
        }

        bool SameValue(const PlayMetricValue& a, const PlayMetricValue& b) {
            return a.measured == b.measured && a.reached == b.reached && a.ticks == b.ticks && a.quantity == b.quantity;
        }

        PlayMetricDiff MakeDiff(const PlayMetricValue* current, const PlayMetricValue* previous) {
            const PlayMetricValue& named = current != nullptr ? *current : *previous;
            PlayMetricDiff diff{.key = named.key, .label = named.label};
            if (current != nullptr)
                diff.current = *current;

            if (previous != nullptr)
                diff.previous = *previous;

            if (current == nullptr || previous == nullptr) {
                diff.changed = true;
                return diff;
            }

            diff.changed = !SameValue(*current, *previous);
            diff.deltaTicks = static_cast<int64_t>(current->ticks) - static_cast<int64_t>(previous->ticks);
            diff.deltaQuantity = static_cast<int64_t>(current->quantity) - static_cast<int64_t>(previous->quantity);

            return diff;
        }

        // 1 つの値の表示("着いた 12.50 秒(750 刻み)・量 0")
        std::string FormatValue(const PlayMetricValue& value, uint64_t ticksPerSecond) {
            if (!value.measured)
                return std::format("測れない({})", value.message);

            return std::format("{} {}({} 刻み)・量 {}", value.reached ? "着いた" : "上限",
                               FormatTicksAsSeconds(value.ticks, ticksPerSecond), value.ticks, value.quantity);
        }

        std::string FormatSigned(int64_t value) {
            return value > 0 ? std::format("+{}", value) : std::format("{}", value);
        }

        template <typename Integer>
        bool ParseInteger(std::string_view text, Integer& out) {
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
            return error == std::errc() && end == text.data() + text.size();
        }

        // 空白で区切った最初の語を取り出す
        std::string_view TakeWord(std::string_view& line) {
            const size_t start = line.find_first_not_of(' ');
            if (start == std::string_view::npos) {
                line = {};
                return {};
            }

            line.remove_prefix(start);
            const size_t end = std::min(line.find(' '), line.size());
            const std::string_view word = line.substr(0, end);
            line.remove_prefix(end);

            return word;
        }

        std::expected<PlayMetricValue, std::string> ParseLine(std::string_view line, size_t lineNumber) {
            const auto error = [&] {
                return std::unexpected(
                    std::format("指標のファイルの {} 行目: 「<0/1> <0/1> <刻み> <量> <鍵>」で書く", lineNumber));
            };

            uint32_t measured = 0;
            uint32_t reached = 0;
            PlayMetricValue value;
            if (!ParseInteger(TakeWord(line), measured) || !ParseInteger(TakeWord(line), reached) ||
                !ParseInteger(TakeWord(line), value.ticks) || !ParseInteger(TakeWord(line), value.quantity) ||
                measured > 1 || reached > 1) {
                return error();
            }

            const size_t start = line.find_first_not_of(' ');
            if (start == std::string_view::npos)
                return error();

            value.key = std::string(line.substr(start));
            value.label = value.key;
            value.measured = measured == 1;
            value.reached = reached == 1;

            return value;
        }

    }  // namespace

    // --- 指標 ---

    uint64_t PlayMetric::JudgeAfterTick() const {
        return pokes.empty() ? 0 : pokes.back().tick;
    }

    bool PlayMetric::Stops(uint64_t value) const {
        switch (stop) {
            case PlayMetricStop::AtMost: return value <= threshold;
            case PlayMetricStop::AtLeast: return value >= threshold;
            case PlayMetricStop::AtLimit: break;
        }

        return false;
    }

    std::expected<std::vector<PlayMetric>, std::string> BakePlayMetrics(
        const std::vector<PlayMetricDefinition>& definitions, const BakedReactionTable& table) {
        std::vector<const PlayMetricDefinition*> ordered;
        for (const PlayMetricDefinition& definition : definitions)
            ordered.push_back(&definition);

        rng::sort(ordered, {}, [](const PlayMetricDefinition* definition) { return definition->key; });

        std::vector<PlayMetric> metrics;
        for (const PlayMetricDefinition* definition : ordered) {
            auto metric = BakeOne(*definition, table);
            if (!metric)
                return std::unexpected(metric.error());

            metrics.push_back(std::move(*metric));
        }

        return metrics;
    }

    // --- 前の版との差 ---

    std::vector<PlayMetricDiff> ComparePlayMetrics(const std::vector<PlayMetricValue>& current,
                                                   const std::vector<PlayMetricValue>& previous) {
        std::map<std::string, const PlayMetricValue*, std::less<>> previousByKey;
        for (const PlayMetricValue& value : previous)
            previousByKey.emplace(value.key, &value);

        std::vector<PlayMetricDiff> diffs;
        for (const PlayMetricValue& value : current) {
            const auto found = previousByKey.find(value.key);
            diffs.push_back(MakeDiff(&value, found == previousByKey.end() ? nullptr : found->second));
            if (found != previousByKey.end())
                previousByKey.erase(found);
        }

        for (const auto& [key, value] : previousByKey)
            diffs.push_back(MakeDiff(nullptr, value));

        return diffs;
    }

    std::string_view PlayMetricQuantityUnit(PlayMetricQuantity quantity) {
        switch (quantity) {
            case PlayMetricQuantity::MaxTemperature: return "mK";
            case PlayMetricQuantity::CellsAtOrAbove: return "セル";
            case PlayMetricQuantity::SpeciesAmount: return "µmol";
        }

        return "";
    }

    std::string FormatTicksAsSeconds(uint64_t ticks, uint64_t ticksPerSecond) {
        if (ticksPerSecond == 0)
            return std::format("{} 刻み", ticks);

        // 小数 2 桁(切り捨て)
        const uint64_t hundredths = (ticks % ticksPerSecond) * 100 / ticksPerSecond;
        return std::format("{}.{:02} 秒", ticks / ticksPerSecond, hundredths);
    }

    std::string FormatPlayMetricReport(const std::vector<PlayMetricDiff>& diffs, uint64_t ticksPerSecond) {
        if (diffs.empty())
            return "遊びの指標: 登録が無い(パッケージの分類 play_metrics)";

        std::string report = std::format("遊びの指標: {} 個", diffs.size());
        for (const PlayMetricDiff& diff : diffs) {
            report += std::format("\n  {}({}): ", diff.label, diff.key);
            if (!diff.current) {
                report += std::format("今の表に無い(前の版: {})", FormatValue(*diff.previous, ticksPerSecond));
                continue;
            }

            report += FormatValue(*diff.current, ticksPerSecond);
            if (!diff.previous)
                report += " ・前の版なし";
            else if (!diff.changed)
                report += " ・前の版と同じ";
            else {
                report += std::format(" ・前の版 {} から 刻み {}・量 {}", FormatValue(*diff.previous, ticksPerSecond),
                                      FormatSigned(diff.deltaTicks), FormatSigned(diff.deltaQuantity));
            }
        }

        return report;
    }

    // --- 前の版の値のファイル ---

    std::string SerializePlayMetricValues(const std::vector<PlayMetricValue>& values) {
        std::string text(FILE_HEADER);
        text += '\n';
        for (const PlayMetricValue& value : values) {
            text += std::format("{} {} {} {} {}\n", value.measured ? 1 : 0, value.reached ? 1 : 0, value.ticks,
                                value.quantity, value.key);
        }

        return text;
    }

    std::expected<std::vector<PlayMetricValue>, std::string> ParsePlayMetricValues(std::string_view text) {
        std::vector<PlayMetricValue> values;
        size_t lineNumber = 0;
        while (!text.empty()) {
            const size_t end = std::min(text.find('\n'), text.size());
            std::string_view line = text.substr(0, end);
            text.remove_prefix(std::min(end + 1, text.size()));
            ++lineNumber;
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);

            if (lineNumber == 1) {
                if (line != FILE_HEADER)
                    return std::unexpected(std::format("指標のファイルの 1 行目が「{}」でない", FILE_HEADER));

                continue;
            }

            if (line.empty())
                continue;

            auto value = ParseLine(line, lineNumber);
            if (!value)
                return std::unexpected(value.error());

            values.push_back(std::move(*value));
        }

        if (lineNumber == 0)
            return std::unexpected("指標のファイルが空");

        return values;
    }

}  // namespace bicameral::sim
