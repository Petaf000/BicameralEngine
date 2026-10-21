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
--   相(任意の欄 phase。省けば気体。T-0710 で condensed から直した): 固体は炭(黒鉛)・セルロース・氷、液体は水。
--   動き方の任意の値(T-0710・土台の決定 3。まだ刻みは読まない。名前と単位は設計の仮):
--     液体の水の粘り 890 µPa·s: 25 ℃ の 0.8900 mPa·s(IAPWS 2008 の水の粘性の式の 25 ℃・0.1 MPa の値を丸めた。未確認)。
--     湯気の混ざる速さ 25 mm²/s: 空気の中の水蒸気の拡散係数 0.24〜0.28 cm²/s(25 ℃・1 atm。教科書により幅がある。未確認)の中ほど。
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
--   光の値(凝縮相の 4 つ。2 つの帯〔短い = 見える光と紫外・長い = 赤外。境は約 0.7 µm〕。ADR-0395・T-0942 の後の本体 2026-10-22):
--     吸い込み a(mm²/mol)= 物の吸収係数 µ(1/m)÷ モル濃度 c(mol/m³)× 10^6。厚さ L の層は e^−µL を通す(セルの式 1 − e^−Σa·n/面 と同じ)。
--     照り返しは「当たった光のうち吸わずに返す割合」(千分率)。長い帯は熱の赤外(8〜14 µm)の値で決めた(太陽の近赤外 0.7〜2.5 µm では
--     水も氷もずっと通し、木は半分ほど返す。帯を 2 つにまとめた丸め。未確認)。どの値も教科書の目安から決めた丸めで、表と一つずつは照合していない(未確認)。
--     炭(黒鉛。c = 2260 ÷ 0.012011 = 188160 mol/m³): 見える光で n 2.7・k 1.4(550 nm)→ µ = 4πk/λ ≈ 3.2e7 /m → a 1.7e8。
--       赤外(10 µm)で k ≈ 6 → µ ≈ 7.5e6 /m → a 4.0e7。どちらも 1 µm も入らず真っ黒。照り返しは木炭・煤の 5 %(放射率 0.95 ほど)。
--       磨いた黒鉛の結晶は見える光を 25〜30 % 返すが、炭は孔の多い面として煤の値にした。
--     セルロース(c = 1500 ÷ 0.16214 = 9251 mol/m³): 乾いた木は見える光を 0.3 mm ほどで止め(µ ≈ 3e3 /m。散らしも吸い込みに入れた目安)→ a 3.2e5、
--       照り返し 40 %(乾いた木・わら 30〜50 %。純粋なセルロース〔紙・綿〕は白く 80 % ほど)。赤外は C−O の吸収帯で強く吸う(µ ≈ 1e5 /m)→ a 1.1e7、
--       照り返し 10 %(木の放射率 0.9 ほど)。
--     液体の水(c = 1000 ÷ 0.018015 = 55508 mol/m³): 見える光の吸収係数は 400 nm 0.007・500 nm 0.03・600 nm 0.22・700 nm 0.65 /m
--       (Pope & Fry 1997 の純水の値の目安)。太陽の光で重みを付けた平均を µ ≈ 0.15 /m と置いた → a 3(1 m で 85 % を通す)。
--       赤外(10 µm)で k ≈ 0.05 → µ ≈ 6e4 /m(0.1 mm で止まる)→ a 1.08e6。照り返しは面のフレネルの値: 見える光 n 1.333 で 2 %・赤外は放射率 0.98 で 2 %。
--       ※ エンジンの照り返しは止めた光(吸った + 返した)を分ける割合なので、通る光の多い水では面のきらめきは小さく出る(今の式の限り)。
--     氷(Ih。c = 917 ÷ 0.018015 = 50902 mol/m³): 見える光は水と同じくほぼ透明(Warren & Brandt 2008 の目安。µ ≈ 0.15 /m)→ a 3。
--       赤外は水と同じ µ ≈ 6e4 /m と置いた → a 1.18e6。照り返し: 見える光 n 1.31 で 1.8 %・赤外は放射率 0.97 で 3 %。
--       雪(白い・照り返し 80〜90 %)は粒の間の散らしで、ここの氷(透き通った塊)とは別の物として書く。
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
            diffusivity_mm2_per_s = 25,
        },
        -- 炭(黒鉛として扱う)
        carbon = {
            composition = { C = 1 },
            formation_enthalpy_j_per_mol = 0,
            heat_capacity_mj_per_mol_k = 8520,
            thermal_conductivity_mw_per_m_k = 100,
            standard_entropy_mj_per_mol_k = 5740,
            phase = "solid",
            density_kg_per_m3 = 2260,
            light_absorption_short_mm2_per_mol = 170000000,
            light_absorption_long_mm2_per_mol = 40000000,
            light_reflectance_short_permille = 50,
            light_reflectance_long_permille = 50,
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
            viscosity_upa_s = 890,
            light_absorption_short_mm2_per_mol = 3,
            light_absorption_long_mm2_per_mol = 1080000,
            light_reflectance_short_permille = 20,
            light_reflectance_long_permille = 20,
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
            light_absorption_short_mm2_per_mol = 3,
            light_absorption_long_mm2_per_mol = 1180000,
            light_reflectance_short_permille = 18,
            light_reflectance_long_permille = 30,
        },
        -- 木・草の燃える部分(セルロース C6H10O5 の 1 単位)
        cellulose = {
            composition = { C = 6, H = 10, O = 5 },
            formation_enthalpy_j_per_mol = -963000,
            heat_capacity_mj_per_mol_k = 211000,
            thermal_conductivity_mw_per_m_k = 120,
            standard_entropy_mj_per_mol_k = 170000,
            phase = "solid",
            density_kg_per_m3 = 1500,
            light_absorption_short_mm2_per_mol = 320000,
            light_absorption_long_mm2_per_mol = 11000000,
            light_reflectance_short_permille = 400,
            light_reflectance_long_permille = 100,
        },
    },
}
