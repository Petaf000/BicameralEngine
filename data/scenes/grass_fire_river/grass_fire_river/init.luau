-- init.luau — 草原の延焼の場面(T-0414)の表。分類 → 鍵 → 値(ADR-0031)。
-- 現実の元素・物質・反応・地球の環境は依存の標準パッケージ。ここは場面に要るものだけ:
--   足す = 地面の砂(SiO2 と元素 Si)・場面の組み立て(流れる区画も。T-0671)・材料・表示・遊びの指標。
--   書き直す = 現実の物質の熱伝導率(サンプルの世界と同じ試験の倍率。放射と気体の流れが無い間、伝導だけで燃え広がるように)。
return {
    elements = require("elements"),
    species = require("species"),
    world_cells = require("world_cells"),
    gas_regions = require("flow_regions").gas_regions,
    liquid_regions = require("flow_regions").liquid_regions,
    material_templates = require("material_templates"),
    view_quantities = require("view_quantities"),
    editor_defaults = require("editor_defaults"),
    play_metrics = require("play_metrics"),
}
