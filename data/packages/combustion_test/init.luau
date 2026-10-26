-- init.luau — 分類 → 鍵 → 値 の表を返す(ADR-0031)。元素・物質・規則は 1 つの鍵 = 1 つ。Mod は鍵を足して増やす。
-- 現実の元素・物質・反応・地球の環境(重力・音速の基準・基準の大気)は依存の標準パッケージが書く(package.luau)。
-- ここに書くのは遊びのための値だけ: 標準の値を書き直すもの(species の熱伝導率・基準の大気の地面の温度)と、足すもの(ほかの全部)。
local atmosphere = require("@earth_environment/atmosphere")
local earth: any = require("@earth_environment")

-- 基準の大気の地面(高さ 0)の点だけ 300 K に書き直す(**仮**・T-1164。ユーザー未確認): サンプルの世界の初めのセルは 300 K
-- (world_cells)。地球の ISA のまま(288.15 K)だと、熱の放射の段の外(環境の温度の物。D-671)より世界が 11.85 K 暖かく、
-- 何もしない世界も外へ冷え続けて眠らない。成分は地球の点のまま。取り消すなら上の 4 行と environment の鍵を消す
local ground = table.clone(earth.environment.atmosphere_profile[1])
ground.temperature_mk = 300000

return {
    environment = {
        atmosphere_profile = atmosphere.set_point(earth.environment.atmosphere_profile, ground),
    },
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
