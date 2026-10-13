-- world_cells.luau — サンプルの世界の初めのセル(T-0398。前は C++ の sim::MakeProbeInitialWorld に書いていた)。試験の値で、ゲームの中身ではない(D-006)。
-- 領域は layer の小さい順に重ね書きする。場所(min_cell・size_cells。セル)を省けば格子の全部。量は µmol、温度は mK。
-- 仮の世界の格子は 64³ セル(1 セル 0.5 m 角 = 0.125 m³)。
return {
    -- 1 気圧・300 K の 0.5 m 角の空気 = 約 5.08 mol(O2 20.95 %・残りを N2。Ar は無視)
    air = {
        amounts_umol = { oxygen = 1063800, nitrogen = 4014200 },
        temperature_mk = 300000,
    },
    -- 木箱: 中央の 8³ セル(4 m 角)の外側の厚さ 1 セル。壁のセルは体積の 1 割が木(密度 500 kg/m³ のセルロースで約 38.6 mol)、
    -- 残りは孔の中の空気。中は空気のまま
    crate_wall = {
        layer = 1,
        min_cell = { 28, 28, 28 },
        size_cells = { 8, 8, 8 },
        shell_cells = 1,
        amounts_umol = { cellulose = 38600000, oxygen = 983000, nitrogen = 3697000 },
        temperature_mk = 300000,
    },
}
