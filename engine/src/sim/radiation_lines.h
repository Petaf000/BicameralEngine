// radiation_lines.h — 放射の対を平行な線の束で見つける(T-0584・ADR-0262。docs/design/07-transport.md §1.3)。
//
// データの流れ:
//   セルごとの吸収率(Q16。CellAbsorptanceQ16)+ 刻み + 種
//   → 刻みの向きの組(radiation_directions.hlsli の 1 組 = 半球を等しい立体角に分けた 32 本。R6 のハッシュで組を選ぶ)
//   → 向きごとに、平行な線の束(一番大きい軸の面の上で間隔 h = 2 セル + 2^-16 の格子。格子のずれも R6 のハッシュ)
//   → 線ごとに整数の DDA(板ごとに、ほかの 2 軸の境を越える順を掛け算で比べる)で通るセルを順に見る
//   → 吸収率が 0 でないセルどうしの対(途中の透過 Π(1 − α) を掛ける)→ RadiationPair(見え方の割合・分け前)
//   → ExchangeRadiation(radiation_reference.h。対ごとに 1 つの整数)
//
// 約束:
//   - 対は線が 1 回だけ見つける(両向きを 1 本の線で扱う)。対の流れは 1 つの整数なので保存はビット単位。温度を見ないので順番によらない。
//   - 受け手の分け前の保証: 1 つの向きの中で、どのセルも高々 1 本の線しか通らない(線の間隔 h > 板の中で線が掃く幅 1 + |D_b/D_a| ≤ 2)。
//     1 本の線の上で、セル a の片側の対の分け前の和 ≤ α_a × Σ α_b τ_ab ≤ 1(透過の和は 1 を超えない)。両側で 2、向き 32 本で 64。
//     対の分け前 = α_a α_b τ_ab ÷ (2 × 本数)なので、どのセルも分け前の合計 ≤ 1(T-0583 の RdPairLimit の前提)。
//   - 完全に遮るセル(α = 1)の向こうへは届かない(陰はちょうど 0)。透過が 1/16 未満か 16 m を越えたら、その先の対は作らない。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "sim/radiation_reference.h"

namespace bicameral::sim {

    // 一様な格子(セルの番号 = (z × sizeY + y) × sizeX + x。仮の世界の ProbeCellIndex と同じ並び)
    struct RadiationGrid {
        uint32_t sizeX = 0;
        uint32_t sizeY = 0;
        uint32_t sizeZ = 0;
    };

    // 刻み tick の対を作る。absorptanceQ16 はセルごとの吸収率(格子のセルの数)。seed は世界の種(向きの組と格子のずれを決める)
    [[nodiscard]] std::vector<RadiationPair> FindRadiationPairs(RadiationGrid grid,
                                                                std::span<const uint32_t> absorptanceQ16, uint64_t tick,
                                                                uint64_t seed);

    // 1 刻みに使う向きの数(1 組の本数)
    [[nodiscard]] uint32_t RadiationDirectionsPerTick();

}  // namespace bicameral::sim
