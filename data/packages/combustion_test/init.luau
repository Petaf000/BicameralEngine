-- init.luau — 分類 → 鍵 → 値 の表を返す(ADR-0031)。元素・物質・規則は 1 つの鍵 = 1 つ。Mod は鍵を足して増やす。
-- 現実の元素・物質・反応・地球の環境(重力・音速の基準・基準の大気)は依存の標準パッケージが書く(package.luau)。
-- ここに書くのは遊びのための値だけ: 標準の値を書き直すもの(species の熱伝導率)と、足すもの(ほかの全部)。
return {
    elements = require("elements"), -- 仮の魔素の元素だけ(現実の元素は real_elements)
    species = require("species"), -- 現実の物質の熱伝導率を書き直したもの + 仮の魔素
    reactions = require("reactions"), -- 仮の魔素の規則だけ(現実の反応は real_reactions)
    world_cells = require("world_cells"), -- サンプルの世界の組み立て(T-0398)
    world_bodies = require("world_bodies"),
    material_templates = require("material_templates"),
    view_quantities = require("view_quantities"),
    editor_defaults = require("editor_defaults"),
    play_metrics = require("play_metrics"), -- 遊びの指標(T-0432)
}
