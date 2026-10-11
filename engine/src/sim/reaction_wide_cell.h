// reaction_wide_cell.h — 成分の数に上限が無いセル(CPU リファレンス。T-0175・D-401・D-418・02 §3)。
//
// 反応の核(shaders/common/reaction.hlsli)はセルの形を Cell 型で受け取る。GPU と今の多重解像度の世界は RxCell
// (インラインの RX_MAX_CELL_SPECIES 個まで)、ここの RxWideCell は成分を std::vector に持ち、何種でも足せる。
// 核が形ごとに呼ぶ道具(RxHasRoomForSpecies・RxCellWithRoom・RxEmptyCellLike・RxEmptyUsage)の RxWideCell 版を
// 同じ名前空間に置くので、核のテンプレートは引数依存の名前探索でこちらを見つける。
//
// 約束は RxCell と同じ: 成分は物質 ID の昇順・物質量が 0 の成分は無い・speciesCount 個より後ろの要素は読まない。
// 成分が RX_MAX_CELL_SPECIES 以下の間は、RxCell で刻んだ結果とビット単位で同じになる(RxWidenCell / RxNarrowCell で行き来できる)。
// GPU の頁ごとの溢れ領域(インライン K + 溢れ)は T-0176 で、このセルと毎刻みビット一致させる。
#pragma once

#include <cstdint>
#include <vector>

#include "common/reaction.hlsli"

namespace bicameral::reaction {

    struct RxWideCell {
        int64_t energy = 0;  // mJ(熱 + 化学)
        uint32_t speciesCount = 0;
        std::vector<uint32_t> species;  // 大きさは speciesCount 以上
        std::vector<uint64_t> amounts;  // µmol
    };

    // 候補が使う量(RxUsage の上限なし版。添字 = 成分の位置)
    struct RxWideUsage {
        std::vector<uint64_t> amounts;
        uint64_t heat = 0;
    };

    using RxWideWaitStep = RxWaitStepOf<RxWideCell>;

    // --- 核が呼ぶ、セルの形ごとの道具(reaction.hlsli の「セルの形ごとの道具」の RxWideCell 版)---

    inline bool RxHasRoomForSpecies(const RxWideCell& /*cell*/) {
        return true;
    }

    inline RxWideCell RxCellWithRoom(RxWideCell cell) {
        const size_t wanted = static_cast<size_t>(cell.speciesCount) + 1;
        if (cell.species.size() < wanted) {
            cell.species.resize(wanted, 0);
            cell.amounts.resize(wanted, 0);
        }

        return cell;
    }

    inline RxWideCell RxEmptyCellLike(const RxWideCell& /*cell*/, int64_t energy) {
        return {.energy = energy, .speciesCount = 0, .species = {}, .amounts = {}};
    }

    inline RxWideUsage RxEmptyUsage(const RxWideCell& cell) {
        return {.amounts = std::vector<uint64_t>(cell.speciesCount, 0), .heat = 0};
    }

    // --- 行き来と比べる ---

    inline RxWideCell RxMakeEmptyWideCell(int64_t energy) {
        return {.energy = energy, .speciesCount = 0, .species = {}, .amounts = {}};
    }

    inline RxWideCell RxWidenCell(const RxCell& cell) {
        RxWideCell wide = RxMakeEmptyWideCell(cell.energy);
        wide.speciesCount = cell.speciesCount;
        wide.species.assign(cell.species, cell.species + cell.speciesCount);
        wide.amounts.assign(cell.amounts, cell.amounts + cell.speciesCount);

        return wide;
    }

    // インラインに入りきるか(RxNarrowCell できるか)
    inline bool RxFitsInline(const RxWideCell& cell) {
        return cell.speciesCount <= RX_MAX_CELL_SPECIES;
    }

    // インラインの形にする(入りきる時だけ呼ぶ。使わない枠は 0 = RxMakeEmptyCell と同じ)
    inline RxCell RxNarrowCell(const RxWideCell& cell) {
        FX_ASSERT(RxFitsInline(cell));
        RxCell narrow = RxMakeEmptyCell(cell.energy);
        narrow.speciesCount = cell.speciesCount;
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            narrow.species[i] = cell.species[i];
            narrow.amounts[i] = cell.amounts[i];
        }

        return narrow;
    }

    // 2 つのセルが同じか(speciesCount 個より後ろは見ない)
    inline bool RxSameCell(const RxWideCell& a, const RxWideCell& b) {
        if (a.energy != b.energy || a.speciesCount != b.speciesCount)
            return false;

        for (uint32_t i = 0; i < a.speciesCount; ++i) {
            if (a.species[i] != b.species[i] || a.amounts[i] != b.amounts[i])
                return false;
        }

        return true;
    }

}  // namespace bicameral::reaction
