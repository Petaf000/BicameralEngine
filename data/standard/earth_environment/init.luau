-- init.luau — 地球の環境の値(分類 environment・atmosphere。T-0400・T-0373・ADR-0176・ADR-0182)。
-- 読み手は engine/src/script/environment_package.cpp。値は整数で、単位は鍵の名前。
-- 基準の大気は国際標準大気(ISA)の海面の値(15 ℃・101325 Pa)と、乾いた空気の成分(体積比。N2 78.08・O2 20.946・Ar 0.934・CO2 約 0.042 %。
-- 足して 100 % になるよう N2 で合わせた。水蒸気・Ne・He などの微量の気体は入れない)。
-- 基準の大気は気体の層の基準(地面の圧力・温度・成分)になる。気体を世界に載せる段(T-0210)が読む。セルを満たすのは earth_air。
return {
    environment = {
        gravity_um_per_s2 = 9806650, -- 重力 9.80665 m/s²(標準の重力の定義値)
        sound_speed_mm_per_s = 340000, -- 気体の音速の基準 340 m/s(D-440: 本物の音速。15 ℃ の空気で約 340.3 m/s)
        atmosphere_temperature_mk = 288150, -- 地面の基準の大気の温度 15 ℃
        atmosphere_surface_pressure_pa = 101325, -- 地面の基準の大気の圧力 1 気圧
    },
    -- 基準の大気の成分(物質 → 物質量の割合 ppm。足して 1000000)
    atmosphere = {
        nitrogen = 780780,
        oxygen = 209460,
        argon = 9340,
        carbon_dioxide = 420,
    },
}
