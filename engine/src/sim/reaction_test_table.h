// reaction_test_table.h — 原理の確認に使う試験用の反応表(T-0014)。公開側に置く試験データで、ゲームの中身ではない(D-006)。
// 物質 7 つ(セルロース・炭・O2・N2・CO2・CO・H2O(気))と規則 5 本(熱分解・木の燃焼・炭の燃焼・不完全燃焼・Boudouard)。
// 値は文献値を丸めたもの(生成エンタルピーは 298.15 K、比熱は一定)。速度の値は燃え方が見える程度に置いた仮の値。
#pragma once

#include "sim/reaction_table.h"

namespace bicameral::sim {

    [[nodiscard]] ReactionTableDefinition MakeCombustionTestTable();

}  // namespace bicameral::sim
