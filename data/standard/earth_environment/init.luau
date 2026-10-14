-- init.luau — 地球の環境の値(分類 environment。T-0400・T-0373・T-0499・ADR-0176・ADR-0182)。
-- 読み手は engine/src/script/environment_package.cpp。値は整数で、単位は鍵の名前。
-- 基準の大気は高さの点の列(atmosphere_profile。点の間は直線)と、一番低い点(海面)の圧力。ほかの高さの圧力はエンジンが
-- 重力・温度・成分から静水圧の釣り合いで出す(D-494)。気体の層の基準になる(気体を世界に載せる段 T-0210 が読む)。セルを満たすのは earth_air。
-- 出典:
--   温度の高さの分布: U.S. Standard Atmosphere, 1976(NOAA/NASA/USAF)= ISO 2533:1975 の国際標準大気(ISA)。
--     層の境の高さ(ジオポテンシャル高度。重力が一定の世界ではそのまま高さ)と温度: 0 m 288.15 K・11 km 216.65 K・20 km 216.65 K・
--     32 km 228.65 K・47 km 270.65 K・51 km 270.65 K・71 km 214.65 K・84.852 km 186.946 K。海面の圧力 101325 Pa。
--     層の中で温度は高さに対して直線(ISA の定義)なので、点の間を直線でつなぐ書き方で ISA そのものになる。
--   成分: 乾いた空気の体積比(N2 78.08・O2 20.946・Ar 0.934・CO2 約 0.042 %。足して 100 % になるよう N2 で合わせた。
--     水蒸気・Ne・He などの微量の気体は入れない)。約 86 km より下はよく混ざって一様(U.S. Standard Atmosphere 1976 の均質圏。
--     平均のモル質量 28.9644 g/mol を 80 km まで一定と置く)なので、どの点も同じ成分。重い物が下に溜まる大気を作るなら atmosphere.luau の settled。
-- ゲームはこの表を require("@earth_environment") で写し、変えたい点だけ atmosphere.luau の set_point で書き足す・上書きして書き直す。
local DRY_AIR: { [string]: number } = {
    nitrogen = 780780,
    oxygen = 209460,
    argon = 9340,
    carbon_dioxide = 420,
}

-- ISA の層の境(高さ mm → 温度 mK)
local ISA_LAYERS: { { number } } = {
    { 0, 288150 },
    { 11000000, 216650 },
    { 20000000, 216650 },
    { 32000000, 228650 },
    { 47000000, 270650 },
    { 51000000, 270650 },
    { 71000000, 214650 },
    { 84852000, 186946 },
}

local profile: { AtmospherePoint } = {}
for _, layer in ISA_LAYERS do
    table.insert(profile, { height_mm = layer[1], temperature_mk = layer[2], composition = table.clone(DRY_AIR) })
end

return {
    environment = {
        gravity_um_per_s2 = 9806650, -- 重力 9.80665 m/s²(標準の重力の定義値)
        sound_speed_mm_per_s = 340000, -- 気体の音速の基準 340 m/s(D-440: 本物の音速。15 ℃ の空気で約 340.3 m/s)
        atmosphere_surface_pressure_pa = 101325, -- 海面(一番低い点)の基準の大気の圧力 1 気圧
        atmosphere_profile = profile, -- 基準の大気の高さの点(ISA の層の境)
    },
}
