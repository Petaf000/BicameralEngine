// coupling_rows.h — 反応表のパネルの「結合」の欄: ベイクした流れどうしの結合(分類 drags・machines。T-0536・ADR-0238)の一覧。
//
// データの流れ: 今の表(script::LoadedReactionTable → sim::BakedReactionTable::couplings)→ ここで 1 行ずつ表に出す(見るだけ)。
// 書いた q とベイクが押さえた q(|q| > 1 は ±1)・効率の上限 η_max(q)・押す構造の費用と速さ・止まる坂を並べる。
// 値の書き換えはパッケージのファイルで(ホットリロードが読み直す)。反応表のパネル(editor/reaction_table_panel)のタブから呼ぶ。
// 並走の T-0548(エディタの表の差し替え)とぶつからないよう、パネル本体には呼び出しの数行だけを足した。
#pragma once

#include "sim/reaction_table.h"

namespace bicameral::editor {

    // 結合の一覧の表(ImGui のウィンドウの中で呼ぶ)
    void BuildCouplingRows(const sim::BakedReactionTable& table);

}  // namespace bicameral::editor
