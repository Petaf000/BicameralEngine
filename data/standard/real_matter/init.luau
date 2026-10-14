-- init.luau — 現実の物質(1 mol あたり)。生成エンタルピーは 298.15 K の値(J/mol)、比熱は一定と置く(mJ/(mol·K))、
-- 熱伝導率は現実の値(mW/(m·K)。試験の倍率は掛けない。サンプルの世界 combustion_test はこれを写して倍率を掛けて書き直す。T-0498)。
-- 出典:
--   気体と炭(黒鉛)の生成エンタルピー: CODATA Key Values for Thermodynamics(1989)を丸めたもの
--     (CO2 −393.51・CO −110.53・H2O(気)−241.826 kJ/mol。元素の単体 O2・N2・Ar・黒鉛は 0)。
--   比熱: NIST-JANAF 熱化学表の 298.15 K の Cp°(O2 29.378・N2 29.124・Ar 20.786・CO2 37.135・CO 29.142・
--     H2O(気)33.58・黒鉛 8.52 J/(mol·K))。※ T-0014 の C++ の表から写した値で、表と一つずつは照合していない(未確認)。
--   セルロース(C6H10O5 の 1 単位あたり): 生成エンタルピー約 −963 kJ/mol は燃焼熱からの逆算、比熱は約 1.3 J/(g·K)(どちらも未確認)。
--   熱伝導率(300 K 付近): O2 27・N2 26・Ar 18・CO2 17・CO 25・H2O(気)25・木 120・炭 100(未確認)。
--     現実の値なので、伝導だけでは火はほとんど広がらない(現実の火は放射と対流で広がる。放射 T-0028・気体の流れ T-0210 が入るまで)。
--   標準エントロピー(298.15 K・1 bar。任意の欄。坂の検査〔T-0491・D-487〕だけが読む):
--     CODATA Key Values for Thermodynamics(1989)の S°(O2 205.152・N2 191.609・Ar 154.846・CO2 213.785・CO 197.660・
--     H2O(気)188.835・黒鉛 5.740 J/(mol·K))。セルロースの 170 J/(mol·K)(C6H10O5 の 1 単位)は推定で**未確認**
--     (結晶のグルコース 209.2 J/(mol·K) から見積もった。照合先の候補: Goldberg et al., J. Chem. Thermodyn. 81 (2015) 184)。
--     セルロースの値を ±100 J/(mol·K) 動かしても、セルロースの規則が坂を下る向きは変わらない(T-0491 の試験の表)。
-- 液体の水と氷は相変化(T-0415)と一緒に足す。⑤ の場面に要る分だけ(D-471 ②)。
return {
    species = {
        oxygen = {
            composition = { O = 2 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 29378,
            thermal_conductivity_mw_per_m_k = 27,
            standard_entropy_mj_per_mol_k = 205152,
        },
        nitrogen = {
            composition = { N = 2 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 29124,
            thermal_conductivity_mw_per_m_k = 26,
            standard_entropy_mj_per_mol_k = 191609,
        },
        argon = {
            composition = { Ar = 1 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 20786,
            thermal_conductivity_mw_per_m_k = 18,
            standard_entropy_mj_per_mol_k = 154846,
        },
        carbon_dioxide = {
            composition = { C = 1, O = 2 },
            formation_enthalpy_j_per_mol = -393509,
            heat_capacity_mj_per_mol_k = 37135,
            thermal_conductivity_mw_per_m_k = 17,
            standard_entropy_mj_per_mol_k = 213785,
        },
        carbon_monoxide = {
            composition = { C = 1, O = 1 },
            formation_enthalpy_j_per_mol = -110527,
            heat_capacity_mj_per_mol_k = 29142,
            thermal_conductivity_mw_per_m_k = 25,
            standard_entropy_mj_per_mol_k = 197660,
        },
        water_vapor = {
            composition = { H = 2, O = 1 },
            formation_enthalpy_j_per_mol = -241826,
            heat_capacity_mj_per_mol_k = 33580,
            thermal_conductivity_mw_per_m_k = 25,
            standard_entropy_mj_per_mol_k = 188835,
        },
        -- 炭(黒鉛として扱う)
        carbon = {
            composition = { C = 1 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 8520,
            thermal_conductivity_mw_per_m_k = 100,
            standard_entropy_mj_per_mol_k = 5740,
        },
        -- 木・草の燃える部分(セルロース C6H10O5 の 1 単位)
        cellulose = {
            composition = { C = 6, H = 10, O = 5 },
            formation_enthalpy_j_per_mol = -963000,
            heat_capacity_mj_per_mol_k = 211000,
            thermal_conductivity_mw_per_m_k = 120,
            standard_entropy_mj_per_mol_k = 170000,
        },
    },
}
