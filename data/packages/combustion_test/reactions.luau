-- reactions.luau — サンプルの世界が足す反応の規則。現実の反応(セルロースの熱分解と燃焼・炭の燃焼・Boudouard)は標準パッケージ
-- real_reactions(速さの値と出典もそこ)。反応熱は書かない(ベイクが生成エンタルピーの差から決める。02 §2)。
-- 速度(Arrhenius: k = A·exp(−Ea/RT)、A は SI)。
return {
    -- --- 触るための仮の魔素の反応(T-0225・D-449・D-451)---
    -- 試験用の仮の反応で、ゲームの中身ではない(D-006)。速度は動きが見えるだけの仮の値。本当の魔素の反応は非公開側で決める。
    -- どちらも魔素を触媒として書く(D-451「操る = 触媒として書く」の形だけ借りる): 魔素は両辺に 1 ずつで減らず、次数 0(あれば効く)。
    -- 1 刻みに進む量は、セルの魔素の量までに抑えられる(反応物として引いてから足し戻すため)。
    -- 魔素があると木が低い温度で燃える(活性化エネルギーを下げた cellulose_combustion。魔素なしは約 600 K、ありは約 400 K から)
    mana_test_catalyzed_cellulose_combustion = {
        reactants = { cellulose = 1, oxygen = 6, mana_test = 1 },
        products = { carbon_dioxide = 6, water_vapor = 5, mana_test = 1 },
        orders = { cellulose = 1, oxygen = 1 },
        rate = { a = "2e10", activation_energy_j_per_mol = 100000 },
    },
    -- 魔素があると炭が CO2 を CO に変える吸熱の反応が低い温度で進む(活性化エネルギーを下げた boudouard。熱を奪う向き)
    mana_test_catalyzed_boudouard = {
        reactants = { carbon = 1, carbon_dioxide = 1, mana_test = 1 },
        products = { carbon_monoxide = 2, mana_test = 1 },
        orders = { carbon = 1, carbon_dioxide = 1 },
        rate = { a = "1e10", activation_energy_j_per_mol = 170000 },
    },
}
