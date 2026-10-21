# ADR-0395 物質の光の値の欄(2 つの帯)と、書かない値の決め方

- Status: Accepted(書かない物質の既定はユーザーの決定 D-610。欄の名前と照り返しの 0.5 は Claude の仮。T-0910)
- 日付: 2026-10-21
- 決めた人: ユーザー(D-610)・Claude(欄の名前・単位・読み方の順。仮)
- 設計: docs/design/23-radiation.md §2(D-571・ADR-0382)
- 置き換える: ADR-0252 の「書かなければ 0」(光の値として。放射の CPU リファレンス radiation_reference の欄 speciesRadiationAbsorption は今のまま残す)

## 背景
- 23 §2 は物質に帯ごとの吸い込み・照り返しを書かせると決めた(名前は仮)。R-R0(太陽の影の柱)が最初に読む。
- 書かない物質の扱いは ADR-0382 仮 1 だったが、D-610 でユーザーが「固体・液体は灰色で光を通さない・気体は透明」と決めた。

## 決定
1. **パッケージの欄**(物質。どれも任意。名前は仮):
   - `light_absorption_short_mm2_per_mol`・`light_absorption_long_mm2_per_mol`(1 mol が帯を遮る面積。ADR-0252 の a を帯ごとに 2 つにしたもの。0〜2^32−1)
   - `light_reflectance_short_permille`・`light_reflectance_long_permille`(当たった光のうち吸わずに返す割合。千分率 0〜1000)
   - 通しやすさは書かない(1 − 吸い込みで導く。同じことを 2 か所に書かせない)。滑らかさ・屈折の値は使う試作(R-R1・R-R2)で足す。
2. **書かない値の決め方**(sim::BakeSpeciesLight。上から順に):
   - 吸い込み: その帯の値 → もう片方の帯の値(灰色)→ `radiation_absorption_mm2_per_mol`(0 でなければ。前の欄との互換)→
     相の既定(固体・液体 = 表の上限 2^32−1 = 通さない・気体 = 0 = 透明。D-610)。
   - 照り返し: その帯の値 → もう片方の帯の値 → 光の値を 1 つも書かない固体・液体は 0.5(**値は仮**)・ほかは 0。
3. 値は CPU の表 `BakedReactionTable::speciesLight`(`LtSpeciesLight` 16 バイト。shaders/common/light.hlsli)に入る。GPU の反応の表(RxSpecies)は
   変えない(反応の表のハッシュ・ルート署名に触れない)。使う GPU の段(sim/gpu_sun_column)が自分の作業のバッファへ写す。
4. セルの値は `LtCellBandOf`: 帯ごとの Σ a·n(128bit)と、照り返し = 物質の照り返しを a·n で重み付けた平均(吸わない物質は効かない)。

## 作り手から見た違い(仮の所)
- 照り返し 0.5: 書き忘れた岩は日なたで入った光の半分を返す(R-R0 の今は返した量を数えるだけ。跳ね返りは R-R1 §3.4)。
  代案「0(全部吸う)」は書き忘れた岩が黒く熱くなる。値だけの違いなので後で変えても仕組みは変わらない。
- 書き忘れの警告は別の行(T-0942)。

## 影響
- data/types/bicameral.d.luau に 4 欄。既存のパッケージは光の値を書いていない → 固体・液体は通さない灰色・気体は透明になる
  (R-R0 の掃きを入れる刻みはまだ無いので、今の世界のふるまいは変わらない)。
- radiation_reference(ADR-0252 の対の交換)は speciesRadiationAbsorption を読むまま(黒い板の解析解の参照。23 §9)。
