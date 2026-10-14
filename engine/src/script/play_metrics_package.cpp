// play_metrics_package.cpp — 分類 play_metrics を読む(play_metrics_package.h。T-0432・ADR-0206)。
// ここで落とすのは「形」の誤り(欄の綴り・型・整数でない数・範囲の外・量と欄の組み合わせ)。物質の名前・上限と間隔の関係・
// 格子の外のつつきはベイク(sim::BakePlayMetrics)が確かめる。
#include "script/play_metrics_package.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>

#include "script/package_fields.h"

namespace bicameral::script {

    namespace {

        using sim::PlayMetricDefinition;
        using sim::PlayMetricPoke;
        using sim::PlayMetricQuantity;
        using sim::PlayMetricStop;

        constexpr int64_t TICKS_MAX = int64_t{1} << 40;  // 早送りの上限(約 580 年ぶんの刻み。整数の器の目安)
        constexpr int64_t INTERVAL_MAX = std::numeric_limits<int32_t>::max();
        constexpr int64_t CELL_MAX = 255;  // つつきの場所は 1 軸 8bit(格子の大きさはベイクが確かめる)
        constexpr int64_t TEMPERATURE_MAX = std::numeric_limits<int32_t>::max();  // mK
        constexpr int64_t THRESHOLD_MAX = PACKAGE_EXACT_INTEGER_LIMIT - 1;

        struct QuantityName {
            std::string_view name;
            PlayMetricQuantity quantity;
        };

        constexpr std::array<QuantityName, 3> QUANTITY_NAMES = {{
            {"max_temperature", PlayMetricQuantity::MaxTemperature},
            {"cells_at_or_above", PlayMetricQuantity::CellsAtOrAbove},
            {"species_amount", PlayMetricQuantity::SpeciesAmount},
        }};

        // 欄 field の空でない文字列
        std::expected<std::string, std::string> ReadText(const ScriptValue& table, std::string_view field,
                                                         std::string_view path) {
            const ScriptValue* value = table.Find(field);
            if (value == nullptr || !value->IsString() || value->text.empty())
                return std::unexpected(std::format("{}.{}: 空でない文字列で書く", path, field));

            return value->text;
        }

        // { tick = …, cell = { x, y, z } }
        std::expected<PlayMetricPoke, std::string> ReadPoke(const ScriptValue& value, const std::string& path) {
            static constexpr std::array<std::string_view, 2> FIELDS = {"tick", "cell"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            const auto tick = ReadIntegerField(value, "tick", path, 0, TICKS_MAX);
            if (!tick)
                return std::unexpected(tick.error());

            const auto cell = RequireField(value, "cell", path);
            if (!cell)
                return std::unexpected(cell.error());

            const std::string cellPath = path + ".cell";
            const ScriptValue& vector = **cell;
            if (!vector.IsTable() || vector.fields.size() != 3)
                return std::unexpected(std::format("{}: {{ x, y, z }} の 3 つの数で書く", cellPath));

            std::array<uint32_t, 3> coordinates{};
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const ScriptField& field = vector.fields[axis];
                if (!field.key.IsNumber() || field.key.number != static_cast<double>(axis + 1))
                    return std::unexpected(std::format("{}: {{ x, y, z }} の 3 つの数で書く", cellPath));

                const auto component = ReadInteger(field.value, std::format("{}[{}]", cellPath, axis + 1), 0, CELL_MAX);
                if (!component)
                    return std::unexpected(component.error());

                coordinates[axis] = static_cast<uint32_t>(*component);
            }

            return PlayMetricPoke{
                .tick = static_cast<uint64_t>(*tick), .x = coordinates[0], .y = coordinates[1], .z = coordinates[2]};
        }

        // pokes = { {…}, {…} }(書いた順。鍵は 1, 2, 3…)。無ければ空
        std::expected<std::vector<PlayMetricPoke>, std::string> ReadPokes(const ScriptValue& table,
                                                                          const std::string& path) {
            const ScriptValue* value = table.Find("pokes");
            if (value == nullptr)
                return std::vector<PlayMetricPoke>{};

            const std::string pokesPath = path + ".pokes";
            if (!value->IsTable())
                return std::unexpected(std::format("{}: 表(列)で書く", pokesPath));

            std::vector<PlayMetricPoke> pokes;
            for (const ScriptField& field : value->fields) {
                if (!field.key.IsNumber() || field.key.number != static_cast<double>(pokes.size() + 1))
                    return std::unexpected(std::format("{}: 列(1, 2, 3…)で書く", pokesPath));

                auto poke = ReadPoke(field.value, std::format("{}[{}]", pokesPath, pokes.size() + 1));
                if (!poke)
                    return std::unexpected(poke.error());

                pokes.push_back(*poke);
            }

            return pokes;
        }

        // quantity と、その量だけが使う欄(temperature_mk・species)
        std::expected<void, std::string> ReadQuantity(const ScriptValue& value, const std::string& path,
                                                      PlayMetricDefinition& metric) {
            const auto name = ReadText(value, "quantity", path);
            if (!name)
                return std::unexpected(name.error());

            const auto found = std::ranges::find(QUANTITY_NAMES, *name, &QuantityName::name);
            if (found == QUANTITY_NAMES.end()) {
                return std::unexpected(std::format(
                    "{}.quantity: 知らない量 {}(max_temperature・cells_at_or_above・species_amount)", path, *name));
            }

            metric.quantity = found->quantity;
            const bool usesTemperature = metric.quantity == PlayMetricQuantity::CellsAtOrAbove;
            const bool usesSpecies = metric.quantity == PlayMetricQuantity::SpeciesAmount;
            if (!usesTemperature && value.Find("temperature_mk") != nullptr)
                return std::unexpected(std::format("{}.temperature_mk: cells_at_or_above のときだけ書く", path));

            if (!usesSpecies && value.Find("species") != nullptr)
                return std::unexpected(std::format("{}.species: species_amount のときだけ書く", path));

            if (usesTemperature) {
                const auto temperature = ReadIntegerField(value, "temperature_mk", path, 0, TEMPERATURE_MAX);
                if (!temperature)
                    return std::unexpected(temperature.error());

                metric.temperatureMillikelvin = static_cast<uint32_t>(*temperature);
            }

            if (usesSpecies) {
                auto species = ReadText(value, "species", path);
                if (!species)
                    return std::unexpected(species.error());

                metric.species = std::move(*species);
            }

            return {};
        }

        // at_most・at_least(どちらか 1 つ。どちらも無ければ上限の量を測る)
        std::expected<void, std::string> ReadStop(const ScriptValue& value, const std::string& path,
                                                  PlayMetricDefinition& metric) {
            const bool atMost = value.Find("at_most") != nullptr;
            const bool atLeast = value.Find("at_least") != nullptr;
            if (atMost && atLeast)
                return std::unexpected(std::format("{}: at_most と at_least はどちらか 1 つ", path));

            if (!atMost && !atLeast)
                return {};

            const auto threshold = ReadIntegerField(value, atMost ? "at_most" : "at_least", path, 0, THRESHOLD_MAX);
            if (!threshold)
                return std::unexpected(threshold.error());

            metric.stop = atMost ? PlayMetricStop::AtMost : PlayMetricStop::AtLeast;
            metric.threshold = static_cast<uint64_t>(*threshold);

            return {};
        }

        // 量を測る範囲(min_cell と size_cells は両方書くか両方省く。省けば世界全体。T-0559。格子に収まるかはベイク)
        std::expected<void, std::string> ReadRegion(const ScriptValue& value, const std::string& path,
                                                    PlayMetricDefinition& metric) {
            const bool hasMin = value.Find("min_cell") != nullptr;
            const bool hasSize = value.Find("size_cells") != nullptr;
            if (hasMin != hasSize)
                return std::unexpected(std::format("{}: min_cell と size_cells は両方書くか両方省く", path));
            if (!hasMin)
                return {};

            const auto minCell = ReadVector3Field(value, "min_cell", path, 0, CELL_MAX);
            if (!minCell)
                return std::unexpected(minCell.error());

            const auto sizeCells = ReadVector3Field(value, "size_cells", path, 1, CELL_MAX + 1);
            if (!sizeCells)
                return std::unexpected(sizeCells.error());

            metric.region.wholeWorld = false;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                metric.region.minCell[axis] = static_cast<uint32_t>((*minCell)[axis]);
                metric.region.sizeCells[axis] = static_cast<uint32_t>((*sizeCells)[axis]);
            }

            return {};
        }

        std::expected<PlayMetricDefinition, std::string> ReadMetric(const std::string& key, const ScriptValue& value,
                                                                    const std::string& path) {
            static constexpr std::array<std::string_view, 11> FIELDS = {
                "label",      "max_ticks", "sample_interval_ticks", "pokes",   "quantity", "min_cell",
                "size_cells", "species",   "temperature_mk",        "at_most", "at_least"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            PlayMetricDefinition metric{.key = key};
            if (value.Find("label") != nullptr) {
                auto label = ReadText(value, "label", path);
                if (!label)
                    return std::unexpected(label.error());

                metric.label = std::move(*label);
            }

            const auto maxTicks = ReadIntegerField(value, "max_ticks", path, 1, TICKS_MAX);
            if (!maxTicks)
                return std::unexpected(maxTicks.error());

            const auto interval = ReadIntegerField(value, "sample_interval_ticks", path, 1, INTERVAL_MAX);
            if (!interval)
                return std::unexpected(interval.error());

            auto pokes = ReadPokes(value, path);
            if (!pokes)
                return std::unexpected(pokes.error());

            if (auto quantity = ReadQuantity(value, path, metric); !quantity)
                return std::unexpected(quantity.error());

            if (auto stop = ReadStop(value, path, metric); !stop)
                return std::unexpected(stop.error());

            if (auto region = ReadRegion(value, path, metric); !region)
                return std::unexpected(region.error());

            metric.maxTicks = static_cast<uint64_t>(*maxTicks);
            metric.sampleIntervalTicks = static_cast<uint32_t>(*interval);
            metric.pokes = std::move(*pokes);

            return metric;
        }

    }  // namespace

    std::expected<std::vector<sim::PlayMetricDefinition>, std::string> ReadPlayMetrics(
        const PackageSetResult& packages) {
        std::vector<PlayMetricDefinition> metrics;
        if (auto result = ReadCategory(packages, PLAY_METRICS_CATEGORY, ReadMetric, metrics); !result)
            return std::unexpected(result.error());

        return metrics;
    }

}  // namespace bicameral::script
