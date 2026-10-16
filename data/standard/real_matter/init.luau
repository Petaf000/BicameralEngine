-- init.luau — 現実の物質(1 mol あたり)。生成エンタルピーは 298.15 K の値(J/mol)、比熱は一定と置く(mJ/(mol·K))、
-- 熱伝導率は現実の値(mW/(m·K)。試験の倍率は掛けない。サンプルの世界 combustion_test はこれを写して倍率を掛けて書き直す。T-0498)。
-- 出典:
--   気体と炭(黒鉛)の生成エンタルピー: CODATA Key Values for Thermodynamics(1989)を丸めたもの
--     (CO2 −393.51・CO −110.53・H2O(気)−241.826 kJ/mol。元素の単体 O2・N2・Ar・黒鉛は 0)。
--   比熱: NIST-JANAF 熱化学表の 298.15 K の Cp°(定圧のまま書く。ベイクが気体の定積の比熱 c_v = c_p − R を作る。T-0597・ADR-0259。O2 29.378・N2 29.124・Ar 20.786・CO2 37.135・CO 29.142・
--     H2O(気)33.58・黒鉛 8.52 J/(mol·K))。※ T-0014 の C++ の表から写した値で、表と一つずつは照合していない(未確認)。
--   セルロース(C6H10O5 の 1 単位あたり): 生成エンタルピー約 −963 kJ/mol は燃焼熱からの逆算、比熱は約 1.3 J/(g·K)(どちらも未確認)。
--   熱伝導率(300 K 付近): O2 27・N2 26・Ar 18・CO2 17・CO 25・H2O(気)25・木 120・炭 100(未確認)。
--     現実の値なので、伝導だけでは火はほとんど広がらない(現実の火は放射と対流で広がる。放射 T-0028・気体の流れ T-0210 が入るまで)。
--   標準エントロピー(298.15 K・1 bar。任意の欄。坂の検査〔T-0491・D-487〕だけが読む):
--     CODATA Key Values for Thermodynamics(1989)の S°(O2 205.152・N2 191.609・Ar 154.846・CO2 213.785・CO 197.660・
--     H2O(気)188.835・黒鉛 5.740 J/(mol·K))。セルロースの 170 J/(mol·K)(C6H10O5 の 1 単位)は推定で**未確認**
--     (結晶のグルコース 209.2 J/(mol·K) から見積もった。照合先の候補: Goldberg et al., J. Chem. Thermodyn. 81 (2015) 184)。
--     セルロースの値を ±100 J/(mol·K) 動かしても、セルロースの規則が坂を下る向きは変わらない(T-0491 の試験の表)。
--   凝縮相(任意の欄 condensed。坂の計器〔T-0518〕が活量を 1 と置く。気体は分圧): 炭(黒鉛)とセルロース。
--   密度(density_kg_per_m3。凝縮相は全部に書く。セルの場所を取る量 = 物質量 × モル体積。ADR-0268・T-0616。書き忘れはベイクが警告する):
--     炭(黒鉛)2260 kg/m³: 理想的な黒鉛の結晶の X 線密度 2.266 g/cm³(CRC Handbook of Chemistry and Physics の黒鉛 2.2〜2.27 の上端)を丸めた。
--       木炭の骨格の密度(1.4〜2.0)より大きいが、炭は黒鉛として扱う(孔は物質量の少なさで出る)。
--     セルロース 1500 kg/m³: 木の細胞壁の実質の密度(木材の真密度。教科書の値 1.49〜1.54 g/cm³。結晶のセルロース Iβ の計算値 1.63・
--       非晶 1.48 の間)。木の孔(木材のかさ密度 0.3〜0.9)は物質量で出る。どちらも資料と一つずつは照合していない(未確認)。
--     液体の水 1000・氷 917 は下の「水の 3 相」。
--   水の 3 相(T-0611・ADR-0272。湯気は上の water_vapor。相変化は real_reactions の規則):
--     液体の水(water): 生成エンタルピー −285.830 kJ/mol・S° 69.95 J/(mol·K)(CODATA Key Values 1989)・
--       Cp 75.33 J/(mol·K)(NIST-JANAF 298.15 K)・熱伝導率 0.598 W/(m·K)(25 ℃。IAPWS 2011 の目安)・
--       密度 1000 kg/m³(0 ℃ 999.84・25 ℃ 997.05 を丸めた。一定と置く)。蒸発熱は生成エンタルピーの差 44.004 kJ/mol(298.15 K)。
--     氷(ice。Ih): 融解熱 6.010 kJ/mol・融解のエントロピー 22.00 J/(mol·K)(273.15 K。どちらも教科書の値)・
--       Cp 38.09 J/(mol·K)(273 K 付近。Giauque & Stout 1936 の値の目安)・熱伝導率 2.22 W/(m·K)(0 ℃)・密度 917 kg/m³(0 ℃ 916.7)。
--       エンジンは比熱を一定と置くので、273.15 K で融解熱 6.010 kJ/mol・ΔS 22.00 になるように 298.15 K の値へ戻して書く
--       (生成エンタルピー = −285.830 − 6.010 − (75.33 − 38.09) × 25.00 / 1000 = −292.771 kJ/mol、
--        S° = 69.95 − 22.00 − 37.24 × ln(298.15 / 273.15) = 44.686 J/(mol·K)。273.15 K の S は 41.4 で、文献の 41.0 と合う)。
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
            condensed = true,
            density_kg_per_m3 = 2260,
        },
        -- 液体の水(T-0611。低い方へ流れるのは液体の区画〔T-0609〕。湯気・氷との行き来は real_reactions の規則)
        water = {
            composition = { H = 2, O = 1 },
            formation_enthalpy_j_per_mol = -285830,
            heat_capacity_mj_per_mol_k = 75330,
            thermal_conductivity_mw_per_m_k = 598,
            standard_entropy_mj_per_mol_k = 69950,
            phase = "liquid",
            density_kg_per_m3 = 1000,
        },
        -- 氷(T-0611。値は 298.15 K へ戻したもの。上の出典)
        ice = {
            composition = { H = 2, O = 1 },
            formation_enthalpy_j_per_mol = -292771,
            heat_capacity_mj_per_mol_k = 38090,
            thermal_conductivity_mw_per_m_k = 2220,
            standard_entropy_mj_per_mol_k = 44686,
            phase = "solid",
            density_kg_per_m3 = 917,
        },
        -- 木・草の燃える部分(セルロース C6H10O5 の 1 単位)
        cellulose = {
            composition = { C = 6, H = 10, O = 5 },
            formation_enthalpy_j_per_mol = -963000,
            heat_capacity_mj_per_mol_k = 211000,
            thermal_conductivity_mw_per_m_k = 120,
            standard_entropy_mj_per_mol_k = 170000,
            condensed = true,
            density_kg_per_m3 = 1500,
        },
    },
}
