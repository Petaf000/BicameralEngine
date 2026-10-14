// radiation_reference.h — 放射の熱の交換の CPU リファレンス(T-0583・ADR-0252。docs/design/07-transport.md §1.3)。
//
// 本体の式は shaders/common/radiation.hlsli(HLSL と共通。後の GPU〔T-0586〕も同じ関数を呼ぶ)。ここはそれを、
// 刻みの初めの面(セル)の状態と、交換する対の一覧に当てはめて、面ごとのエネルギーの変化を出す。
//
// データの流れ:
//   ベイクした表の物質の a(BakedReactionTable::speciesRadiationAbsorption。パッケージの radiation_absorption_mm2_per_mol)
//   + セルの成分 → CellAbsorptanceQ16(吸収率 = 放射率)
//   → RadiationSurface(温度・熱容量・吸収率)と RadiationPair(どの 2 つが・どれだけ見えるか・上限の分け前)
//   → ExchangeRadiation(対ごとに 1 つの整数の流れ。first から引き、second に足す)→ 面ごとの変化(mJ)
// 対を光線で見つけるのは T-0584(研究)。ここでは呼ぶ側が対を渡す(試験は解析解の見え方の割合を渡す)。
// 決めごと: 温度は刻みの初め(Jacobi 型)なので、対の並びに結果が依存しない(整数の和は順番によらない)。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "common/radiation.hlsli"
#include "common/reaction.hlsli"

namespace bicameral::sim {

    struct BakedReactionTable;

    // 放射を交わす面(セル)の刻みの初めの状態
    struct RadiationSurface {
        uint32_t temperatureMk = 0;
        uint64_t heatCapacityNanojoulesPerKelvin = 0;  // 行き過ぎの上限に使う
        uint32_t absorptanceQ16 = 0;                   // 吸収率 = 放射率(0〜65536)
    };

    // 交換する 2 つの面
    struct RadiationPair {
        uint32_t first = 0;     // 面の添字
        uint32_t second = 0;    // 面の添字
        uint32_t viewQ16 = 0;   // 見え方の割合 F(途中の透過も掛けたもの。0〜65536)
        uint32_t shareQ16 = 0;  // 行き過ぎの上限の分け前(1 つの面の分け前の合計 ≤ 65536 にする)
    };

    // セルの吸収率(Q16)。a を書いていない物質(0)は数えない。表に無い物質 ID も数えない
    [[nodiscard]] uint32_t CellAbsorptanceQ16(const BakedReactionTable& baked, const reaction::RxCell& cell);

    // 1 対の流れ(first → second が正、mJ)。first と second を入れ替えると符号だけが反転する(ビット単位)
    [[nodiscard]] int64_t RadiationPairFlow(const RadiationSurface& first, const RadiationSurface& second,
                                            const RadiationPair& pair);

    // すべての対の流れを足し引きした、面ごとのエネルギーの変化(mJ。添字 = 面)。変化の合計は 0(ビット単位)
    [[nodiscard]] std::vector<int64_t> ExchangeRadiation(std::span<const RadiationSurface> surfaces,
                                                         std::span<const RadiationPair> pairs);

}  // namespace bicameral::sim
