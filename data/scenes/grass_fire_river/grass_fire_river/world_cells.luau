-- world_cells.luau — 場面の組み立て(仮の世界の格子 64³ セル・1 セル 0.5 m 角。y が上)。見本の値(D-006)。
-- 火は x の向きに広がり、川は z の向きに流れる(と見立てた、流れない水の帯。水の流れは T-0027)。
--   y 0〜3   地面(乾いた砂。厚さ 2 m)
--   y 4      草(x 0〜39 が手前・x 44〜63 が向こう岸)。上は空気
--   x 40〜43 川(幅 2 m・深さ 2 m。y 1〜4 が水、y 0 は川底の砂)
-- 量は 1 セルあたりの µmol、温度は mK(300 K)。
local AIR_OXYGEN = 1063800 -- 1 気圧・300 K の 0.5 m 角の空気(サンプルの世界と同じ)
local AIR_NITROGEN = 4014200

-- 草の量: 乾いた草 0.78 kg/m²(セルの床 0.25 m² に 0.195 kg)をセルロース(162 g/mol)で約 1.2 mol。
-- 草の体積はわずかなので、残りは空気のまま
local GRASS_CELLULOSE = 1200000
-- 地面: 乾いた砂のかさ密度 1500 kg/m³ でセル 187.5 kg = SiO2 約 3121 mol。孔の 4 割は空気
-- (1 セルの量は合計 2^36 µmol ≒ 68,719 mol まで・気体のぶんは約 2147 mol まで。T-0558・ADR-0260)
local SOIL_SILICA = 3121000000
local SOIL_PORE_AIR = 4 -- 割(孔の空気)
-- 川の水: 0.125 m³ × 997 kg/m³ ÷ 18.015 g/mol ≒ 6917 mol(セルいっぱい)
local RIVER_WATER = 6917000000

local TEMPERATURE = 300000

return {
    air = {
        amounts_umol = { oxygen = AIR_OXYGEN, nitrogen = AIR_NITROGEN },
        temperature_mk = TEMPERATURE,
    },
    ground = {
        layer = 1,
        min_cell = { 0, 0, 0 },
        size_cells = { 64, 4, 64 },
        amounts_umol = {
            silica = SOIL_SILICA,
            oxygen = AIR_OXYGEN * SOIL_PORE_AIR // 10,
            nitrogen = AIR_NITROGEN * SOIL_PORE_AIR // 10,
        },
        temperature_mk = TEMPERATURE,
    },
    grass_near = {
        layer = 2,
        min_cell = { 0, 4, 0 },
        size_cells = { 40, 1, 64 },
        amounts_umol = { cellulose = GRASS_CELLULOSE, oxygen = AIR_OXYGEN, nitrogen = AIR_NITROGEN },
        temperature_mk = TEMPERATURE,
    },
    grass_far = {
        layer = 2,
        min_cell = { 44, 4, 0 },
        size_cells = { 20, 1, 64 },
        amounts_umol = { cellulose = GRASS_CELLULOSE, oxygen = AIR_OXYGEN, nitrogen = AIR_NITROGEN },
        temperature_mk = TEMPERATURE,
    },
    river = {
        layer = 3,
        min_cell = { 40, 1, 0 },
        size_cells = { 4, 4, 64 },
        amounts_umol = { water = RIVER_WATER },
        temperature_mk = TEMPERATURE,
    },
}
