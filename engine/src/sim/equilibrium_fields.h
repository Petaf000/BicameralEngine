// equilibrium_fields.h — 釣り合う家族の部品(C-0。sim/equilibrium・sim/equilibrium_block)の温めた始まり(前の刻みの答え)を、
// C-1 の登録した欄(温めた始まりの印 SF_FLAG_WARM_START。ADR-0399)に置く口(T-0922・ADR-0427。20 §10.4・§13.3)。
// 欄に置くと、ハッシュと保存点に入るか(D-608 の (A)。state_field.hlsli の SF_WARM_START_IN_STATE)・眠る時の平均・起きる時の配りが
// 印から決まる(法則が自分で控えを持たない)。部品の節は木のセル 1 つ(EquilibriumNode のレベルと座標)なので、節 → ブロック → セルで引く。
// データの流れ: 部品の答え(EquilibriumState::values・EquilibriumBlockState::values。節 × 値の数)→ StoreWarmStart → 欄の頁
//   → 保存点・ハッシュ(multires_state)→ LoadWarmStart → 次の解きの初め
// 浮動小数点は使わない(engine/src/sim は検査の対象)。
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "sim/equilibrium.h"
#include "sim/multires_nest.h"

namespace bicameral::sim {

    // 温めた始まりの欄を登録する(作った直後・根を置く前に)。SfMakeDerivedDesc(SF_PLACE_CELL, SF_TYPE_I64, phase, true, components)。
    // components は節の値の数(1〜SF_MAX_COMPONENTS。成分を持つ節の k)。断られたら MULTIRES_NO_FIELD。
    // discardOnSleep: 眠る時に平均ではなく 0 を残す(SF_FLAG_SLEEP_DISCARD。起きた時に使わない値。支えの刻みをまたぐ共役勾配の向き・残差。
    // ADR-0469 の 2-3・T-1159)
    [[nodiscard]] uint32_t RegisterWarmStartField(MultiresNest& nest, std::string name, uint32_t phase,
                                                  uint32_t components, bool discardOnSleep = false);

    // 節 i の値 values[i × perNode + c](c < perNode)を、節のセル(レベルと座標で引いた本物のブロックのセル)の欄の値
    // warm.component + c へ書く。書けなかった節の数を返す: 節のブロックが無い・一様(眠っている)・覆われたセルの節は書かない
    // (解くのは起きている所だけの約束。眠った所の値は畳んだ時の平均〔ADR-0399〕)
    uint32_t StoreWarmStart(const EquilibriumWarmStart& warm, std::span<const EquilibriumNode> nodes,
                            std::span<const int64_t> values, uint32_t perNode);

    // 欄から節の順に読む(節 × perNode)。無いブロック・覆われたセルの節は 0、一様なブロックは眠った時の平均(LoadNestField)
    [[nodiscard]] std::vector<int64_t> LoadWarmStart(const EquilibriumWarmStart& warm,
                                                     std::span<const EquilibriumNode> nodes, uint32_t perNode);

    // 欄の値の置き場の大きさ(セルの欄の頁と一様の値。疎なら頁の表も。保存点に入るバイト。T-0922 の測り)
    [[nodiscard]] uint64_t WarmStartFieldBytes(const MultiresNest& nest, uint32_t field);

}  // namespace bicameral::sim
