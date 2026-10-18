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
    title: string?, -- 人に見せる名前(環境ミキサーの一覧。T-0373)
    depends: { string }?, -- 依存するパッケージの名前
    entry: string?, -- 表を返すファイル(既定 "init")
    game: boolean?, -- ゲーム本体なら true(読む一覧の中に 1 つまで。名前に依らない。T-0557・ADR-0250)
}

-- --- 反応表(分類 elements・species・reactions。ADR-0032。値は整数で、単位は欄の名前)---
export type Element = {
    atomic_mass_mg_per_mol: number,
}

export type Species = {
    composition: { [string]: number }, -- 元素 → 個数
    formation_enthalpy_j_per_mol: number,
    heat_capacity_mj_per_mol_k: number, -- 比熱(一定と置く)。気体は資料の定圧の比熱 c_p をそのまま書く(ベイクがセルの読む定積の c_v = c_p − R を作る。R 以下は断る。T-0597)。凝縮相はそのまま
    thermal_conductivity_mw_per_m_k: number,
    standard_entropy_mj_per_mol_k: number?, -- 標準モルエントロピー(298.15 K・1 bar)。省ける。坂の検査だけが読む(T-0491)
    condensed: boolean?, -- 廃止(T-0710。phase で書く)。読み込みが true = "solid"・false = "gas" と読み替えて警告する。phase と食い違えば誤り
    radiation_absorption_mm2_per_mol: number?, -- 1 mol が放射を遮る面積(mm²/mol)。省ける(既定 0 = 放射しない・吸わない)。セルの放射率 = 1 − e^−Σ a·n ÷ 面(T-0583・ADR-0252)
    phase: string?, -- 相("gas"・"solid"・"liquid" の 1 本。省ける = 気体)。どの流れがこの物質を運ぶかを決める(T-0609・T-0710)
    density_kg_per_m3: number?, -- 密度。液体には要る(無ければ誤り)。固体も書く(セルの場所を取る。書かないとベイクが警告する。ADR-0268・T-0616)
    -- 動き方の任意の値(T-0710・土台の決定 3。名前と単位は仮)。書かなければ今と同じ動き。相に合わない欄はベイクが誤りにする
    viscosity_upa_s: number?, -- 粘り(µPa·s。1 以上の整数。水 890・空気 18)。液体と気体に書ける。液体に書かないとベイクが警告する
    diffusivity_mm2_per_s: number?, -- 気体の混ざる速さ(mm²/s。0 以上の整数。湯気 25)
    permeability_um2: (number | string)?, -- 固体の孔の通しやすさ(µm²。0〜1e7 を 1e-6 刻み。小さい値は 10 進の文字列 "1e-6"・"0.001"。砂 10)
    suction_pa: number?, -- 固体の吸い上げ(Pa。0 以上の整数。乾いた砂 5000)
}

export type ReactionRate = {
    a: number | string, -- 頻度因子(SI)。桁が大きいので 10 進の文字列 "2.8e19" でもよい
    activation_energy_j_per_mol: number,
}

-- 釣り合い(T-0623・ADR-0290)。書いた規則は 1 本で両向きに進み、釣り合えば止まる(ベイクが逆向き「名前~逆」を作る)。
-- その温度で、気体の項の濃さの比(生成物 ÷ 反応物。係数乗)が concentration^Δn で釣り合う(Δn = 気体の係数の差)。凝縮相の項は入らない。
-- 温度で ΔH_eq の急さで動く(van 't Hoff)。rate は「釣り合いへ近づく速さ」(1/s)
export type ReactionBalance = {
    temperature_mk: number, -- 釣り合いの温度(mK)。凝縮相だけの規則(融解)なら融点
    enthalpy_j_per_mol: number, -- ΔH_eq(正なら温めると生成物の側へ)
    concentration_mol_per_m3: (number | string)?, -- 釣り合いの濃さ。気体の係数が両辺で違う時に要る
}

export type Reaction = {
    reactants: { [string]: number }, -- 物質 → 係数
    products: { [string]: number },
    rate: ReactionRate,
    balance: ReactionBalance?, -- 釣り合いの規則(orders は書けない)
    orders: { [string]: number }?, -- 物質 → 次数(0 か 1)。省けば反応物は全部 1。書かない反応物は 0。全部 0 なら A は mol/(m³·s)(T-0611)
    reaction_enthalpy_j_per_mol: number?, -- 文献の反応熱(検査用。食い違えばベイクが警告)
}

-- --- 世界の組み立て(分類 world_cells・world_bodies。T-0398・ADR-0172。engine/src/script/world_layout_package.h)---
-- セルの領域: 箱の中を同じ中身で埋める。layer の小さい順に重ね書き(同じなら鍵の順)。場所を省けば格子の全部
export type WorldCellRegion = {
    layer: number?, -- 重ねる順(既定 0)
    min_cell: { number }?, -- { x, y, z }(セル)。size_cells と両方書くか両方省く
    size_cells: { number }?, -- { x, y, z }(セルの数)
    shell_cells: number?, -- 外側の殻の厚さ(セル。既定 0 = 中まで埋める)
    amounts_umol: { [string]: number }, -- 物質 → 1 セルの量(µmol)。合計 2^36(約 68,719 mol)まで・気体(phase が gas)の物質の合計は 2^31(約 2147 mol)未満(ADR-0260)
    temperature_mk: number, -- 温度(mK)
}

-- 物(直方体)。物理の座標(y が上)。order の小さい順に番号が付く(同じなら鍵の順)
export type WorldBody = {
    order: number?, -- 番号を振る順(既定 0)
    size_mm: { number }, -- { x, y, z } 辺の長さ(mm)
    position_mm: { number }, -- { x, y, z } 重心(mm)
    mass_g: number, -- 質量(g)。0 = 動かない物(地面・壁)
}

-- 気体・液体を流す箱(前の分類 gas_regions・liquid_regions)は書かない(D-510・T-0724): 気体は基準の大気があれば・液体は重力があれば
-- 世界のどこでも流れる。書いてあればエンジンは読まずに注意を出す

-- --- 材料のひな形・抽出の物質・既定(分類 material_templates・view_quantities・editor_defaults。T-0399)---
-- 読み手は engine/src/script/material_templates_package.h。量は 1 セル(0.5 m 角)あたりの µmol
export type MaterialAmount = {
    species: string, -- 物質の名前
    micromoles: number,
}

export type MaterialTemplate = {
    label: string, -- パネルに出す名前
    order: number?, -- パネルの並び(小さい順。同じなら鍵の順)
    contents: { MaterialAmount }, -- 1〜3 個。書いた順に置く
}

export type ViewQuantity = {
    label: string, -- 表示の「色分けする量」の名前
    species: string, -- 抽出に写す物質
    span_micromoles: number, -- この量で色が 1
    depletion: boolean?, -- true なら 幅 − 量 を塗る(減り)
}

export type ViewQuantities = {
    slot_1: ViewQuantity?,
    slot_2: ViewQuantity?,
    slot_3: ViewQuantity?,
}

export type EditorDefaults = {
    lab_fill: string?, -- 実験室の箱を満たす材料(material_templates の鍵)
    place_material: string?, -- 「置く」で初めに選ぶ材料
    temperature_mk: number?, -- 箱を満たす温度・置く温度の初め(mK。lab_fill を書いたら要る)
}

-- --- 環境(分類 environment。T-0400・ADR-0176。1 つの鍵 = 1 つの値・整数で、単位は鍵の名前。書かない値は 0 = 無い)---
-- 読み手は engine/src/script/environment_package.cpp(範囲: 重力 0〜100 m/s²・音速の基準は気体の小刻みの安定の上限まで)
-- 基準の大気の高さの 1 つの点(T-0499・ADR-0182 の改訂。点の間は直線でつなぐ。端の点の外は端の値)
export type AtmospherePoint = {
    height_mm: number, -- 海面からの高さ(mm。整数・±2^40。前の点より高く)
    temperature_mk: number, -- 温度(mK)
    composition: { [string]: number }, -- 成分(物質 → ppm。足して 1000000。0 は書かない)
}

export type Environment = {
    gravity_um_per_s2: number?, -- 重力の強さ(µm/s²。地球 9806650)
    sound_speed_mm_per_s: number?, -- 気体の音速の基準(mm/s。D-440 の本物の音速 340000)
    -- 気体の格子で解けない乱れの粘り(動粘性 mm²/s。0〜1875000。T-0758・D-516。名前と単位は仮)。乱れた風と残響が静まる速さ。
    -- 書かない = 0(静まらない)。6 m の部屋の揺れが e 分の 1 になる時間 ≒ 3.6 s ÷ (値 ÷ 10^6)。一様な風は減らない。
    -- 標準パッケージ earth_environment は仮に 200000(0.2 m²/s。T-0775。値は ⑤ の場面で決める)
    gas_eddy_viscosity_mm2_per_s: number?,
    atmosphere_surface_pressure_pa: number?, -- 基準の大気の一番低い点の圧力(Pa。ほかの高さはエンジンが重力と温度から静水圧で出す)
    -- 基準の大気の高さの点の列(低い順。T-0499)。点の形は AtmospherePoint だが、ここでは any で受ける: 点を表の中にそのまま書くと
    -- 型検査が点ごとに別の型を作り({ nitrogen: number } は { [string]: number } と「ちょうど同じ」でない)誤りにするため。
    -- 欄の誤り(知らない欄・整数でない・範囲の外)は読み込み(environment_package.cpp)が場所付きで止める
    atmosphere_profile: { any }?,
    atmosphere_temperature_mk: number?, -- 一様な大気の温度(mK。T-0373 の書き方。分類 atmosphere と一緒に。atmosphere_profile とは書けない)
    world_edges: WorldEdges?, -- 世界の端(T-0710・土台の決定 5。外との境は世界の端だけ。書かない面は wall)
}

-- 世界の端の面: "open"(外は基準の大気。流れが出入りする)か "wall"(閉じた壁。既定)
export type WorldEdges = {
    top: string?, -- 上(空)
    bottom: string?, -- 下(地の底)
    sides: string?, -- 横の 4 面
}

-- --- 遊びの指標(分類 play_metrics。T-0432・ADR-0206。1 つの鍵 = 1 つの指標。読み手は engine/src/script/play_metrics_package.h)---
-- 場面は world_cells の初めの世界。つつきを当て、量が at_most 以下(at_least 以上)になるまで写しを早送りして刻みを測る
export type PlayMetricPoke = {
    tick: number, -- 当てる刻み(0 から。max_ticks より前)
    cell: { number }, -- { x, y, z }(セル)
}

export type PlayMetric = {
    label: string?, -- 表示の名前(省けば鍵)
    max_ticks: number, -- 早送りの上限(刻み。sample_interval_ticks の倍数)
    sample_interval_ticks: number, -- 量を測る間隔(刻み)
    pokes: { PlayMetricPoke }?, -- 場面に当てるつつき(セルを温める)
    quantity: string, -- 測る量: "max_temperature"(mK)・"cells_at_or_above"(セル)・"species_amount"(µmol)
    min_cell: { number }?, -- 量を測る範囲 { x, y, z }(セル)。size_cells と両方書くか両方省く(省けば世界全体。T-0559)
    size_cells: { number }?, -- 範囲の大きさ { x, y, z }(セルの数)
    temperature_mk: number?, -- cells_at_or_above のしきい値の温度(この量だけ)
    species: string?, -- species_amount の物質(この量だけ)
    at_most: number?, -- 量がこれ以下で止める(at_least と 1 つまで。どちらも無ければ上限の量を測る)
    at_least: number?,
}

-- --- 流れどうしの結合(分類 drags・machines。T-0536・ADR-0238。1 つの鍵 = 1 つの結合。読み手は engine/src/script/coupling_package.h)---
-- 2 つの分類は同じ型・同じベイク・同じ検査(|q| > 1 は警告して ±1 に押さえる)。流れは "heat"・"work"・"gas"・"species:<物質>"。
-- 書き方は仮(ユーザー未確認。QUESTIONS Q66・Q70)
export type Drag = {
    flow: string, -- 流れ X("heat"・"species:<物質>")
    drags: string, -- 引きずられる量 Y("heat"・"species:<物質>")
    coupling: number | string, -- 結合の強さ q(−1〜1)。小数は 10 進の文字列 "0.3"・"-0.5"
    within: string?, -- 効く物質(省けばどこでも)
}

export type Machine = {
    store: string, -- 蓄え X("work"・"heat"・"species:<物質>")
    push: string, -- 押す物 Y("gas"・"heat"・"species:<物質>")
    coupling: number | string, -- 結合の強さ q(1 = きっちり結合)。小数は 10 進の文字列 "0.9"
    cost_j_per_mol: number, -- w: Y を 1 mol 押すのに使う蓄え(J/mol)
    speed_w: number, -- P: 負荷のない時に蓄えを使う速さ(W)
}

-- --- entry が返す表(分類 → 鍵 → 値。ADR-0031 の 1)。反応表のほかの分類(UI の文字列など)は形を決めていない ---
export type PackageEntry = {
    elements: { [string]: Element }?,
    species: { [string]: Species }?,
    reactions: { [string]: Reaction }?,
    world_cells: { [string]: WorldCellRegion }?,
    world_bodies: { [string]: WorldBody }?,
    material_templates: { [string]: MaterialTemplate }?,
    view_quantities: ViewQuantities?,
    editor_defaults: EditorDefaults?,
    environment: Environment?,
    atmosphere: { [string]: number }?, -- 一様な大気の成分(物質 → ppm。足して 1000000。T-0373。高さで変えるなら environment.atmosphere_profile)
    play_metrics: { [string]: PlayMetric }?, -- 遊びの指標(T-0432)
    drags: { [string]: Drag }?, -- 引きずり(T-0536)
    machines: { [string]: Machine }?, -- 押す構造(T-0536)
    [string]: any,
}
