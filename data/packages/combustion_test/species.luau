-- species.luau — 物質(1 mol あたり)。生成エンタルピーは 298.15 K の値(J/mol)、比熱は一定と置く(mJ/(mol·K))。
-- 出典:
--   気体と炭(黒鉛)の生成エンタルピー: CODATA Key Values for Thermodynamics(1989)の値を丸めたもの
--     (CO2 −393.51・CO −110.53・H2O(気)−241.826 kJ/mol。元素の単体 O2・N2・黒鉛は 0)。
--   比熱: NIST-JANAF 熱化学表の 298.15 K の Cp°(O2 29.378・N2 29.124・CO2 37.135・CO 29.142・H2O(気)33.58・黒鉛 8.52 J/(mol·K))。
--     ※ 値は T-0014 の C++ の表から写した。この T-0021 では表と一つずつ照合していない(未確認)。
--   セルロース(C6H10O5 の 1 単位あたり): 生成エンタルピー約 −963 kJ/mol は燃焼熱からの逆算、比熱は約 1.3 J/(g·K)(どちらも未確認)。
--     この値だと熱分解が発熱になる(実際は生成物が複雑でほぼ中立)。
--   熱伝導率: 現実の値(木 120・炭 100・気体 17〜27 mW/(m·K)。未確認)に試験の倍率を掛けたもの(units.luau)。
local units = require("units")
local solid = units.solid_conductivity
local gas = units.gas_conductivity

return {
    cellulose = {
        composition = { C = 6, H = 10, O = 5 },
        formation_enthalpy_j_per_mol = -963000,
        heat_capacity_mj_per_mol_k = 211000,
        thermal_conductivity_mw_per_m_k = solid(120),
    },
    carbon = {
        composition = { C = 1 },
        formation_enthalpy_j_per_mol = 0,
        heat_capacity_mj_per_mol_k = 8520,
        thermal_conductivity_mw_per_m_k = solid(100),
    },
    oxygen = {
        composition = { O = 2 },
        formation_enthalpy_j_per_mol = 0,
        heat_capacity_mj_per_mol_k = 29378,
        thermal_conductivity_mw_per_m_k = gas(27),
    },
    nitrogen = {
        composition = { N = 2 },
        formation_enthalpy_j_per_mol = 0,
        heat_capacity_mj_per_mol_k = 29124,
        thermal_conductivity_mw_per_m_k = gas(26),
    },
    carbon_dioxide = {
        composition = { C = 1, O = 2 },
        formation_enthalpy_j_per_mol = -393509,
        heat_capacity_mj_per_mol_k = 37135,
        thermal_conductivity_mw_per_m_k = gas(17),
    },
    carbon_monoxide = {
        composition = { C = 1, O = 1 },
        formation_enthalpy_j_per_mol = -110527,
        heat_capacity_mj_per_mol_k = 29142,
        thermal_conductivity_mw_per_m_k = gas(25),
    },
    water_vapor = {
        composition = { H = 2, O = 1 },
        formation_enthalpy_j_per_mol = -241826,
        heat_capacity_mj_per_mol_k = 33580,
        thermal_conductivity_mw_per_m_k = gas(25),
    },
    -- 触るための仮の魔素(気体。T-0225・D-449・D-451)。エディタで値を動かして試すための試験用の物質で、ゲームの中身ではない(D-006)。
    -- 値は動きが見えるだけの仮の値: 生成エンタルピー 0(元素の単体)・比熱は単原子の理想気体の 5/2 R・熱伝導率は気体並み。
    -- 本当の魔素(性質・反応・湧き出し)は非公開側で決める
    mana_test = {
        composition = { mana_test = 1 },
        formation_enthalpy_j_per_mol = 0,
        heat_capacity_mj_per_mol_k = 20786,
        thermal_conductivity_mw_per_m_k = gas(20),
    },
}
