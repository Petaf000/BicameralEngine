-- package.luau — ⑤ の最初の場面「地面 + 火 → 草原の延焼が川で止まる」の最小の版(T-0414・D-471 ⑤・D-486)。
-- 作り手と同じやり方(Luau のパッケージとエディタの道具)だけで作った場面で、エンジンの C++ は変えていない。
-- 値は見本の値(現実の値 + サンプルの世界と同じ熱伝導率の試験の倍率)で、ゲームの中身ではない(D-006)。
--
-- 起動(exe の横から): bicameral --packages data/scenes/grass_fire_river --editor --no-physics
--   測る:            bicameral --packages data/scenes/grass_fire_river --measure-metrics --no-physics
--                      --metrics-baseline data/scenes/grass_fire_river/metrics_baseline.txt
-- game = true でこの場面がゲーム本体になる(名前に依らない。T-0557・ADR-0250)。標準パッケージの値(熱伝導率)を書き直せるのは
-- ゲーム本体だけ(Mod は足すだけ。ADR-0183)。ブランクの世界(既定)でも game = true があればゲーム本体として読む。
return {
    format = 1,
    title = "草原の延焼が川で止まる(⑤ の最小の版)",
    depends = { "real_elements", "real_matter", "real_reactions", "earth_environment" },
    entry = "init",
    game = true,
}
