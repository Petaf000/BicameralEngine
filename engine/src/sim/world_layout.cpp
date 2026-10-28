// world_layout.cpp — 世界の組み立てのベイク(world_layout.h。T-0398)。物質の名前を ID に直し、領域ごとのセルを作る。
// 層(T-1236): 領域の settled を成分の並びの位置ごとの層にし、書かなかった粒の物質を読み方(UnwrittenGrainLayers)で埋める / 知らせる。
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

        // --- 層(T-1236・ADR-0489)---

        // セルの成分の並びで物質 species の位置(無ければ成分の数)
        uint32_t SpeciesPosition(const reaction::RxCell& cell, uint32_t species) {
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] == species)
                    return i;
            }

            return cell.speciesCount;
        }

        // 書いた層を並びの位置に置く(中身に無い物質・量を超える層・同じ物質が 2 回は誤り)。書いた位置の印を返す
        std::expected<uint32_t, std::string> PlaceWrittenLayers(const WorldCellRegionDefinition& region,
                                                                const BakedReactionTable& table,
                                                                const reaction::RxCell& cell, WorldCellRegion& baked) {
            uint32_t written = 0;
            for (const WorldCellAmount& entry : region.settled) {
                const std::string path = std::format("world_cells.{}.settled_umol.{}", region.name, entry.species);
                const uint32_t species = table.SpeciesId(entry.species);
                const uint32_t position = species == 0 ? cell.speciesCount : SpeciesPosition(cell, species);
                if (position == cell.speciesCount)
                    return std::unexpected(std::format("{}: 中身(amounts_umol)に無い物質の層", path));

                if ((written & (1u << position)) != 0)
                    return std::unexpected(std::format("{}: 同じ物質の層が 2 回ある", path));

                if (entry.micromoles > cell.amounts[position]) {
                    return std::unexpected(std::format("{}: 層 {} µmol が量 {} µmol を超える(層 ≤ 量)", path,
                                                       entry.micromoles, cell.amounts[position]));
                }

                baked.layers[position] = entry.micromoles;
                written |= 1u << position;
            }

            return written;
        }

        // 層を書かなかった粒の物質: Settled なら全部が層、Floating なら層 0 のまま知らせを足す
        void ReadUnwrittenGrains(const WorldCellRegionDefinition& region, const BakedReactionTable& table,
                                 uint32_t written, UnwrittenGrainLayers grainReading, WorldCellRegion& baked,
                                 std::vector<std::string>& notes) {
            const reaction::RxCell& cell = baked.cell;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const uint32_t species = cell.species[i];
                const bool grain = (table.species[species].flags & reaction::RX_SPECIES_GRAIN) != 0;
                if (!grain || (written & (1u << i)) != 0)
                    continue;

                if (grainReading == UnwrittenGrainLayers::Settled) {
                    baked.layers[i] = cell.amounts[i];
                    continue;
                }

                notes.push_back(
                    std::format("注意: world_cells.{}: 粒の物質 {} "
                                "の層(settled_umol)を書いていない。層を使う世界では全部が舞う粒になる"
                                "(細かくすると空きに同じ濃さで広がり、頁は眠らない)。積もらせるなら settled_umol "
                                "に書く(舞わせるなら 0)",
                                region.name, table.speciesNames[species]));
            }
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
                                                            const BakedReactionTable& table,
                                                            UnwrittenGrainLayers grainReading) {
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

            WorldCellRegion baked{.wholeWorld = region.wholeWorld,
                                  .minCell = region.minCell,
                                  .sizeCells = region.sizeCells,
                                  .shellCells = region.shellCells,
                                  .cell = *cell};
            const auto written = PlaceWrittenLayers(region, table, *cell, baked);
            if (!written)
                return std::unexpected(written.error());

            ReadUnwrittenGrains(region, table, *written, grainReading, baked, layout.unsettledGrainNotes);
            layout.cellRegions.push_back(baked);
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

    WorldLayoutCell LayoutCellAt(const WorldLayout& layout, uint32_t x, uint32_t y, uint32_t z) {
        WorldLayoutCell placed{.cell = reaction::RxMakeEmptyCell(0)};
        for (const WorldCellRegion& region : layout.cellRegions) {
            if (WorldLayout::Contains(region, x, y, z))
                placed = {.cell = region.cell, .layers = region.layers};
        }

        return placed;
    }

}  // namespace bicameral::sim
