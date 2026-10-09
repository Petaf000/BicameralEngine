-- bicameral.d.luau — パッケージを書く人が使う型(T-0140・ADR-0046)。Luau の定義ファイル(型と declare だけを書く)。
--
-- エンジンはパッケージを読む前に、この型でパッケージの Luau を型検査する(Luau.Analysis。engine/src/script/luau_type_check.*)。
--   package.luau が返す表は PackageManifest、entry のファイル(既定 init.luau)が返す表は PackageEntry として確かめる。
--   ほかのファイルも全部 strict で検査する(ファイルの先頭に --!nonstrict と書けば、そのファイルだけ緩められる)。
-- 返す表は欄ごとに突き合わせる: 省けない欄が無い・型の定義に無い欄(綴りの誤り)・欄の型の誤りを、その欄を書いた行に出す。
-- 整数か・範囲の中かは型では書けないので、読み込みの時に読み手が確かめる
--   (engine/src/script/luau_package.cpp・reaction_package.cpp。ADR-0031・ADR-0032)。
-- 読み手の欄を変えたら、ここも変える(luau_type_check_test が、試験の表のパッケージがこの型で通ることを確かめる)。
-- エディタ(luau-lsp など)の定義ファイルとしても使える。自分のファイルで型を付けると、誤りがその行に出る:
--   local species: { [string]: Species } = { ... }

-- --- マニフェスト(package.luau が返す表。ADR-0031 の 1)---
export type PackageManifest = {
    format: number, -- この版が読める形は 1
    depends: { string }?, -- 依存するパッケージの名前
    entry: string?, -- 表を返すファイル(既定 "init")
}

-- --- 反応表(分類 elements・species・reactions。ADR-0032。値は整数で、単位は欄の名前)---
export type Element = {
    atomic_mass_mg_per_mol: number,
}

export type Species = {
    composition: { [string]: number }, -- 元素 → 個数
    formation_enthalpy_j_per_mol: number,
    heat_capacity_mj_per_mol_k: number,
    thermal_conductivity_mw_per_m_k: number,
}

export type ReactionRate = {
    a: number | string, -- 頻度因子(SI)。桁が大きいので 10 進の文字列 "2.8e19" でもよい
    activation_energy_j_per_mol: number,
}

export type Reaction = {
    reactants: { [string]: number }, -- 物質 → 係数
    products: { [string]: number },
    rate: ReactionRate,
    orders: { [string]: number }?, -- 物質 → 次数(0 か 1)。省けば反応物は全部 1
    reaction_enthalpy_j_per_mol: number?, -- 文献の反応熱(検査用。食い違えばベイクが警告)
}

-- --- entry が返す表(分類 → 鍵 → 値。ADR-0031 の 1)。反応表のほかの分類(UI の文字列など)は形を決めていない ---
export type PackageEntry = {
    elements: { [string]: Element }?,
    species: { [string]: Species }?,
    reactions: { [string]: Reaction }?,
    [string]: any,
}
