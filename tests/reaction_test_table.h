// reaction_test_table.h — テストが使う試験の反応表(T-0169)。中身は試験のパッケージ data/packages/combustion_test(ランタイムが
// 既定で読むゲーム本体と同じもの。T-0157・ADR-0033)だけに書き、C++ には写しを置かない(前は sim/reaction_test_table に手で写していた)。
// 公開側に置く試験データで、ゲームの中身ではない(D-006)。
//
// データの流れ: リポジトリの data/packages/combustion_test(BICAMERAL_DATA_PACKAGES_DIR)→ ReadPackageFolder → LoadPackages(Luau の殻)
//   → ReadReactionTableDefinition → 定義(呼び手が値を変えてから sim::BakeReactionTable に渡せる)。
// 定義の並びは名前のバイト順(パッケージの鍵の順)。ベイクも名前の順に並べ直すので、ベイクした表は並びに依らない(ADR-0032)。
// 読めなければ理由を表示して終了コード 1 で止まる(試験の表が無いテストは何も確かめられない)。
// 試験の中で作る特別な表(上限の表 reaction_limits_table.h・溢れの表・下の MakeSpeciesChangedTestTable)は C++ のまま。
#pragma once

#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 試験のパッケージの表の定義(元素 5・物質 8〔仮の魔素 mana_test を含む。T-0225〕・規則 7)
    [[nodiscard]] ReactionTableDefinition MakeCombustionTestTable();

    // 物質を消した定義から、消えた物質を指す材料のひな形・抽出の枠・既定を除く(ベイクが知らない物質で落ちないように。
    // T-0399・T-0398 の共有の決め事 4)。試験の中で物質を消したら呼ぶ
    void PruneMaterialTemplates(ReactionTableDefinition& definition);

    // 物質を足す・消す差し替えの試験用(T-0223・T-0242): 試験の表に水素(H2)とオゾン(O3)を足し、セルロースとそれを使う規則を消した表
    // (物質の数が 8 → 9 に変わる。仮の魔素は残る)。水素・オゾンの後ろの物質の ID がずれ、セルロースは炭・水素・酸素の単体に分かれる
    // (ADR-0065)。試験のパッケージそのものは変えない
    [[nodiscard]] ReactionTableDefinition MakeSpeciesChangedTestTable();

}  // namespace bicameral::sim
