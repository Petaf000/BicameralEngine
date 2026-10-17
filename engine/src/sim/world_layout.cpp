// world_layout.cpp — 世界の組み立てのベイク(world_layout.h。T-0398)。物質の名前を ID に直し、領域ごとのセルを作る。
#include "sim/world_layout.h"

#include <algorithm>
#include <format>
#include <span>

#include "common/probe_sim.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    namespace {

        std::expected<reaction::RxCell, std::string> BakeRegionCell(const WorldCellRegionDefinition& region,
                                                                    const BakedReactionTable& table) {
            if (region.contents.size() > reaction::RX_MAX_CELL_SPECIES) {
                return std::unexpected(std::format("world_cells.{}: 中身の物質は {} 種まで(1 セルの成分の上限)",
                                                   region.name, static_cast<uint32_t>(reaction::RX_MAX_CELL_SPECIES)));
            }

            std::vector<SpeciesAmount> amounts;
            for (const WorldCellAmount& entry : region.contents) {
                const uint32_t species = table.SpeciesId(entry.species);
                if (species == 0) {
                    return std::unexpected(
                        std::format("world_cells.{}.amounts_umol: 知らない物質 {}", region.name, entry.species));
                }

                if (std::ranges::contains(amounts, species, &SpeciesAmount::species)) {
                    return std::unexpected(
                        std::format("world_cells.{}.amounts_umol: 物質 {} が 2 回ある", region.name, entry.species));
                }

                amounts.push_back({.species = species, .amount = entry.micromoles});
            }

            if (auto fits = CheckCellAmounts(table, amounts); !fits)
                return std::unexpected(std::format("world_cells.{}.amounts_umol: {}", region.name, fits.error()));

            return MakeReactionCell(table, amounts, region.temperatureMilliKelvin);
        }

        bool InSpan(uint32_t value, uint32_t begin, uint32_t size) {
            return value >= begin && value - begin < size;
        }

        // 箱の中の value が、殻(外側の厚さ shell)に入るか
        bool InShell(uint32_t value, uint32_t begin, uint32_t size, uint32_t shell) {
            const uint32_t offset = value - begin;
            return offset < shell || size - offset <= shell;
        }

        // --- 流れる区画(T-0671)---

        // 箱が仮の世界の格子(PROBE_GRID_SIZE³)に収まるか
        std::expected<void, std::string> CheckInGrid(const std::string& path, const std::array<uint32_t, 3>& minCell,
                                                     const std::array<uint32_t, 3>& sizeCells) {
            for (uint32_t axis = 0; axis < 3; ++axis) {
                if (uint64_t{minCell[axis]} + sizeCells[axis] > PROBE_GRID_SIZE) {
                    return std::unexpected(std::format("{}: 箱が格子({} セル角)からはみ出す(軸 {}: {} + {})", path,
                                                       PROBE_GRID_SIZE, "xyz"[axis], minCell[axis], sizeCells[axis]));
                }
            }

            return {};
        }

        // 区画は今は世界に 1 つまで(ProbeSim の区画が 1 つ。T-0671)。無ければ nullopt
        template <typename Region>
        std::expected<std::optional<Region>, std::string> OnlyRegion(std::string_view category,
                                                                     const std::vector<Region>& regions) {
            if (regions.empty())
                return std::nullopt;

            if (regions.size() > 1) {
                return std::unexpected(std::format("{}: 区画は今は世界に 1 つまで({} と {} がある)", category,
                                                   regions[0].name, regions[1].name));
            }

            const std::string path = std::format("{}.{}", category, regions[0].name);
            if (auto inGrid = CheckInGrid(path, regions[0].minCell, regions[0].sizeCells); !inGrid)
                return std::unexpected(inGrid.error());

            return regions[0];
        }

        // 気体は基準の大気(層ごとの基準の状態)、液体は重力が要る
        std::expected<void, std::string> BakeFlowRegions(const WorldLayoutDefinition& definition,
                                                         const BakedReactionTable& table, WorldLayout& layout) {
            auto gas = OnlyRegion("gas_regions", definition.gasRegions);
            if (!gas)
                return std::unexpected(gas.error());

            if (*gas && table.atmosphere.Empty()) {
                return std::unexpected(
                    std::format("gas_regions.{}: 基準の大気が無い世界では気体を流せない"
                                "(environment.atmosphere_profile か atmosphere を書く)",
                                (*gas)->name));
            }

            auto liquid = OnlyRegion("liquid_regions", definition.liquidRegions);
            if (!liquid)
                return std::unexpected(liquid.error());

            if (*liquid && table.environment.gravityMicrometersPerSecond2 == 0) {
                return std::unexpected(std::format(
                    "liquid_regions.{}: 重力が 0 の世界では液体を流せない(environment.gravity_um_per_s2 を書く)",
                    (*liquid)->name));
            }

            layout.gasRegion = std::move(*gas);
            layout.liquidRegion = std::move(*liquid);

            return {};
        }

    }  // namespace

    bool WorldLayout::Contains(const WorldCellRegion& region, uint32_t x, uint32_t y, uint32_t z) {
        if (region.wholeWorld)
            return true;

        const std::array<uint32_t, 3> cell{x, y, z};
        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (!InSpan(cell[axis], region.minCell[axis], region.sizeCells[axis]))
                return false;
        }

        if (region.shellCells == 0)
            return true;

        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (InShell(cell[axis], region.minCell[axis], region.sizeCells[axis], region.shellCells))
                return true;
        }

        return false;
    }

    std::expected<WorldLayout, std::string> BakeWorldLayout(const WorldLayoutDefinition& definition,
                                                            const BakedReactionTable& table) {
        // --- セルの領域: 層の順(同じ層は名前の順)---
        std::vector<WorldCellRegionDefinition> regions = definition.cellRegions;
        std::ranges::stable_sort(regions, [](const auto& left, const auto& right) {
            return left.layer != right.layer ? left.layer < right.layer : left.name < right.name;
        });

        WorldLayout layout;
        for (const WorldCellRegionDefinition& region : regions) {
            const auto cell = BakeRegionCell(region, table);
            if (!cell)
                return std::unexpected(cell.error());

            layout.cellRegions.push_back({.wholeWorld = region.wholeWorld,
                                          .minCell = region.minCell,
                                          .sizeCells = region.sizeCells,
                                          .shellCells = region.shellCells,
                                          .cell = *cell});
        }

        // --- 物: 並びの順(同じ並びは名前の順)---
        layout.bodies = definition.bodies;
        std::ranges::stable_sort(layout.bodies, [](const auto& left, const auto& right) {
            return left.order != right.order ? left.order < right.order : left.name < right.name;
        });

        // --- 流れる区画 ---
        if (auto flow = BakeFlowRegions(definition, table, layout); !flow)
            return std::unexpected(flow.error());

        return layout;
    }

}  // namespace bicameral::sim
