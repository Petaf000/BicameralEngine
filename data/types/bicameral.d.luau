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

-- --- 世界の組み立て(分類 world_cells・world_bodies。T-0398・ADR-0172。engine/src/script/world_layout_package.h)---
-- セルの領域: 箱の中を同じ中身で埋める。layer の小さい順に重ね書き(同じなら鍵の順)。場所を省けば格子の全部
export type WorldCellRegion = {
    layer: number?, -- 重ねる順(既定 0)
    min_cell: { number }?, -- { x, y, z }(セル)。size_cells と両方書くか両方省く
    size_cells: { number }?, -- { x, y, z }(セルの数)
    shell_cells: number?, -- 外側の殻の厚さ(セル。既定 0 = 中まで埋める)
    amounts_umol: { [string]: number }, -- 物質 → 1 セルの量(µmol)
    temperature_mk: number, -- 温度(mK)
}

-- 物(直方体)。物理の座標(y が上)。order の小さい順に番号が付く(同じなら鍵の順)
export type WorldBody = {
    order: number?, -- 番号を振る順(既定 0)
    size_mm: { number }, -- { x, y, z } 辺の長さ(mm)
    position_mm: { number }, -- { x, y, z } 重心(mm)
    mass_g: number, -- 質量(g)。0 = 動かない物(地面・壁)
}

-- --- 環境(分類 environment。T-0400・ADR-0176。1 つの鍵 = 1 つの値・整数で、単位は鍵の名前。書かない値は 0 = 無い)---
-- 読み手は engine/src/script/environment_package.cpp(範囲: 重力 0〜100 m/s²・音速の基準は気体の小刻みの安定の上限まで)
export type Environment = {
    gravity_um_per_s2: number?, -- 重力の強さ(µm/s²。地球 9806650)
    sound_speed_mm_per_s: number?, -- 気体の音速の基準(mm/s。D-440 の本物の音速 340000)
}

-- --- entry が返す表(分類 → 鍵 → 値。ADR-0031 の 1)。反応表のほかの分類(UI の文字列など)は形を決めていない ---
export type PackageEntry = {
    elements: { [string]: Element }?,
    species: { [string]: Species }?,
    reactions: { [string]: Reaction }?,
    world_cells: { [string]: WorldCellRegion }?,
    world_bodies: { [string]: WorldBody }?,
    environment: Environment?,
    [string]: any,
}
