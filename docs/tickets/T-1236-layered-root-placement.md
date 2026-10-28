# T-1236 層つきの根の置き口と、パッケージから層を置く道

- Status: Done(2026-10-29 wt3。ADR-0489 の 3 は仮 = (a) のまま。(a)(b) どちらでも置き口は同じ)
- 種類: 工学
- PC: 必須(CPU。GPU は写すだけ)
- 見積もり: チャット 1 回分
- マイルストーン: 土台の変更要求の束(ADR-0469 の 2-2 の続き)。T-1210 と同じ
- 設計: docs/design/20 §10.5 / 決定: ADR-0481 の 5・ADR-0489(Proposed)/ 前: T-1220

## 目的(1〜2 行)
今の根の置き口(`PlaceRootBlock`)は層 0 で置く。ADR-0489 の 3 が (a)「層を書かずに置いた粒は舞う粒」のままなら、
層を使う世界の初めの場面(積もった砂の床)を置くには、層つきで根を置く口と、環境のパッケージから層を置く道が要る。

## 完了条件(チェックできる形で)
- [x] 層つきの根の置き口(セルと成分ごとの層。一様なら一様の枠の層・頁なら頁の層)。層はセルに収まる(0 ≤ 層 ≤ 量)
- [x] 環境のパッケージ(Luau)から層を置く道(初めの場面の作り方。パッケージの書き間違い〔層 > 量〕がどう現れるか)
- [x] 試験: 層つきで置いた根が HashWholeNest・保存点に入る・層の無い置き方は今とビット単位で同じ
- [x] ADR-0489 の 3 が (b) に決まったら、このチケットは「全部が層」の印の置き方に読み替える(→ 値 1 つで切り替える形にした。下の「作ったもの」)

## メモ・参考
- 根の置き口: engine/src/sim/multires_nest.cpp の PlaceRootBlock。層を書く口は StoreNestCellWithLayers・nest_detail::SetUniformLayers。
- GPU は層の頁を写すだけ(T-1221。一様の枠の層を GPU に写すのは T-1240)。

## 作ったもの(2026-10-29)
- **置き口**: `PlaceRootBlock(nest, origin, cells, layers)`(multires_nest.h。layers は空か 512 個)。セルと層まで全部同じなら一様の枠の層
  (`SetUniformLayers`)、でなければ頁の層(溢れの世界は上限の無い形で)。層が全部 0 か空なら前の置き方そのまま(ビット単位で同じ。層の並びを持たない)。
- **パッケージの道**: `world_cells` の領域に `settled_umol = { 物質 = µmol }`(中身のうち積もった層。名前は仮)。読み(world_layout_package.cpp)は形だけ、
  ベイク(world_layout.cpp)が「中身に無い物質・層 > 量・同じ物質 2 回」を場所付きの誤りで止める(= 書き間違いはベイクで止まる。黙って押さえない)。
  ベイクした領域は成分の並びの位置ごとの層(`WorldCellRegion::layers`)を持ち、`LayoutCellAt`・`PlaceLayoutRootBlock`(根のセル 1 つ = 格子のセル 1 つ)で根を層つきで置く。
  仮の世界(MakeProbeInitialWorld)は層を持たないので何も変わらない。
- **(a)(b) の切り替え**: `UnwrittenGrainLayers`(world_layout.h)の値 1 つ `UNWRITTEN_GRAIN_LAYERS`(仮 = Floating)。
  Floating = 書かなかった粒は層 0・ベイクが領域と物質ごとに `WorldLayout::unsettledGrainNotes` に知らせを作る(0 と書けば「舞わせる」と書いたことになり知らせない)。
  Settled = 書かなかった粒は層 = 量・知らせなし。(b) に決まったら値を変えるだけ(BakeWorldLayout の引数でも選べる。試験は両方を通す)。
  ただし (b) の「層の並びが無い ≠ 層 0」の印(刻みの中の付け替え・PlaceRootBlock の層なしの置き方)は ADR-0489 の 3 の別の仕事で、ここでは入れていない。

## 司令塔へ(分けたもの・ゲームの中身へ)
- 知らせ(unsettledGrainNotes)を表の警告に足すのは「層を使う世界(EnableCellLayers)を作る所」。今は作り手が層を使う世界を選ぶ口がパッケージに無い
  (C++ の EnableCellLayers だけ)ので、警告に足す所はまだ無い。層を使う世界をパッケージで選ぶ口(環境の欄など)を作る時に、そこで足す(新しいチケットに: 番号は司令塔)。
- 多重解像度の本物の世界は、まだ world_cells から作られていない(frame_loop は仮の世界)。PlaceLayoutRootBlock はその時の入口。
- wt2(T-1188)との重なり: data/types/bicameral.d.luau の WorldCellRegion に 1 行(wt2 は Species の packing_percent。別の所)。reaction_package.cpp は触っていない。
  multires_tree.cpp は PlaceRootBlock の所だけ(wt2 の差分は粗くする所)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29(wt3・T-1221 の作業役): T-1220 の「分けたもの 1」からファイルを作った。
- 2026-10-29(wt3・T-1236 の作業役): 層つきの根の置き口・world_cells の settled_umol・書かなかった粒の読み方を値で切り替える口(仮 = 舞う粒・知らせ)・
  PlaceLayoutRootBlock。試験は multires_layers(層 0 = 層なしとビット一致・一様 / 頁の層・要約・保存点。インライン / 溢れ)と world_layout(読み・誤り・(a)(b)・根を置く)。
