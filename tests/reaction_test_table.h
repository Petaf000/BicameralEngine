// reaction_test_table.h — テストが使う試験の反応表(T-0169)。中身は試験のパッケージ data/packages/combustion_test(ランタイムが
// 既定で読むゲーム本体と同じもの。T-0157・ADR-0033)だけに書き、C++ には写しを置かない(前は sim/reaction_test_table に手で写していた)。
// 公開側に置く試験データで、ゲームの中身ではない(D-006)。
//
// データの流れ: リポジトリの data/packages/combustion_test(BICAMERAL_DATA_PACKAGES_DIR)+ それが依存する標準パッケージ
//   (data/standard の real_elements・real_matter・real_reactions・earth_environment。T-0498)→ ReadPackageFolder → LoadPackages(Luau の殻)
//   → ReadReactionTableDefinition → 定義(呼び手が値を変えてから sim::BakeReactionTable に渡せる)。
// 定義の並びは名前のバイト順(パッケージの鍵の順)。ベイクも名前の順に並べ直すので、ベイクした表は並びに依らない(ADR-0032)。
// 読めなければ理由を表示して終了コード 1 で止まる(試験の表が無いテストは何も確かめられない)。
// 試験の中で作る特別な表(上限の表 reaction_limits_table.h・溢れの表・下の MakeSpeciesChangedTestTable)は C++ のまま。
#pragma once

#include <vector>

#include "script/luau_package.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    // 試験のパッケージと、それが依存する標準パッケージ(名前は combustion_test が先頭。T-0498)。読めなければ止まる
    [[nodiscard]] std::vector<script::PackageSource> ReadCombustionTestPackages();

    // 試験のパッケージの表の定義(元素 6・物質 9〔標準パッケージの現実の元素・物質と、仮の魔素 mana_test。T-0225・T-0498〕・規則 7)
    [[nodiscard]] ReactionTableDefinition MakeCombustionTestTable();

    // 物質を消した定義から、消えた物質を指す材料のひな形・抽出の枠・既定を除く(ベイクが知らない物質で落ちないように。
    // T-0399・T-0398 の共有の決め事 4)。試験の中で物質を消したら呼ぶ
    void PruneMaterialTemplates(ReactionTableDefinition& definition);

    // 物質を足す・消す差し替えの試験用(T-0223・T-0242): 試験の表に水素(H2)とオゾン(O3)を足し、セルロースとそれを使う規則を消した表
    // (物質の数が 8 → 9 に変わる。仮の魔素は残る)。水素・オゾンの後ろの物質の ID がずれ、セルロースは炭・水素・酸素の単体に分かれる
    // (ADR-0065)。試験のパッケージそのものは変えない
    [[nodiscard]] ReactionTableDefinition MakeSpeciesChangedTestTable();

    // 魔法の転換の試験用(T-0359): 試験の表に、二酸化炭素と同じ元素の仮の物質(carbon_dioxide_isomer_test。異性体の役)と
    // 炭素 2 原子の仮の物質(dicarbon_test。係数が変わる転換の役)を足した表。値はどちらも試験用の仮のもの(D-006)
    [[nodiscard]] ReactionTableDefinition MakeConversionTestTable();

    // 物質を足す・消す差し替えを跨ぐ転換の試験用(T-0377)。値はどちらも試験用の仮のもの(D-006):
    //   - 前の表 = MakeConversionTestTable に、二酸化炭素と同じ元素の仮の物質をもう 1 つ(carbon_dioxide_late_test。消える転換の
    //     替える先の役。名前の順で異性体の後ろ)足した表
    //   - 後の表 = MakeConversionTestTable に、名前の順で一番前の仮の物質(a_marker_test。CO と同じ元素)を足した表(late は無い)。
    //     異性体の ID は 1 つ後ろへずれ、前の表の異性体の ID は後の表では二酸化炭素を指す
    [[nodiscard]] ReactionTableDefinition MakeConversionRemapBeforeTestTable();
    [[nodiscard]] ReactionTableDefinition MakeConversionRemapAfterTestTable();

}  // namespace bicameral::sim
