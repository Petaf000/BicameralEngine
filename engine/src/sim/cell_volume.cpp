// cell_volume.cpp — 1 セルの体積を相で分け合う(T-0593・T-0612・ADR-0268)。約束は cell_volume.h。
#include "sim/cell_volume.h"

#include <algorithm>

#include "common/fixed.hlsli"

namespace bicameral::sim {

    namespace {

        // 占める体積の頭打ち(pL。どのセルの体積よりずっと大きく、固体 + 液体の和が 64bit を溢れない)
        constexpr uint64_t VOLUME_CAP = (uint64_t)1 << 62;

        // 占める体積の割合を除いた残り(Q16。切り捨て = 空きを小さめに。占める分がセル以上なら 0)
        uint32_t RemainingQ16(uint64_t occupied, uint64_t cellVolume) {
            if (occupied >= cellVolume)
                return 0;

            const fx::FxU128 product = fx::FxMulU64Full(cellVolume - occupied, CELL_OPEN_FULL);
            return (uint32_t)fx::FxDivU128By64(product, cellVolume).quotient;
        }

    }  // namespace

    CellVolumeShare ShareCellVolume(const BakedReactionTable& table, const reaction::RxCell& cell) {
        CellVolumeShare share;
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            const uint32_t species = cell.species[i];
            const bool condensed = species < table.speciesCondensed.size() && table.speciesCondensed[species] != 0;
            if (!condensed || species >= table.speciesMolarVolume.size())
                continue;

            // µmol × mm³/mol(セルの器 2^36 µmol × モル体積 2^27 mm³/mol までは 64bit に収まる。超える分は頭打ち = セルより大きい)
            const fx::FxU128 product = fx::FxMulU64Full(cell.amounts[i], table.speciesMolarVolume[species]);
            const uint64_t volume = product.hi != 0 ? VOLUME_CAP : std::min(product.lo, VOLUME_CAP);
            const bool liquid = species < table.speciesLiquid.size() && table.speciesLiquid[species] != 0;
            uint64_t& sum = liquid ? share.liquid : share.solid;
            sum = std::min(sum + volume, VOLUME_CAP);
        }

        return share;
    }

    uint32_t LiquidOpenQ16(const CellVolumeShare& share, uint64_t cellVolume) {
        return RemainingQ16(share.solid, cellVolume);
    }

    uint32_t GasOpenQ16(const CellVolumeShare& share, uint64_t cellVolume) {
        return RemainingQ16(share.solid + share.liquid, cellVolume);
    }

}  // namespace bicameral::sim
