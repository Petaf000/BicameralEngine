-- init.luau — 現実に則した反応の規則。反応熱は書かない(ベイクが生成エンタルピーの差から決める。02 §2)。
-- reaction_enthalpy_j_per_mol は文献の反応熱(298.15 K)で、ベイクが生成エンタルピーの差と比べて食い違えば警告する(検査用)。
-- 速度(Arrhenius: k = A·exp(−Ea/RT)、A は SI):
--   セルロースの熱分解: Bradbury, Sakai & Shafizadeh(1979, J. Appl. Polym. Sci. 23, 3271)の開始反応
--     (A = 2.8e19 /s・Ea = 242.4 kJ/mol)を、炭と水蒸気までの 1 段の反応に使っている(単純化)。
--   ほかの 4 本: 600〜1000 K で燃え始めるように置いた値で、文献の速度ではない(未確認。サンプルの世界 combustion_test もこの値を使う)。
--     ⑤ の場面(T-0414・T-0416)で文献の値に替える。
-- 反応物の次数は、書いていなければ全部 1(orders を省いた)。
--
-- 水の相変化(T-0611・ADR-0272)。向きは両方とも書く(D-487 ① 案 A)。潜熱は物質の生成エンタルピーの差から出る(書かない)。
--   凝縮相は量によらず活量 1 なので、液体の水と氷は次数 0(あれば効く)。進みは mol/(m³·s) × セルの体積。
--   - 蒸発と凝縮: 釣り合いの湯気の濃さ c = A_蒸発 exp(−Ea/RT) ÷ k_凝縮 が、飽和水蒸気の濃さ P_sat / RT になるように置いた。
--     P_sat = 3169.9 Pa(298.15 K)と 101.418 kPa(373.15 K)(IAPWS-95。Wagner & Pruss, J. Phys. Chem. Ref. Data 31 (2002) 387)
--     から 2 点で合わせ、Ea = 40 kJ/mol(合わせた値は 39.975)・k_凝縮 = 1 /s(湯気の行き過ぎが 1 秒で戻る。速さの値は仮)。
--     273.15 K では 0.292 mol/m³(本当は 0.269。+8%)・500 K で 863 mol/m³。
--     凝縮は水があるセルでだけ起きる(水を触媒として書いた)。水も氷も無い所で過飽和になっても霧にはならない(仮。ADR-0272 の注)。
--   - 融解と凝固: どちらも氷について次数 1(氷の量に比例。正味 = 氷 × (k_融解 − k_凝固) で、向きが量によらず温度だけで決まる)。
--     凝固は水のあるセルでだけ(水は次数 0)・氷があるセルでだけ(種の氷が無いと凍り始めない。仮)、k = 1 /s。
--     融解は Ea = 600 kJ/mol の急な Arrhenius で、273.15 K で凝固と等しい(A = 1 × exp(600000 / (R × 273.15)))。
--     1 K 上で融解は凝固の 2.6 倍。熱が足りなければ融点に張り付く。Ea は「融点のまわりの切り替わりの鋭さ」で、現実の値ではない。
--     (凝固を次数 0〔一定の速さ〕にすると、熱いセルでも同じ刻みの凝固が氷を作り直し、氷が少し残り続けて溶けた水が流れなかった。T-0611 の測り)
--     速さの表は 1 K ごと(下へ切り捨て)なので、張り付く温度は 274 K の手前(0.85 K 高い。ADR-0272 の注)。
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
        -- 蒸発: 水 → 湯気(ΔH = +44.0 kJ/mol。吸熱)
        water_evaporation = {
            reactants = { water = 1 },
            products = { water_vapor = 1 },
            orders = { water = 0 },
            rate = { a = "1.3016e7", activation_energy_j_per_mol = 40000 },
            reaction_enthalpy_j_per_mol = 44004,
        },
        -- 凝縮: 湯気 → 水(水のあるセルで。湯気について次数 1)
        water_condensation = {
            reactants = { water_vapor = 1, water = 1 },
            products = { water = 2 },
            orders = { water_vapor = 1 },
            rate = { a = 1, activation_energy_j_per_mol = 0 },
        },
        -- 融解: 氷 → 水(273.15 K で凝固と釣り合う)
        ice_melting = {
            reactants = { ice = 1 },
            products = { water = 1 },
            rate = { a = "5.4466e114", activation_energy_j_per_mol = 600000 },
        },
        -- 凝固: 水 → 氷(水と氷のあるセルで。氷について次数 1)
        water_freezing = {
            reactants = { water = 1, ice = 1 },
            products = { ice = 2 },
            orders = { ice = 1 },
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
