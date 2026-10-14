-- species.luau — サンプルの世界の物質(1 mol あたり)。
-- 現実の物質は標準パッケージ real_matter の値を写し、熱伝導率だけに試験の倍率を掛けて書き直す(遊びのための値。units.luau)。
-- ほかの欄(組成・生成エンタルピー・比熱と、real_matter に足された欄)は現実の値のまま写す(値は real_matter の 1 か所。出典もそこ)。
-- 依存の標準パッケージの鍵をゲーム本体が書き直すのは、作り手が自分の世界の値を決める形で、「改造された世界」の印は付かない
-- (ADR-0183 の追記・T-0498)。require("@real_matter") は real_matter の表の写し(書き換えても real_matter の表は変わらない)。
local units = require("units")
local realMatter: any = require("@real_matter") -- 依存の表の写し(物質の表は下で型を付けて受ける)
local realSpecies: { [string]: Species } = realMatter.species

-- 倍率は固体と気体で違う(units.luau)。固体はこの一覧、ほかは気体として扱う(固体を real_matter に足したら、ここにも足す)
local SOLIDS: { [string]: boolean } = {
    cellulose = true,
    carbon = true,
}

-- 足す物質: 触るための仮の魔素(気体。T-0225・D-449・D-451)。エディタで値を動かして試すための試験用の物質で、ゲームの中身ではない(D-006)。
-- 値は動きが見えるだけの仮の値: 生成エンタルピー 0(元素の単体)・比熱は単原子の理想気体の 5/2 R・熱伝導率は気体並み。
-- 本当の魔素(性質・反応・湧き出し)は非公開側で決める
local species: { [string]: Species } = {
    mana_test = {
        composition = { mana_test = 1 },
        formation_enthalpy_j_per_mol = 0,
        heat_capacity_mj_per_mol_k = 20786,
        thermal_conductivity_mw_per_m_k = units.gas_conductivity(20),
    },
}

-- 書き直す物質: 現実の物質を全部写し、熱伝導率だけに倍率を掛ける
for name, real in realSpecies do
    local scaled = table.clone(real)
    local conductivity = real.thermal_conductivity_mw_per_m_k
    scaled.thermal_conductivity_mw_per_m_k = if SOLIDS[name]
        then units.solid_conductivity(conductivity)
        else units.gas_conductivity(conductivity)
    species[name] = scaled
end

return species
