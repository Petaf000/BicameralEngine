# ADR-0174 材料のひな形・抽出の物質・実験室と置く操作の既定をパッケージの分類にする

- Status: Accepted(2026-10-14 ユーザー決定)
- 日付: 2026-10-13
- 決めた人: Claude(T-0399。方向はユーザー決定 D-454〜D-456・D-486、読み込みの形は T-0398 の共有の決め事)

## 背景
- 実験室・筆・魔法のパッチのパネルの材料のひな形(空気・木・木炭・二酸化炭素・窒素・仮の魔素)と量、実験室の箱を満たす空気と 300 K、
  「置く」で初めに選ぶ材料(木)、抽出に写す物質(O2・CO2・炭)と描画の色の幅(空気の O2 など)が C++ と HLSL に書いてあった
  (sim/lab_box.cpp・sim/probe_sim.cpp・shaders/render/probe_view.hlsl・editor/*。T-0371 の洗い出し)。
- D-454(エンジンは中身を持たない)・D-455(ブランクの世界は本当に空)・D-486(中身は作り手と同じやり方で書く)により、これらをパッケージへ移す。

## 決定
1. **分類を 3 つ足す**(反応表と同じ読み込み・同じ表の版。T-0398 の共有の決め事 1):
   - `material_templates`: 鍵 = 材料。`label`(パネルの名前)・`order`(並び。省けば 0。同じなら鍵の順)・
     `contents`(`{ species, micromoles }` の列。1〜3 個・書いた順。量は 1 セル〔0.5 m 角〕あたりの µmol)。
   - `view_quantities`: 鍵 `slot_1`〜`slot_3` = 抽出に写す物質(エンジンの器は 3 枠。common/probe_sim.hlsli の PROBE_VIEW_SPECIES_COUNT)。
     `label`(表示の「色分けする量」の名前)・`species`・`span_micromoles`(色が 1 になる量)・`depletion`(true なら 幅 − 量 を塗る)。
   - `editor_defaults`: `lab_fill`(実験室の箱を満たす材料の鍵)・`place_material`(「置く」の初めの材料の鍵)・
     `temperature_mk`(箱を満たす温度と置く温度の初め。lab_fill を書いたら要る)。
2. **読む所とベイク**: script/material_templates_package(形の誤り)→ ReactionTableDefinition::templates →
   sim::BakeMaterialTemplates(BakeReactionTable の最後。名前 → ID。知らない物質・知らない材料の鍵・量 0・同じ物質を 2 回・
   4 つ以上は誤り = 表の読み込みが落ちる)→ BakedReactionTable::templates(CPU だけ)。
3. **使う所**: sim::MakeLabMaterials(表の templates の写し。LabMaterial に key を足し、name は label)・sim::MakeLabBoxNest
   (lab_fill の材料と温度。無ければ空のセル)・sim::ProbeViewSpecies(slot の物質。無い枠は 0)・描画(ProbeViewConstants を 112 バイトにし、
   枠ごとの幅を speciesSpans、減りの枠を flags のビット 12〜14 で渡す。幅 0 の枠は塗らない)・パネルの初めの材料と温度。
4. **エンジンのソースから中身の名前を消す**: 材料の名前と量・空気の成分・300 K(実験室)・O2 の基準量・CO2 と炭の幅・表示の量の名前(「O2 の減り」など)。
   残すもの: 自動の確かめ(--auto-lab・--auto-place・--auto-patch・--auto-lab-compare)はサンプルの世界の試験の道具なので、
   サンプルのパッケージの鍵(`wood`・`charcoal`・`cellulose` など)で探す(frame/auto_reload と同じ扱い)。
   魔法のパッチのパネルの範囲の中心の初め(前は仮の世界の木箱の壁 28,32,32)は格子の真ん中にした。

## 捨てた案
- 「別の読み込み(版に入れない)」 — 実験室の初めの箱は表で決まる(lab_fill)ので、再生ファイルの表の中身から同じ箱を作り直せる必要がある(T-0398 の 1)。
- 「表に無い物質を指す材料は黙って除く」(前の MakeLabMaterials の振る舞い) — 綴りの誤りを黙って通さない(ADR-0032 と同じ)。
  試験の中で物質を消す道具は tests/reaction_test_table の PruneMaterialTemplates で指す中身を除く(共有の決め事 4)。
- 「抽出の物質の数をパッケージで決める」 — 抽出の形(4 語/セル)と GPU の段を変える。今は器の 3 枠のまま(必要になったら別チケット)。

## 影響
- サンプルのパッケージ(combustion_test)に material_templates.luau・view_quantities.luau・editor_defaults.luau。型の定義(bicameral.d.luau)。
- 表の版の値が変わる(版の値を書き込んだ試験は無い)。材料だけを直しても版が変わり、ホットリロードは表の差し替えを入れる(世界は変わらない)。
- 設計: 13 §2.6・14 §2(実験室・筆・パッチ)。

## ユーザーの決定(2026-10-14)
- D-488 ②: 材料・表示の物質が表に無い物質を指したら読み込みごと止める。
