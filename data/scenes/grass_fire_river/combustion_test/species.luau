-- species.luau — 場面の物質(1 mol あたり)。
-- 現実の物質(real_matter)は写して熱伝導率だけに試験の倍率を掛ける(units.luau)。凝縮相(condensed)は固体の倍率、ほかは気体の倍率。
-- 足す物質(地面の砂・液体の水)は現実の値で、熱伝導率は水だけ固体の倍率(川が熱を吸う)。出典:
--   SiO2(α 石英): 生成エンタルピー −910.7 kJ/mol・Cp 44.43 J/(mol·K)・S° 41.46 J/(mol·K)(NIST-JANAF。表と一つずつは照合していない = 未確認)。
--     熱伝導率は乾いた砂の目安 1.4 W/(m·K)(石英の結晶はずっと大きい。土の値は水分で数倍変わる。未確認)。試験の倍率は掛けない。
--   H2O(液): −285.83 kJ/mol・Cp 75.33 J/(mol·K)・S° 69.95 J/(mol·K)(CODATA 1989)・0.598 W/(m·K)(25 ℃)。
--     **相変化が無い**(沸かない・凍らない。T-0415)ので、熱いと液体のまま熱くなる(この場面では川の水は温まりきらない)。
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
    water = {
        composition = { H = 2, O = 1 },
        formation_enthalpy_j_per_mol = -285830,
        heat_capacity_mj_per_mol_k = 75330,
        thermal_conductivity_mw_per_m_k = units.solid_conductivity(598),
        standard_entropy_mj_per_mol_k = 69950,
        condensed = true,
    },
}

for name, real in realSpecies do
    local scaled = table.clone(real)
    local conductivity = real.thermal_conductivity_mw_per_m_k
    scaled.thermal_conductivity_mw_per_m_k = if real.condensed
        then units.solid_conductivity(conductivity)
        else units.gas_conductivity(conductivity)
    species[name] = scaled
end

return species
