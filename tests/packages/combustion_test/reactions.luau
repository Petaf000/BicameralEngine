-- reactions.luau — 反応の規則。反応熱は書かない(ベイクが生成エンタルピーの差から決める。02 §2)。
-- reaction_enthalpy_j_per_mol は文献の反応熱(298.15 K)で、ベイクが生成エンタルピーの差と比べて食い違えば警告する(検査用)。
-- 速度(Arrhenius: k = A·exp(−Ea/RT)、A は SI):
--   セルロースの熱分解: Bradbury, Sakai & Shafizadeh(1979, J. Appl. Polym. Sci. 23, 3271)の開始反応
--     (セルロース → 活性セルロース、A = 2.8e19 /s・Ea = 242.4 kJ/mol)の値を、炭と水蒸気までの 1 段の反応に使っている(単純化)。
--   ほかの 4 本: 600〜1000 K で燃え始めるように置いた**仮の値**(現実の速度ではない。未確認)。
-- 反応物の次数は全部 1(orders を省いた)。
return {
    -- 酸素なしで炭と水蒸気になる(一次反応)
    cellulose_pyrolysis = {
        reactants = { cellulose = 1 },
        products = { carbon = 6, water_vapor = 5 },
        rate = { a = "2.8e19", activation_energy_j_per_mol = 242400 },
    },
    -- 木が燃える(O2 を炭の燃焼と取り合う)
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
    -- Boudouard: C + CO2 → 2CO(ΔH = +172.5 kJ/mol。吸熱で、熱を資源として取り合う場面)
    boudouard = {
        reactants = { carbon = 1, carbon_dioxide = 1 },
        products = { carbon_monoxide = 2 },
        rate = { a = "1e10", activation_energy_j_per_mol = 250000 },
        reaction_enthalpy_j_per_mol = 172500,
    },
}
