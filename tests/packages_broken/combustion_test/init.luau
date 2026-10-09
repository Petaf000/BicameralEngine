-- init.luau — C + O2 → CO2 の生成物から O を 1 つ落とした規則(元素が釣り合わない)。形は正しく、ベイクの検査で落ちる。
return {
    elements = {
        C = { atomic_mass_mg_per_mol = 12011 },
        O = { atomic_mass_mg_per_mol = 15999 },
    },
    species = {
        carbon = {
            composition = { C = 1 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 8520,
            thermal_conductivity_mw_per_m_k = 100000,
        },
        oxygen = {
            composition = { O = 2 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 29378,
            thermal_conductivity_mw_per_m_k = 26000,
        },
        carbon_monoxide = {
            composition = { C = 1, O = 1 },
            formation_enthalpy_j_per_mol = -110530,
            heat_capacity_mj_per_mol_k = 29142,
            thermal_conductivity_mw_per_m_k = 25000,
        },
    },
    reactions = {
        carbon_combustion = {
            reactants = { carbon = 1, oxygen = 1 },
            products = { carbon_monoxide = 1 },
            rate = { a = "3e8", activation_energy_j_per_mol = 160000 },
        },
    },
}
