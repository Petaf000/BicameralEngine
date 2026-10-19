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

        // --- 流れる所(D-510・T-0724)---

        // 区画の箱はやめた(D-510): 気体・液体は世界のどこでも流れる。仮の世界では「世界いっぱいの区画 1 つ」で表す
        // (端は閉じた壁。世界の端の種類〔environment.world_edges〕は多重解像度の世界が読む。D-514)。
        // 気体は基準の大気がある世界(層ごとの基準の状態が要る)、液体は重力がある世界で流れる
        void BakeFlowAreas(const BakedReactionTable& table, WorldLayout& layout) {
            const std::array<uint32_t, 3> whole = {PROBE_GRID_SIZE, PROBE_GRID_SIZE, PROBE_GRID_SIZE};
            if (!table.atmosphere.Empty())
                layout.gasRegion = WorldGasRegion{.name = "world", .minCell = {}, .sizeCells = whole};

            if (table.environment.gravityMicrometersPerSecond2 != 0)
                layout.liquidRegion = WorldLiquidRegion{.name = "world", .minCell = {}, .sizeCells = whole};
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

        // --- 流れる所(世界いっぱい。D-510)---
        BakeFlowAreas(table, layout);

        return layout;
    }

}  // namespace bicameral::sim
