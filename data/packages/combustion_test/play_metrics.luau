-- play_metrics.luau — サンプルの世界の遊びの指標(T-0432・ADR-0206。読み手は engine/src/script/play_metrics_package.h)。
-- 試験の値で、ゲームの中身ではない(D-006)。表を変えるたびに写しの早送りで測り、前の版との差を出す(エディタのパネル「遊びの指標」・
-- bicameral --measure-metrics)。場面は world_cells の初めの世界(空気の中の木箱)。1 秒 = 60 刻み。
return {
    -- 閉じた木箱の壁に火をつけ、1000 K 以上のセル(炎の目安)が無くなるまでの刻み。答えの細かさは 1 秒。
    -- 2026-10-14 の試験の表で約 4.7 分(箱の中の酸素は約 1 分で尽き、残りは冷めるまで。--measure-metrics --log-level debug の道のり)
    sealed_fire_out = {
        label = "密室の火が消えるまで",
        max_ticks = 36000,
        sample_interval_ticks = 60,
        pokes = { { tick = 0, cell = { 28, 32, 32 } } },
        quantity = "cells_at_or_above",
        temperature_mk = 1000000,
        at_most = 0,
    },
}
