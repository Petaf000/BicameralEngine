// gas_fastest_layout.cpp — 最速 × 小さい空きの検査を仮の世界の区画と置いた世界で呼ぶ(gas_fastest_layout.h。T-0804・D-526 ①)。
#include "sim/gas_fastest_layout.h"

#include <format>
#include <optional>

#include "common/probe_sim.hlsli"
#include "sim/cell_volume.h"
#include "sim/gas_reference.h"
#include "sim/world_layout.h"

namespace bicameral::sim {

    static_assert(CELL_OPEN_FULL == GAS_OPEN_FULL);

    std::vector<uint32_t> CollectProbeGasOpen(const BakedReactionTable& table, std::span<const reaction::RxCell> cells,
                                              GasOpenCounts& counts) {
        std::vector<uint32_t> open(cells.size(), CELL_OPEN_FULL);
        bool partial = false;
        for (uint32_t region = 0; region < cells.size(); ++region) {
            const CellVolumeShare share = ShareCellVolume(table, cells[region]);
            open[region] = GasOpenQ16(share, PROBE_CELL_VOLUME_PICOLITERS);
            if (open[region] == CELL_OPEN_FULL)
                continue;

            partial = true;
            if (open[region] == 0)
                ++counts.blockedCells;
            else
                ++counts.partialCells;
        }

        if (!partial)
            open.clear();

        return open;
    }

    std::string ProbeRegionJointWarning(const GasSmallOpenJoint& joint, const std::array<uint32_t, 3>& minCell) {
        // 気体の箱の座標(x・y・z が上)→ 格子の座標(x・y が上・z)
        const auto grid = [&](const std::array<uint32_t, GAS_AXES>& at, uint32_t gridAxis) {
            const uint32_t gasAxis = PROBE_GRID_AXIS_OF_GAS_AXIS[gridAxis];
            return minCell[gridAxis] + at[gasAxis];
        };
        const std::string range = std::format("格子の x {}〜{}・y {}〜{}・z {}〜{}", grid(joint.minCell, 0),
                                              grid(joint.maxCell, 0), grid(joint.minCell, 1), grid(joint.maxCell, 1),
                                              grid(joint.minCell, 2), grid(joint.maxCell, 2));

        return GasSmallOpenJointWarning(joint, "気体の区画", range);
    }

    std::vector<std::string> CheckLayoutFastestOpenings(const BakedReactionTable& table) {
        const std::optional<WorldGasRegion>& gasRegion = table.worldLayout.gasRegion;
        if (table.environment.gasPressureSpeed != GasPressureSpeed::Instant || !gasRegion)
            return {};

        // --- 区画の箱(検査は箱の大きさと境界だけを読む。気体の箱の軸 = 格子の x・z・y)---
        const std::array<uint32_t, 3>& size = gasRegion->sizeCells;
        GasConfig config;
        config.sizeX = size[PROBE_GRID_AXIS_OF_GAS_AXIS[0]];
        config.sizeY = size[PROBE_GRID_AXIS_OF_GAS_AXIS[1]];
        config.sizeZ = size[PROBE_GRID_AXIS_OF_GAS_AXIS[2]];
        for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
            config.boundary[axis] = gasRegion->boundary[PROBE_GRID_AXIS_OF_GAS_AXIS[axis]];

        // --- 区画のセルの初めの中身(区画の中の x + 幅 × (z + 奥行き × y)。気体の箱と同じ並び。空のセルは真空)---
        std::vector<reaction::RxCell> cells(size[0] * size[1] * size[2], reaction::RxMakeEmptyCell(0));
        for (uint32_t index = 0; index < cells.size(); ++index) {
            const uint32_t x = gasRegion->minCell[0] + (index % size[0]);
            const uint32_t z = gasRegion->minCell[2] + ((index / size[0]) % size[2]);
            const uint32_t y = gasRegion->minCell[1] + (index / (size[0] * size[2]));
            for (const WorldCellRegion& placed : table.worldLayout.cellRegions) {
                if (WorldLayout::Contains(placed, x, y, z))
                    cells[index] = placed.cell;
            }
        }

        GasOpenCounts counts;
        const std::vector<uint32_t> open = CollectProbeGasOpen(table, cells, counts);
        std::vector<std::string> warnings;
        for (const GasSmallOpenJoint& joint : FindGasSmallOpenJoints(config, open))
            warnings.push_back(ProbeRegionJointWarning(joint, gasRegion->minCell));

        return warnings;
    }

}  // namespace bicameral::sim
