-- init.luau — 世界の全部のセルを地球の基準の大気(earth_environment と同じ 15 ℃・101325 Pa・乾いた空気)で満たす(分類 world_cells。T-0398)。
-- 量は 1 セル(0.5 m 角 = 0.125 m³)あたりの µmol: 理想気体 n = PV/(RT) = 101325 × 0.125 ÷ (8.314462618 × 288.15) ≈ 5.28657 mol を
-- 成分の割合(N2 780780・O2 209460・Ar 9340・CO2 420 ppm)で分けて丸めた値。地面からの高さによる圧力の差は入れない(一様)。
-- 層は −1000(ほかのパッケージの領域〔層 0 以上〕が上に重なる)。
return {
    world_cells = {
        earth_air = {
            layer = -1000,
            amounts_umol = {
                nitrogen = 4127647,
                oxygen = 1107325,
                argon = 49377,
                carbon_dioxide = 2220,
            },
            temperature_mk = 288150,
        },
    },
}
