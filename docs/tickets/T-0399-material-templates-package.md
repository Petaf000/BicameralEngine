# T-0399 実験室・筆・パネルの材料のひな形と抽出の物質をパッケージから読む

- Status: Doing
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: D-471 ② 中身の器(docs/plan/ROADMAP.md。T-0371 から分けた)
- 2026-10-13 に新しい指針に合わせて見直した: エンジンは中身を持たない(D-454・D-464)。サンプル「現実」は ⑤ の場面に要る分だけ(D-471 ②)。
  中身は作り手と同じやり方(Luau のパッケージ)で書く(D-486)。ブランクの世界(D-455)では何も読まずに動く。物理定数は D-480・ADR-0171(Accepted)の形に揃える
- 設計: docs/design/13-scripting-game-model.md §2.6・14 §2 / 決定: D-454〜D-456・ADR-0151(追記 1)・ADR-0174

## 目的(1〜2 行)
sim::MakeLabMaterials(空気・木・木炭…)・実験室の空気で満たす既定・筆とパッチのパネルの既定の材料(木・cellulose)・抽出の物質(ProbeViewSpecies)と描画の O2 の基準量を、パッケージの値にする。

## 完了条件(チェックできる形で)
- [x] 材料のひな形の分類・型の定義・読む所。ブランクの世界ではひな形が空でパネルが壊れない(window_lab_blank)
- [x] パネルの自動の確かめ(--auto-lab・--auto-place・--auto-patch)がサンプルのパッケージのひな形で通る(window_lab・window_lab_compare・window_place_record/play・window_patch_record/play。release)
- [x] 今の試験(--world sample・サンプルのパッケージを読む試験)の結果(ハッシュ・画像)が変わらない(image_probe_volume・image_probe_slice・gpu_lab_box・gpu_probe_sim・gpu_probe_place・gpu_probe_peek・CPU の 22 本。release)

## メモ・参考
- 洗い出しの一覧は T-0371 のチケット。合わせた表の版(TableVersion)は全部の分類を含むので、反応表と同じ読み込みに新しい分類を足すと
  表の版が変わる。世界の組み立ては別の読み込み(または版に入れない分類)にするかをここで決める。

## やったこと(2026-10-13 wt3)
- 分類 `material_templates`(材料: label・order・contents の列)・`view_quantities`(slot_1〜3: 抽出に写す物質・表示の名前・色が 1 になる量・減り)・
  `editor_defaults`(lab_fill・place_material・temperature_mk)。T-0398 の共有の決め事に従い反応表と同じ読み込み・同じ版
  (script/material_templates_package → ReactionTableDefinition::templates → sim::BakeMaterialTemplates → BakedReactionTable::templates)。ADR-0174。
- エンジンのソースから消した: 材料の名前と量(lab_box.cpp)・実験室の空気と 300 K(MakeLabBoxNest・LAB_INITIAL_TEMPERATURE_MILLIKELVIN)・
  ProbeViewSpecies の名前・probe_view.hlsl の O2 の基準量と CO2・炭の幅・表示の量の名前・パネルの初めの材料(添字 1 = 木)・パッチの中心 28,32,32。
- サンプルのパッケージ combustion_test に 3 ファイル(値は前の C++ と同じ。材料の成分の順も同じ)。型の定義 bicameral.d.luau。
- 自動の確かめはサンプルのパッケージの鍵(wood・charcoal・mana_test・air・carbon_dioxide)で探す。試験の中で物質を消す道具は
  tests/reaction_test_table の PruneMaterialTemplates(共有の決め事 4)。

## 判断待ち(仮で進めたもの。作り手から見た違い)
1. **材料・抽出の枠が表に無い物質を指したら、表の読み込みごと落とす**(仮)。作り手から見ると: 物質を消した・綴りを誤った時、
   起動(またはホットリロード)が「material_templates.wood: 知らない物質 cellulose」で止まる(前の C++ は黙ってその材料を一覧から外した)。
   案 B「その材料だけ一覧から外し、警告を出す」は、Mod で物質を消した時に材料のパッケージを直さなくて済むが、綴りの誤りが見えにくい。おすすめ A(今の形)。
2. **抽出に写す物質は 3 枠まで**(仮。エンジンの器)。作り手から見ると: パッケージで選べるのは「どの 3 つの物質を表示で色分けできるか」と名前・幅だけ。
   4 つ以上を見たいなら抽出の形を変える別チケット。おすすめ: 今は 3 枠(要望が出たら広げる)。
3. **材料だけを直しても表の版が変わる**(T-0398 の共有の決め事 1 のとおり)。作り手から見ると: エディタで材料の .luau を保存すると
   表の差し替え(ホットリロード)が 1 回起きる(世界は変わらない)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-13(wt3): 分類 material_templates・view_quantities・editor_defaults を反応表と同じ読み込みに足し(T-0398 の共有の決め事)、
  実験室・筆・パネルの材料・実験室の箱を満たす空気と温度・抽出の物質・描画の色の幅をパッケージから読む形にした(ADR-0174)。
  release のビルド警告 0・CPU 22 本・window_lab/place/patch の 7 本・gpu_lab_box・gpu_probe_sim/place/peek・image_probe_volume/slice が通過。
  wt2 の欄の道具(073eca1。script/package_fields)を cherry-pick して、自分の欄の道具をそちらへ寄せた。
