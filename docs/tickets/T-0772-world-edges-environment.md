# T-0772 世界の端の外を基準の大気から作る・環境の world_edges と世界の箱を MultiresNest::edges へ・外が変わったら端のブロックを種に

- Status: Done(CPU。MR の世界を作る所と表の差し替えから呼ぶ口は T-0774)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(2 時間)
- マイルストーン: 運ぶ計算の共通の土台 段 3 ③(T-0721 から分けた)
- 設計: docs/design/20-transport-substrate.md §1・§2.6 / 決定: ADR-0338(Proposed)・ADR-0337・ADR-0182 / D-494・T-0710・T-0619

## 目的
作り手が環境に `world_edges = { top = "open" }` と書くと、パッケージから MR の世界の端がそのまま効く(T-0721 は試験で直接置いていた)。

## 完了条件
- [x] engine/src/sim/multires_world_edges.{h,cpp}: MultiresWorldBox(根のレベルの世界の箱)・MakeAtmosphereColumn(基準の大気の柱)・
      MakeMultiresWorldEdges(環境の world_edges + 基準の大気 + 箱 → MultiresWorldEdges)・SetWorldEdges(替える。違えば開いた端に接するブロックを種に)
- [x] 外の柱は仮の世界の気体の区画の層の基準と同じ道(SampleAtmosphere → 一番下の層の静水圧 AtmospherePressureMicroPascal → MakeHydrostaticReference)。
      箱の中の層だけを持ち、上下の外は端の層を読む(= PS の開いた境界の外 ghost[z] と同じ)。風は入れない(D-514)
- [x] 縦を y にそろえた(world_edge.hlsli の WE_UP_AXIS = 1・WeLevelZeroY・WeOutsideY・MultiresWorldEdges::columnBaseY。ADR-0338 の 1)
- [x] 基準の大気が無いのに open: ベイクの「注意」(reaction_table.cpp の WarnWorldSetting。セルを置かない世界でも出す)。外は真空のまま
- [x] 試験 multires_edge_test(y が上に直した上で): パッケージ edges_open_top(earth_environment の上で top = open)から作った柱が
      ProbeGasField::ReferenceCell とビット一致・温めた世界の熱が上から逃げ保存量一定・帳簿は世界の端・出だけ /
      3³ の根で上だけ open → 上の段の 9 ブロックだけ種・同じなら 0・外の温度を変える / wall に戻すと 9 / edges_vacuum で注意と真空
- [x] release のビルド警告 0・multires_edge・environment_package・world_atmosphere・world_layout・^multires_・archmap --check

## 縦の向き(判断。仮〔ユーザー未確認〕。ADR-0338 の 1)
T-0721 は世界の端の縦を z にした(MR の気体の木 gas_tree・気体の箱 GasConfig の「z が上」に合わせた)。一方、仮の世界の格子・world_cells・物理・描画は
T-0619 で y が上にそろい、main の MR の液体(T-0717)も y が上。wt4 の T-0720(MR の気体)は z のまま(縦の軸をそろえるのは T-0764)。
- 案 A(おすすめ・採った): **世界の上は +y**。世界の端・外の柱・MR の液体・場面の書き方・物理・描画が同じ。気体の箱の「z が上」は気体の解き方の中の軸
  (仮の世界の気体の区画が GRID_AXIS_OF_GAS_AXIS で写すのと同じ形)で、MR の気体は T-0764 で y にそろえる。
  作り手から見た違い: world_cells・world_bodies に書いた高さ(y)がそのまま世界の端の上下と基準の大気の高さになる。PS(1 段の MR)と MR の座標が
  入れ替えなしで同じなので、答え合わせ(D-513)がそのまま比べられる。
- 案 B: 世界の上を +z(T-0721 のまま)。気体の箱と同じ軸になるが、場面の書き方・物理・描画・MR の液体・PS の格子を全部 z に直す(T-0619 の逆)。採らない。

## 高さの基準
- レベル 0 のセル(0.5 m)の y = Y の高さは海面から Y × 0.5 m(セルの下の面)。仮の世界の気体の区画の床の高さ 0(ProbeGasOptions::floorHeightMillimeters の既定)と同じ。
  世界の床を海面からずらす値(作り手が書く)は今は無い(要るなら環境の値として足す。T-0774 で世界を作る所と一緒に決める)。

## 判断待ち(仮〔ユーザー未確認〕。取り消しやすい)
1. 縦は y(上)。上の通り。
2. 上下の外は端の層の基準(柱の外の高さは端の行)。作り手から見た違い: 上を開くと、上の層は「同じ高さの基準の大気」と熱・気体をやり取りする
   (1 層上の高さの大気ではない)。PS の気体の開いた境界と同じで、気体の式(基準からのずれ p̃)では外が p̃ = 0 になり静かな大気が動かない。
3. 基準の大気が無い open は真空のまま + ベイクの注意(T-0721 の判断待ち 1 のおすすめ)。

## 司令塔へ(並走との重なり)
- wt4 の T-0720(MR の気体)は「縦は z・世界の端 world_edge.hlsli と同じ」と書いている。このチケットで world_edge.hlsli の縦を y にしたので、
  T-0764(気体・液体の世界の端の口と縦の軸)で MR の気体を y にそろえる。名前の変更: WeLevelZeroZ → WeLevelZeroY・WeOutsideZ → WeOutsideY・
  MultiresWorldEdges::columnBaseZ → columnBaseY(使う側はマージで直す)。
- bicameral_multires が bicameral_gas を読むようにした(外の柱を気体の層の基準と同じ関数で作るため。循環は無い)。

## 分けたもの
- **T-0774**(工学・CPU): MR の世界を作る所(パッケージの世界から世界の箱を決める)と、表の差し替え(ホットリロード)で
  MakeMultiresWorldEdges → SetWorldEdges を呼ぶ口(今は MR の世界をアプリが作らないので呼ぶ所が無い。縦切りの段で)。世界の床の高さの値もここで決める。

## 作業ログ
- 2026-10-17(wt3): multires_world_edges を足し、world_edge.hlsli の縦を y に、WarnWorldSetting に真空の注意。試験のパッケージ edges_open_top・edges_vacuum。
  device_bash が何度も切れて時間を使った(書き込みはクラウド側で作って base64 で送った)。
