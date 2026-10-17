-- init.luau — 現実に則した反応の規則。反応熱は書かない(ベイクが生成エンタルピーの差から決める。02 §2)。
-- reaction_enthalpy_j_per_mol は文献の反応熱(298.15 K)で、ベイクが生成エンタルピーの差と比べて食い違えば警告する(検査用)。
-- 速度(Arrhenius: k = A·exp(−Ea/RT)、A は SI):
--   セルロースの熱分解: Bradbury, Sakai & Shafizadeh(1979, J. Appl. Polym. Sci. 23, 3271)の開始反応
--     (A = 2.8e19 /s・Ea = 242.4 kJ/mol)を、炭と水蒸気までの 1 段の反応に使っている(単純化)。
--   ほかの 4 本: 600〜1000 K で燃え始めるように置いた値で、文献の速度ではない(未確認。サンプルの世界 combustion_test もこの値を使う)。
--     ⑤ の場面(T-0414・T-0416)で文献の値に替える。
-- 反応物の次数は、書いていなければ全部 1(orders を省いた)。
--
-- 水の相変化(T-0611・ADR-0272 → T-0623・ADR-0290 で釣り合いの規則に)。1 本で両向き(ベイクが逆向き「名前~逆」を作る)。
--   潜熱は物質の生成エンタルピーの差から出る(書かない)。凝縮相(水・氷)は釣り合いの式に入らない(活量 1)。
--   rate は「釣り合いへ近づく速さ」(1/s。距離の 1/60 ずつ 1 刻みに進む。仮の値)。
--   - 蒸発 ⇄ 凝縮: 釣り合いの湯気の濃さ c(T) = c(373.15 K) × exp(−ΔH/R × (1/T − 1/373.15))。
--     c = P_sat / RT を 298.15 K(3169.9 Pa → 1.2787 mol/m³)と 373.15 K(101.418 kPa → 32.688 mol/m³)の 2 点で合わせた
--     (IAPWS-95。Wagner & Pruss, J. Phys. Chem. Ref. Data 31 (2002) 387)。ΔH_eq = 39975 J/mol(濃さで合わせた値。潜熱そのものではない)。
--     水が無くても、湯気が濃さを超えれば凝縮する(霧・露)。
--   - 融解 ⇄ 凝固: 凝縮相だけなので、温度がちょうど 273.15 K になる所で釣り合う(潜熱の分だけ温度を動かして止まる)。
--     種の氷が無くても、273.15 K より冷えた水は凍り始める(過冷却は書いていない)。ΔH_eq は向きの符号にだけ効く(融解熱 6.01 kJ/mol)。
return {
    reactions = {
        -- 酸素なしで炭と水蒸気になる(一次反応)
        cellulose_pyrolysis = {
            reactants = { cellulose = 1 },
            products = { carbon = 6, water_vapor = 5 },
            rate = { a = "2.8e19", activation_energy_j_per_mol = 242400 },
        },
        -- 木・草が燃える(O2 を炭の燃焼と取り合う)
        cellulose_combustion = {
            reactants = { cellulose = 1, oxygen = 6 },
            products = { carbon_dioxide = 6, water_vapor = 5 },
            rate = { a = "2e10", activation_energy_j_per_mol = 150000 },
        },
        -- C + O2 → CO2(ΔH = −393.5 kJ/mol)
        carbon_combustion = {
            reactants = { carbon = 1, oxygen = 1 },
            products = { carbon_dioxide = 1 },
            rate = { a = "3e8", activation_energy_j_per_mol = 160000 },
            reaction_enthalpy_j_per_mol = -393500,
        },
        -- 2C + O2 → 2CO(ΔH = −221.0 kJ/mol)。熱いと CO が増える
        carbon_partial_combustion = {
            reactants = { carbon = 2, oxygen = 1 },
            products = { carbon_monoxide = 2 },
            rate = { a = "1.5e9", activation_energy_j_per_mol = 200000 },
            reaction_enthalpy_j_per_mol = -221000,
        },
        -- --- 水の相変化(上の注)---
        -- 蒸発 ⇄ 凝縮: 水 ⇄ 湯気(ΔH = +44.0 kJ/mol。吸熱)
        water_evaporation = {
            reactants = { water = 1 },
            products = { water_vapor = 1 },
            balance = { temperature_mk = 373150, concentration_mol_per_m3 = "32.688", enthalpy_j_per_mol = 39975 },
            rate = { a = 1, activation_energy_j_per_mol = 0 },
            reaction_enthalpy_j_per_mol = 44004,
        },
        -- 融解 ⇄ 凝固: 氷 ⇄ 水(273.15 K で釣り合う)
        ice_melting = {
            reactants = { ice = 1 },
            products = { water = 1 },
            balance = { temperature_mk = 273150, enthalpy_j_per_mol = 6010 },
            rate = { a = 1, activation_energy_j_per_mol = 0 },
        },
        -- Boudouard: C + CO2 → 2CO(ΔH = +172.5 kJ/mol。吸熱)
        boudouard = {
            reactants = { carbon = 1, carbon_dioxide = 1 },
            products = { carbon_monoxide = 2 },
            rate = { a = "1e10", activation_energy_j_per_mol = 250000 },
            reaction_enthalpy_j_per_mol = 172500,
        },
    },
}
