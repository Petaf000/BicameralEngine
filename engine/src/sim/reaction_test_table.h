// reaction_test_table.h — 原理の確認に使う試験用の反応表(T-0014)。公開側に置く試験データで、ゲームの中身ではない(D-006)。
// 物質 7 つ(セルロース・炭・O2・N2・CO2・CO・H2O(気))と規則 5 本(熱分解・木の燃焼・炭の燃焼・不完全燃焼・Boudouard)に、
// 触るための仮の魔素 mana_test(気体 1 種と、魔素を触媒にする規則 2 本。試験用の仮の値。T-0225・D-449・D-451)。
// 値は文献値を丸めたもの(生成エンタルピーは 298.15 K、比熱は一定)。速度の値は燃え方が見える程度に置いた仮の値。
// 同じ中身を Luau のパッケージでも書いてある(data/packages/combustion_test。T-0021。ランタイムは T-0157 からそちらを読む)。ベイクするとビットで同じ表になる
// (reaction_package_test)。片方を変えたらもう片方も変える。
#pragma once

#include "sim/reaction_table.h"

namespace bicameral::sim {

    [[nodiscard]] ReactionTableDefinition MakeCombustionTestTable();

}  // namespace bicameral::sim
