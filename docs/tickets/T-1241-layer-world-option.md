# T-1241 層を使う世界をパッケージで選ぶ口と、層を書かなかった粒の警告

- Status: Done(2026-10-29 wt3。ADR-0489 の 3 は仮 = (a) のまま。(a)(b) どちらでも作り直さない形)
- 種類: 工学
- PC: 必須(CPU だけ)
- 見積もり: チャット 1 回分
- マイルストーン: 土台の変更要求の束(ADR-0469 の 2-2 の続き)。T-1236 の「司令塔へ」から
- 設計: ADR-0489 の 3(Proposed)/ 前: T-1236

## 目的(1〜2 行)
層を使う世界(EnableCellLayers)は C++ でしか選べなかった。作り手がパッケージ(Luau)で選べるようにし、T-1236 のベイクの知らせ
`WorldLayout::unsettledGrainNotes`(層を書かなかった粒)を、選んだ世界だけ作り手に見える警告(表の警告)に出す。

## 完了条件(チェックできる形で)
- [x] 環境のパッケージで層を使う世界を選ぶ口(書かない = 使わない。ブランクの世界は何も読まずに今と同じ)
- [x] 書き間違い(true / false 以外)は読み込みが場所付きで止める
- [x] 層を使う世界だけ、層を書かなかった粒の知らせが表の警告(BakedReactionTable::warnings。開発ビルドの計器・エディタ・CLI の読み込みの警告と同じ道)に出る
- [x] 多重解像度の木に選びを当てる口(使う / 使わない)
- [x] (a)(b) のどちらに決まっても選ぶ口と警告を作り直さない(値 UNWRITTEN_GRAIN_LAYERS はそのまま)

## 作ったもの(2026-10-29)
- **選ぶ口**: `environment = { cell_layers = true }`(名前は仮。data/types/bicameral.d.luau の Environment・environment_package.cpp の ReadCellLayers)
  → `WorldPhysics::cellLayers`(既定 false。== に入るので表の値として比べられる)。true / false 以外は「パッケージ X: environment.cell_layers: true(層を使う世界)か false(使わない)で書く」。
- **警告**: 表のベイク(reaction_table.cpp。組み立てのベイクの後、読まない分類の注意の隣)が、`cellLayers` の世界だけ `unsettledGrainNotes` を `warnings` に足す。
  知らせ自体は組み立てに今まで通り残る(使わない世界でも作るが出さない)。(b)(Settled)なら知らせが作られないので、ここは変えずに警告が出なくなる。
- **木に当てる**: `ApplyCellLayersChoice(nest, table)`(multires_nest.h・multires_refine_cells.cpp)。使う世界は EnableCellLayers、使わない世界は層を使わない形(cellLayers = {})に戻す。
  世界を作る時と表を差し替えた時に呼ぶ(今はまだ本物の世界を world_cells から作る所が無いので、呼ぶのは試験だけ。T-1236 の PlaceLayoutRootBlock と同じ時に入口になる)。
- 試験: environment_package(書かない・true・false = 書かないのと同じ・誤り 2 つ)・world_layout(警告は使う世界だけ・知らせと 1 対 1・木に当てて戻す・誤り)。

## 司令塔へ(分けたもの・ゲームの中身へ)
- ホットリロードで cell_layers を切り替えた時: 差し替えの検査(CheckHotReloadCompatible)は今は止めない。木に当て直すのは、本物の世界を表から作る所(上の入口)で ApplyCellLayersChoice を差し替えの後にも呼ぶこと。
  途中で使う世界にすると、層を書いていない粒はその時から舞う粒になる(状態は変わらない。読み方が変わる)。止めるかどうかは本物の世界を作るチケットで決める。
- 名前 `cell_layers` は仮(`settled_umol` と同じく名前を決める時にまとめて)。
- wt2(T-1188)との重なり: data/types/bicameral.d.luau は Environment の 1 か所だけ(Species の行は触っていない)。reaction_package.cpp・multires_tree・gpu_multires は触っていない。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29(wt3・T-1241 の作業役): T-1236 の「司令塔へ」からファイルを作った。environment.cell_layers → WorldPhysics::cellLayers・
  使う世界だけ unsettledGrainNotes を表の警告に・ApplyCellLayersChoice。試験は environment_package・world_layout。ADR-0489 に 1 項追記。
