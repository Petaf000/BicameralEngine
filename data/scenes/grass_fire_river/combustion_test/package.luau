-- package.luau — ⑤ の最初の場面「地面 + 火 → 草原の延焼が川で止まる」の最小の版(T-0414・D-471 ⑤・D-486)。
-- 作り手と同じやり方(Luau のパッケージとエディタの道具)だけで作った場面で、エンジンの C++ は変えていない。
-- 値は見本の値(現実の値 + サンプルの世界と同じ熱伝導率の試験の倍率)で、ゲームの中身ではない(D-006)。
--
-- 起動(exe の横から): bicameral --world sample --packages data/scenes/grass_fire_river --editor
--   測る:            bicameral --world sample --packages data/scenes/grass_fire_river --measure-metrics --no-physics
-- **フォルダの名前が combustion_test なのは回避**: 標準パッケージの値を書き直せるのはゲーム本体だけで(Mod は足すだけ。ADR-0183)、
-- ゲーム本体の名前はエンジンが combustion_test に決め打ちしている(script::DEFAULT_BASE_PACKAGE)。場面を自分の名前の
-- ゲーム本体として読ませる道が無い(作り心地の記録 1。T-0414 のチケット)。
return {
    format = 1,
    title = "草原の延焼が川で止まる(⑤ の最小の版)",
    depends = { "real_elements", "real_matter", "real_reactions", "earth_environment" },
    entry = "init",
}
