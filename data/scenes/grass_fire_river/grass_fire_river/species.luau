-- species.luau — 場面の物質(1 mol あたり)。
-- 現実の物質(real_matter)は写して熱伝導率だけに試験の倍率を掛ける(units.luau)。凝縮相(condensed か phase が gas でない)は固体の倍率、ほかは気体の倍率。
-- 足す物質(地面の砂)は現実の値。real_matter の凝縮相(液体の水・氷も)は固体の倍率(川が熱を吸う)。出典:
--   SiO2(α 石英): 生成エンタルピー −910.7 kJ/mol・Cp 44.43 J/(mol·K)・S° 41.46 J/(mol·K)(NIST-JANAF。表と一つずつは照合していない = 未確認)。
--     熱伝導率は乾いた砂の目安 1.4 W/(m·K)(石英の結晶はずっと大きい。土の値は水分で数倍変わる。未確認)。試験の倍率は掛けない。
--   液体の水(water)は real_matter の値を写す(T-0611 から。前はここに相変化の無い水を書いていた)。real_reactions の
--     蒸発・凝縮・融解・凝固の規則が効くので、火に近い川の水は湯気になって熱を吸う(仮の世界の気体の区画が GPU に載るまで湯気は流れない。T-0592)。
local units = require("units")
local realMatter: any = require("@real_matter")
local realSpecies: { [string]: Species } = realMatter.species

local species: { [string]: Species } = {
    silica = {
        composition = { Si = 1, O = 2 },
        formation_enthalpy_j_per_mol = -910700,
        heat_capacity_mj_per_mol_k = 44430,
        -- 倍率を掛けない(現実の値のまま): 倍率は燃える物の中を伝わる放射と対流の代わりで、地面は燃えない。
        -- 掛けると地面が大きな熱の吸い込みになり、草の火が 1 秒で消えた(2026-10-15 の測り。作り心地の記録 5)
        thermal_conductivity_mw_per_m_k = 1400,
        standard_entropy_mj_per_mol_k = 41460,
        condensed = true,
    },
}

for name, real in realSpecies do
    local scaled = table.clone(real)
    local conductivity = real.thermal_conductivity_mw_per_m_k
    local condensed = real.condensed or (real.phase ~= nil and real.phase ~= "gas")
    scaled.thermal_conductivity_mw_per_m_k = if condensed
        then units.solid_conductivity(conductivity)
        else units.gas_conductivity(conductivity)
    species[name] = scaled
end

return species
