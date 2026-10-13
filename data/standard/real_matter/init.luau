-- init.luau — 現実の物質(1 mol あたり)。生成エンタルピーは 298.15 K の値(J/mol)、比熱は一定と置く(mJ/(mol·K))、
-- 熱伝導率は現実の値(mW/(m·K)。サンプルの世界 combustion_test のような試験の倍率は掛けない)。
-- 出典:
--   気体と炭(黒鉛)の生成エンタルピー: CODATA Key Values for Thermodynamics(1989)を丸めたもの
--     (CO2 −393.51・CO −110.53・H2O(気)−241.826 kJ/mol。元素の単体 O2・N2・Ar・黒鉛は 0)。
--   比熱: NIST-JANAF 熱化学表の 298.15 K の Cp°(O2 29.378・N2 29.124・Ar 20.786・CO2 37.135・CO 29.142・
--     H2O(気)33.58・黒鉛 8.52 J/(mol·K))。※ combustion_test から写した値で、表と一つずつは照合していない(未確認)。
--   セルロース(C6H10O5 の 1 単位あたり): 生成エンタルピー約 −963 kJ/mol は燃焼熱からの逆算、比熱は約 1.3 J/(g·K)(どちらも未確認)。
--   熱伝導率(300 K 付近): O2 27・N2 26・Ar 18・CO2 17・CO 25・H2O(気)25・木 120・炭 100(未確認)。
--     現実の値なので、伝導だけでは火はほとんど広がらない(現実の火は放射と対流で広がる。放射 T-0028・気体の流れ T-0210 が入るまで)。
-- 液体の水と氷は相変化(T-0415)と一緒に足す。⑤ の場面に要る分だけ(D-471 ②)。
return {
    species = {
        oxygen = {
            composition = { O = 2 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 29378,
            thermal_conductivity_mw_per_m_k = 27,
        },
        nitrogen = {
            composition = { N = 2 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 29124,
            thermal_conductivity_mw_per_m_k = 26,
        },
        argon = {
            composition = { Ar = 1 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 20786,
            thermal_conductivity_mw_per_m_k = 18,
        },
        carbon_dioxide = {
            composition = { C = 1, O = 2 },
            formation_enthalpy_j_per_mol = -393509,
            heat_capacity_mj_per_mol_k = 37135,
            thermal_conductivity_mw_per_m_k = 17,
        },
        carbon_monoxide = {
            composition = { C = 1, O = 1 },
            formation_enthalpy_j_per_mol = -110527,
            heat_capacity_mj_per_mol_k = 29142,
            thermal_conductivity_mw_per_m_k = 25,
        },
        water_vapor = {
            composition = { H = 2, O = 1 },
            formation_enthalpy_j_per_mol = -241826,
            heat_capacity_mj_per_mol_k = 33580,
            thermal_conductivity_mw_per_m_k = 25,
        },
        -- 炭(黒鉛として扱う)
        carbon = {
            composition = { C = 1 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 8520,
            thermal_conductivity_mw_per_m_k = 100,
        },
        -- 木・草の燃える部分(セルロース C6H10O5 の 1 単位)
        cellulose = {
            composition = { C = 6, H = 10, O = 5 },
            formation_enthalpy_j_per_mol = -963000,
            heat_capacity_mj_per_mol_k = 211000,
            thermal_conductivity_mw_per_m_k = 120,
        },
    },
}
