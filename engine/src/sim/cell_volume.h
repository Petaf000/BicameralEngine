// cell_volume.h — 1 セルの体積を相で分け合う(T-0593・T-0612・ADR-0268 Proposed。07 §2.8)。
//
// 一つの芯: セルの体積 = 固体が占める分 + 液体が占める分 + 残り(気体)。占める分は物質の密度から(モル体積 = モル質量 ÷ 密度。
// BakedReactionTable::speciesMolarVolume。T-0609 の density_kg_per_m3)。気体と液体の流れは、この「空き」だけで決まる:
//   - 液体の空き(固体が占めない割合)= 液体が入れる量・液体が面を通れる割合(sim/probe_liquid)
//   - 気体の空き(固体と液体が占めない割合)= 気体の圧力の体積(p = nRT ÷ 空きの体積)・気体が面を通れる割合(sim/probe_gas → gas_reference)
//   - 面の開き = 両側のセルの空きの小さい方(仮。孔の形〔透水の係数〕は読まない。T-0616)
// 密度を書いていない物質は場所を取らない(体積 0。今までの気体と同じ。仮・ユーザー未確認。T-0593 の判断待ち 1)。
// 気体の物質は書いた密度があっても場所を取らない(圧力で体積が決まる)。
// 単位: 体積は pL(= µmol × mm³/mol = 10^-15 m³)、割合は Q16(CELL_OPEN_FULL = 全部)。浮動小数点は使わない(04 §4)。
#pragma once

#include <cstdint>

#include "common/reaction.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 割合の 1(Q16)
    inline constexpr uint32_t CELL_OPEN_FULL = 1u << 16;

    // 仮の世界のセル(0.5 m 角)の体積(pL。0.125 m³)
    inline constexpr uint64_t PROBE_CELL_VOLUME_PICOLITERS = 500ull * 500 * 500 * 1000000;

    // 1 セルの凝縮相が占める体積(pL)
    struct CellVolumeShare {
        uint64_t solid = 0;   // 液体でない凝縮相(固体)
        uint64_t liquid = 0;  // 液体(phase = "liquid")
    };

    // セルの成分から占める体積を数える(気体の物質と、密度を書いていない物質は 0)
    [[nodiscard]] CellVolumeShare ShareCellVolume(const BakedReactionTable& table, const reaction::RxCell& cell);

    // 液体の空き: 固体が占めない割合(Q16。0 = 固体で満ちて液体は入れない)
    [[nodiscard]] uint32_t LiquidOpenQ16(const CellVolumeShare& share, uint64_t cellVolume);

    // 気体の空き: 固体と液体が占めない割合(Q16。0 = 気体の入る所が無い)
    [[nodiscard]] uint32_t GasOpenQ16(const CellVolumeShare& share, uint64_t cellVolume);

    // 面の開き(Q16): 両側の空きの小さい方
    [[nodiscard]] constexpr uint32_t FaceOpenQ16(uint32_t a, uint32_t b) {
        return a < b ? a : b;
    }

}  // namespace bicameral::sim
