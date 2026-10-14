-- play_metrics.luau — 場面の遊びの指標(T-0432)。場面は world_cells の初めの世界。1 秒 = 60 刻み。
-- 火は手前の岸の x = 4・z = 32 の草につける。草は y = 4 の 1 層: 手前の岸 x 0〜39・向こう岸 x 44〜63(川は x 40〜43)。
-- 量は岸ごとの範囲(min_cell・size_cells。T-0559)で測る:
--   手前の草 40 × 64 セル = 3,072,000,000 µmol・向こう岸の草 20 × 64 セル = 1,536,000,000 µmol。
local IGNITE = { { tick = 0, cell = { 4, 4, 32 } } }
local NEAR_BANK = { min_cell = { 0, 4, 0 }, size_cells = { 40, 1, 64 } }
local FAR_BANK = { min_cell = { 44, 4, 0 }, size_cells = { 20, 1, 64 } }

return {
    -- 手前の草の 9 割が燃えるまで(= 火が川に着いた目安)。延焼の速さ ≒ 18 m(火の点から川まで 36 セル)÷ この時間
    grass_fire_reaches_river = {
        label = "草原の火が川に着くまで",
        max_ticks = 36000,
        sample_interval_ticks = 60,
        pokes = IGNITE,
        quantity = "species_amount",
        species = "cellulose",
        min_cell = NEAR_BANK.min_cell,
        size_cells = NEAR_BANK.size_cells,
        at_most = 307200000,
    },
    -- 10 分後に残る向こう岸の草(1,536,000,000 µmol が全部残れば川で止まった)
    far_grass_left_after_10_min = {
        label = "10 分後に残る向こう岸の草(全部残れば川で止まった)",
        max_ticks = 36000,
        sample_interval_ticks = 600,
        pokes = IGNITE,
        quantity = "species_amount",
        species = "cellulose",
        min_cell = FAR_BANK.min_cell,
        size_cells = FAR_BANK.size_cells,
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
