// cell_view.h — セルの見立て(shaders/common/cell_view.hlsli の MakeCellView。T-0713・ADR-0333 Proposed)を CPU で呼ぶための表。
//
// データの流れ: ベイクの表(BakedReactionTable)→ 物質ごとの相の表(MakeSpeciesPhases。表を差し替えた時だけ作る)
//   → CellViewTable(反応の表の読み方 + 相の表)→ MakeCellView(セル, レベル, 表, 基準の圧力)→ 流れが読む値(CvCellView。永続しない)
// 相の表の作り方はここ 1 か所(ベイクの相の表 speciesPhase〔T-0710〕を写す。値の並びは同じ)。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "common/cell_view.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 物質ごとの相の値(添字 = 物質 ID。0 は使わない)。凝縮相で液体 → 液体・凝縮相で液体でない → 固体・それ以外 → 気体
    [[nodiscard]] std::vector<multires::CvSpeciesPhase> MakeSpeciesPhases(const BakedReactionTable& table);

    // MakeCellView に渡す表(reaction.hlsli の Table の約束 + Phase)
    struct CellViewTable {
        ReactionTableView reaction;
        std::span<const multires::CvSpeciesPhase> phases;

        [[nodiscard]] reaction::RxSpecies Species(uint32_t id) const { return reaction.Species(id); }
        [[nodiscard]] multires::CvSpeciesPhase Phase(uint32_t id) const { return phases[id]; }
    };

}  // namespace bicameral::sim
