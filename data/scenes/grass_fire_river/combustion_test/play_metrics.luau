-- play_metrics.luau — 場面の遊びの指標(T-0432)。場面は world_cells の初めの世界。1 秒 = 60 刻み。
-- 火は手前の岸の x = 4・z = 32 の草につける。指標は世界全体の量しか測れない(範囲を書けない)ので、
-- 「手前の草が燃えたか」「向こう岸の草が残ったか」は草(cellulose)の世界全体の量で読む(作り心地の記録 4):
--   草の量: 手前 40 × 64 セル = 3,072,000,000 µmol・向こう岸 20 × 64 セル = 1,536,000,000 µmol・合わせて 4,608,000,000 µmol。
local IGNITE = { { tick = 0, cell = { 4, 4, 32 } } }
local FAR_GRASS = 1536000000

return {
    -- 手前の草の 9 割が燃えるまで(= 火が川に着いた目安)。延焼の速さ ≒ 18 m(火の点から川まで 36 セル)÷ この時間
    grass_fire_reaches_river = {
        label = "草原の火が川に着くまで",
        max_ticks = 36000,
        sample_interval_ticks = 60,
        pokes = IGNITE,
        quantity = "species_amount",
        species = "cellulose",
        at_most = FAR_GRASS + 307200000,
    },
    -- 10 分後に残る草(向こう岸の 1,536,000,000 µmol が残れば川で止まった)
    grass_left_after_10_min = {
        label = "10 分後に残る草(向こう岸が残れば川で止まった)",
        max_ticks = 36000,
        sample_interval_ticks = 600,
        pokes = IGNITE,
        quantity = "species_amount",
        species = "cellulose",
    },
    -- 火(1000 K 以上のセル)が無くなるまで
    grass_fire_out = {
        label = "草原の火が消えるまで",
        max_ticks = 36000,
        sample_interval_ticks = 60,
        pokes = IGNITE,
        quantity = "cells_at_or_above",
        temperature_mk = 1000000,
        at_most = 0,
    },
}
