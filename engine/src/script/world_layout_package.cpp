// world_layout_package.cpp — 分類 world_cells・world_bodies を読む(world_layout_package.h。T-0398)。gas_regions・liquid_regions は読まず注意だけ(D-510・T-0724)。
// ここで落とすのは「形」の誤り(欄の綴り・型・整数でない数・範囲の外)。物質の名前はベイク(sim::BakeWorldLayout)が確かめる。
#include "script/world_layout_package.h"

#include <array>
#include <format>
#include <limits>
#include <vector>

#include "common/units.hlsli"
#include "script/package_fields.h"

namespace bicameral::script {

    namespace {

        using sim::WorldBodyDefinition;
        using sim::WorldCellAmount;
        using sim::WorldCellRegionDefinition;

        constexpr int64_t CELL_COORDINATE_MAX = int64_t{1}
                                                << 20;  // セルの位置・大きさの上限(格子の大きさの器より十分大きい)
        // 1 セルの 1 物質(µmol)。セルの合計と気体のぶんはベイク(sim::BakeWorldLayout)が表の相で確かめる(ADR-0260)
        constexpr auto CELL_AMOUNT_MAX = static_cast<int64_t>(fx::CELL_AMOUNT_LIMIT_MICROMOLES);
        constexpr int64_t TEMPERATURE_MAX = std::numeric_limits<int32_t>::max();  // mK
        constexpr int64_t LENGTH_MAX_MILLIMETERS = 1000000000;  // 1000 km(2^-20 m に直しても int64 に入る)
        constexpr int64_t MASS_MAX_GRAMS = int64_t{1} << 40;    // mg に直しても uint64 に入る
        constexpr int64_t ORDER_LIMIT = int64_t{1} << 31;

        // 欄 field が無ければ fallback
        std::expected<int64_t, std::string> ReadOptionalInteger(const ScriptValue& table, std::string_view field,
                                                                std::string_view path, int64_t minimum, int64_t maximum,
                                                                int64_t fallback) {
            if (table.Find(field) == nullptr)
                return fallback;

            return ReadIntegerField(table, field, path, minimum, maximum);
        }

        // { x, y, z }(数の鍵 1〜3 だけの表)
        std::array<uint32_t, 3> ToCells(const std::array<int64_t, 3>& vector) {
            return {static_cast<uint32_t>(vector[0]), static_cast<uint32_t>(vector[1]),
                    static_cast<uint32_t>(vector[2])};
        }

        // --- セルの領域 ---

        std::expected<std::vector<WorldCellAmount>, std::string> ReadAmounts(const ScriptValue& table,
                                                                             const std::string& path) {
            const auto value = RequireField(table, "amounts_umol", path);
            if (!value)
                return std::unexpected(value.error());

            const std::string amountsPath = path + ".amounts_umol";
            if (auto record = RequireTable(**value, amountsPath); !record)
                return std::unexpected(record.error());

            std::vector<WorldCellAmount> amounts;
            for (const ScriptField& field : (*value)->fields) {
                if (!field.key.IsString())
                    return std::unexpected(std::format("{}: 鍵は物質の名前", amountsPath));

                const auto amount = ReadInteger(field.value, std::format("{}.{}", amountsPath, field.key.text), 1,
                                                CELL_AMOUNT_MAX);
                if (!amount)
                    return std::unexpected(amount.error());

                amounts.push_back({.species = field.key.text, .micromoles = static_cast<uint64_t>(*amount)});
            }

            return amounts;
        }

        // 場所(min_cell と size_cells は両方書くか両方省く。省けば格子の全部。shell_cells は箱のときだけ)
        std::expected<void, std::string> ReadRegionPlace(const ScriptValue& value, const std::string& path,
                                                         WorldCellRegionDefinition& region) {
            const bool hasMin = value.Find("min_cell") != nullptr;
            const bool hasSize = value.Find("size_cells") != nullptr;
            if (hasMin != hasSize)
                return std::unexpected(std::format("{}: min_cell と size_cells は両方書くか両方省く", path));

            region.wholeWorld = !hasMin;
            if (region.wholeWorld) {
                if (value.Find("shell_cells") != nullptr)
                    return std::unexpected(std::format("{}: shell_cells は箱(min_cell・size_cells)のときだけ", path));

                return {};
            }

            const auto minCell = ReadVector3Field(value, "min_cell", path, 0, CELL_COORDINATE_MAX);
            if (!minCell)
                return std::unexpected(minCell.error());

            const auto sizeCells = ReadVector3Field(value, "size_cells", path, 1, CELL_COORDINATE_MAX);
            if (!sizeCells)
                return std::unexpected(sizeCells.error());

            const auto shell = ReadOptionalInteger(value, "shell_cells", path, 0, CELL_COORDINATE_MAX, 0);
            if (!shell)
                return std::unexpected(shell.error());

            region.minCell = ToCells(*minCell);
            region.sizeCells = ToCells(*sizeCells);
            region.shellCells = static_cast<uint32_t>(*shell);

            return {};
        }

        std::expected<WorldCellRegionDefinition, std::string> ReadCellRegion(const std::string& name,
                                                                             const ScriptValue& value,
                                                                             const std::string& path) {
            static constexpr std::string_view FIELDS[] = {"layer",       "min_cell",     "size_cells",
                                                          "shell_cells", "amounts_umol", "temperature_mk"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            WorldCellRegionDefinition region{.name = name};
            const auto layer = ReadOptionalInteger(value, "layer", path, -ORDER_LIMIT, ORDER_LIMIT, 0);
            if (!layer)
                return std::unexpected(layer.error());

            if (auto place = ReadRegionPlace(value, path, region); !place)
                return std::unexpected(place.error());

            auto amounts = ReadAmounts(value, path);
            if (!amounts)
                return std::unexpected(amounts.error());

            const auto temperature = ReadIntegerField(value, "temperature_mk", path, 0, TEMPERATURE_MAX);
            if (!temperature)
                return std::unexpected(temperature.error());

            region.layer = *layer;
            region.contents = std::move(*amounts);
            region.temperatureMilliKelvin = static_cast<int32_t>(*temperature);

            return region;
        }

        // --- 物 ---

        std::expected<WorldBodyDefinition, std::string> ReadBody(const std::string& name, const ScriptValue& value,
                                                                 const std::string& path) {
            static constexpr std::string_view FIELDS[] = {"order", "size_mm", "position_mm", "mass_g"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            const auto order = ReadOptionalInteger(value, "order", path, -ORDER_LIMIT, ORDER_LIMIT, 0);
            if (!order)
                return std::unexpected(order.error());

            const auto size = ReadVector3Field(value, "size_mm", path, 1, LENGTH_MAX_MILLIMETERS);
            if (!size)
                return std::unexpected(size.error());

            const auto position = ReadVector3Field(value, "position_mm", path, -LENGTH_MAX_MILLIMETERS,
                                                   LENGTH_MAX_MILLIMETERS);
            if (!position)
                return std::unexpected(position.error());

            const auto mass = ReadIntegerField(value, "mass_g", path, 0, MASS_MAX_GRAMS);
            if (!mass)
                return std::unexpected(mass.error());

            return WorldBodyDefinition{.name = name,
                                       .order = *order,
                                       .sizeMillimeters = *size,
                                       .positionMillimeters = *position,
                                       .massGrams = static_cast<uint64_t>(*mass)};
        }

        // --- 読まない分類(D-510・T-0724)---

        // 区画の箱(gas_regions・liquid_regions)はやめた。書いてあれば読まずに注意を出す(どのパッケージが書いたかを付ける)
        void NoteRetiredCategory(const PackageSetResult& packages, std::string_view category,
                                 std::vector<std::string>& notes) {
            const auto found = packages.tables.find(std::string(category));
            if (found == packages.tables.end() || found->second.empty())
                return;

            const auto& [key, entry] = *found->second.begin();
            notes.push_back(
                std::format("注意: パッケージ {}: {} は読まない(D-510: 区画の箱はやめ、気体は基準の大気があれば・"
                            "液体は重力があれば世界のどこでも流れる。{}.{} などを消す)",
                            entry.package, category, category, key));
        }

    }  // namespace

    std::expected<sim::WorldLayoutDefinition, std::string> ReadWorldLayout(const PackageSetResult& packages) {
        sim::WorldLayoutDefinition layout;
        if (auto result = ReadCategory(packages, WORLD_CELLS_CATEGORY, ReadCellRegion, layout.cellRegions); !result)
            return std::unexpected(result.error());

        if (auto result = ReadCategory(packages, WORLD_BODIES_CATEGORY, ReadBody, layout.bodies); !result)
            return std::unexpected(result.error());

        NoteRetiredCategory(packages, GAS_REGIONS_CATEGORY, layout.retiredCategories);
        NoteRetiredCategory(packages, LIQUID_REGIONS_CATEGORY, layout.retiredCategories);

        return layout;
    }

}  // namespace bicameral::script
