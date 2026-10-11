// multires_wide_cell.h — 多重解像度の世界の、成分の数に上限が無いセルと端数(CPU リファレンス。T-0187・02 §3・17)。
//
// 粗くする(multires.hlsli の MrCoarsenCellOf)は結果の形と子の形のテンプレート。ここに上限の無い形(MrWideFraction・
// MrWideChildren・MrWideCoarsened)と、核が呼ぶ形ごとの道具の版を置く(同じ名前空間なので引数依存の名前探索で見つかる)。
// セルは reaction_wide_cell.h の RxWideCell。世界(MultiresNest)の中では「インライン 8(RxCell・MrFraction)+ 頁ごとの溢れ」に
// 分けて持つ(multires_nest.h の MultiresOverflowArea)。読み書きは LoadWideNestCell などで、いつもこの形で受け渡す。
// 成分が RX_MAX_CELL_SPECIES 以下の間は、インラインの形で粗くした結果とビット単位で同じになる。
#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <vector>

#include "common/multires.hlsli"
#include "sim/reaction_wide_cell.h"

namespace bicameral::multires {

    // 端数(MrFraction の上限なし版。約束は同じ: 物質 ID の昇順・0 は持たない・speciesCount 個より後ろは読まない)
    struct MrWideFraction {
        uint64_t energy = 0;
        uint32_t speciesCount = 0;
        std::vector<uint32_t> species;
        std::vector<uint64_t> amounts;
    };

    // 親のセル 1 つの子 2³(粗くする入力)
    struct MrWideChildren {
        std::array<reaction::RxWideCell, MR_CHILDREN_PER_CELL> cells;
        std::array<MrWideFraction, MR_CHILDREN_PER_CELL> fractions;
    };

    // 粗くした結果(MrCoarsened の上限なし版。溢れ〔overflowCount〕は起きない)
    struct MrWideCoarsened {
        reaction::RxWideCell cell;
        MrWideFraction fraction;
        uint32_t lostCount = 0;
        uint32_t overflowCount = 0;

        uint32_t energyLostBits = 0;
        uint32_t lostSpeciesCount = 0;
        std::vector<uint32_t> lostSpecies;
        std::vector<uint32_t> lostBits;
    };

    // 影の子 2³(MrShadowFamily の上限なし版。引き戻し MrPullBackShadowOf の入力と出力。T-0199)
    struct MrWideShadowFamily {
        std::array<reaction::RxWideCell, MR_CHILDREN_PER_CELL> cells;
        uint32_t clamped = 0;
    };

    // ほぼ同じ頁を畳む集計(MrFoldStats の上限なし版。tooManySpecies は起きない。T-0199)。初めの値は MrMakeFoldStats と同じ
    struct MrWideFoldStats {
        // --- セル ---
        uint32_t cellCount = 0;
        uint32_t tooManySpecies = 0;
        int32_t lowTemperature = INT32_MAX;  // mK
        int32_t highTemperature = INT32_MIN;
        uint64_t highTotal = 0;
        MrWide energy = MrMakeWide();

        // --- 成分(物質 ID の昇順)---
        uint32_t speciesCount = 0;
        std::vector<uint32_t> species;
        std::vector<uint32_t> present;
        std::vector<uint64_t> lowAmount;
        std::vector<uint64_t> highAmount;
        std::vector<uint64_t> sumHigh;
        std::vector<uint64_t> sumLow;
    };

    // 平均の値と切り捨ての余り(MrFoldValue の上限なし版。余りの並びは集計の species と同じ)
    struct MrWideFoldValue {
        reaction::RxWideCell cell;
        uint64_t energyRemainder = 0;
        std::vector<uint64_t> amountRemainders;
    };

    // --- 畳む集計が呼ぶ、形ごとの道具(multires.hlsli の「集計の形ごとの道具」の上限なし版)---

    inline bool MrFoldStatsHasRoom(const MrWideFoldStats& /*stats*/) {
        return true;
    }

    inline MrWideFoldStats MrFoldStatsWithRoom(MrWideFoldStats stats) {
        const size_t wanted = static_cast<size_t>(stats.speciesCount) + 1;
        if (stats.species.size() < wanted) {
            stats.species.resize(wanted, 0);
            stats.present.resize(wanted, 0);
            stats.lowAmount.resize(wanted, 0);
            stats.highAmount.resize(wanted, 0);
            stats.sumHigh.resize(wanted, 0);
            stats.sumLow.resize(wanted, 0);
        }

        return stats;
    }

    inline MrWideFoldValue MrFoldValueWithRoom(MrWideFoldValue value, uint32_t count) {
        value.amountRemainders.assign(count, 0);

        return value;
    }

    // --- 粗くする核が呼ぶ、形ごとの道具(multires.hlsli の「粗くした結果の形ごとの道具」の上限なし版)---

    inline bool MrFractionHasRoom(const MrWideFraction& /*fraction*/) {
        return true;
    }

    inline MrWideFraction MrFractionWithRoom(MrWideFraction fraction) {
        const size_t wanted = static_cast<size_t>(fraction.speciesCount) + 1;
        if (fraction.species.size() < wanted) {
            fraction.species.resize(wanted, 0);
            fraction.amounts.resize(wanted, 0);
        }

        return fraction;
    }

    inline MrWideFraction MrEmptyFractionLike(const MrWideFraction& /*fraction*/) {
        return {};
    }

    inline bool MrLostHasRoom(const MrWideCoarsened& /*result*/) {
        return true;
    }

    inline MrWideCoarsened MrWithLostRoom(MrWideCoarsened result) {
        const size_t wanted = static_cast<size_t>(result.lostSpeciesCount) + 1;
        if (result.lostSpecies.size() < wanted) {
            result.lostSpecies.resize(wanted, 0);
            result.lostBits.resize(wanted, 0);
        }

        return result;
    }

    inline uint32_t MrUnionStepLimit(const MrWideChildren& children) {
        uint32_t total = 1;
        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
            total += children.cells[j].speciesCount + children.fractions[j].speciesCount;

        return total;
    }

    // 子 2³ を親のセル 1 つにまとめる(上限なし)
    inline MrWideCoarsened MrCoarsenWideCell(const MrWideChildren& children) {
        return MrCoarsenCellOf(children, MrWideCoarsened{});
    }

    // --- 行き来と比べる ---

    inline bool MrFractionIsZero(const MrWideFraction& fraction) {
        return fraction.energy == 0 && fraction.speciesCount == 0;
    }

    inline MrWideFraction MrWidenFraction(const MrFraction& fraction) {
        MrWideFraction wide;
        wide.energy = fraction.energy;
        wide.speciesCount = fraction.speciesCount;
        wide.species.assign(fraction.species, fraction.species + fraction.speciesCount);
        wide.amounts.assign(fraction.amounts, fraction.amounts + fraction.speciesCount);

        return wide;
    }

}  // namespace bicameral::multires
