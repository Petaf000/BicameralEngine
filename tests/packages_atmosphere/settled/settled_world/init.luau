-- init.luau — 地球の温度の高さの分布のまま、地面で半分が二酸化炭素の大気を「重い物ほど下に溜まる」形で作り(atmosphere.settled)、
-- 海面の点だけ温度を書き直す(atmosphere.set_point)。依存の標準パッケージの鍵をゲーム本体が書き直す形(T-0498)。
local atmosphere = require("@earth_environment/atmosphere")
local earth: any = require("@earth_environment")
local matter: any = require("@real_matter")
local elements: any = require("@real_elements")

local profile = atmosphere.settled({
    gravity_um_per_s2 = earth.environment.gravity_um_per_s2,
    ground = { nitrogen = 500000, carbon_dioxide = 500000 },
    temperatures = earth.environment.atmosphere_profile,
    heights_mm = { 0, 5000000, 10000000, 15000000 },
    molar_masses = atmosphere.molar_masses(matter.species, elements.elements),
})

local sea = table.clone(profile[1])
sea.temperature_mk = 300000
profile = atmosphere.set_point(profile, sea)

return {
    environment = {
        atmosphere_profile = profile,
    },
}
