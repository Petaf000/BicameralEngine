# T-0772 世界の端の外を基準の大気から作る・環境の world_edges と世界の箱を MultiresNest::edges へ・外が変わったら端のブロックを種に(+ T-0764 の気体の半分)

- Status: Done(CPU。液体を上の軸に従わせるのは T-0774。MR の世界を作る所と表の差し替えから呼ぶ口は縦切りの段)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(2 時間)
- マイルストーン: 運ぶ計算の共通の土台 段 3 ③(T-0721 から分けた。T-0720 の T-0764 の気体の半分を司令塔の指示で一緒に)
- 設計: docs/design/20-transport-substrate.md §1・§2.6 / 決定: ADR-0338(Proposed)・ADR-0337・ADR-0182 / D-494・T-0710・T-0619

## 目的
作り手が環境に `world_edges = { top = "open" }` と書くと、パッケージから MR の世界の端がそのまま効く(T-0721 は試験で直接置いていた)。
世界の上の軸は 1 か所の定数だけで決まり、後でどちらにも 1 行で切り替えられる(司令塔の指示。ユーザーに確認中)。

## 完了条件
- [x] engine/src/sim/multires_world_edges.{h,cpp}(bicameral_sim): MultiresWorldBox(根のレベルの世界の箱)・MakeAtmosphereColumn(基準の大気の柱)・
      MakeMultiresWorldEdges(環境の world_edges + 基準の大気 + 箱 → MultiresWorldEdges)・SetWorldEdges(替える。違えば開いた端に接するブロックを種に)
- [x] 外の柱は仮の世界と MR の気体と同じ GasAtmosphereLayers(gas_cells。SampleAtmosphere → 一番下の層の静水圧 → MakeHydrostaticReference)。
      箱の中の層だけを持ち、上下の外は端の層を読む(= PS の開いた境界の外 ghost[層] と同じ)。風は入れない(D-514)
- [x] 世界の上の軸は world_edge.hlsli の **WE_UP_AXIS の 1 か所**(仮は 2 = z)。端・外の柱・試験はこれを読む(WeAxisUpLast・WeCellAxis・WeOriginAxis・
      WeLevelZeroUp・WeOutsideUp・MultiresWorldEdges::columnBaseUp)
- [x] T-0764 の気体の半分: MR の気体(multires_gas)は端の種類を MultiresNest::edges.world から読む(MultiresGasOptions の top / bottom / sides を消した)。
      気体の箱は z が上なので、世界の軸を WeAxisUpLast で(横 1・横 2・縦)の順に写す(上が z なら写しは恒等 = 前とビット一致)。試験 multires_gas も同じ写しで
      仮の世界と比べる(上の軸を替えても試験はそのまま)
- [x] 基準の大気が無いのに open: ベイクの「注意」(reaction_table.cpp の WarnWorldSetting。セルを置かない世界でも出す)。外は真空のまま
- [x] 試験 multires_edge_test: パッケージ edges_open_top(earth_environment の上で top = open)から作った柱が ProbeGasField::ReferenceCell とビット一致・
      温めた世界の熱が上から逃げ保存量一定・帳簿は世界の端・出だけ / 3³ の根で上だけ open → 上の段の 9 ブロックだけ種・同じなら 0・
      外の温度を変える / wall に戻すと 9 / edges_vacuum で注意と真空
- [x] release のビルド警告 0・multires_edge・multires_gas・environment_package・world_atmosphere・world_layout・^multires_・archmap --check

## 世界の上の軸(判断。**2026-10-18 ユーザーが y に決めた = D-518**。WE_UP_AXIS = 1 にした。下は決める前の記録)
今は MR の液体(T-0717)が y、MR の気体(T-0720)・世界の端(T-0721)が z で食い違っている。このチケットで「上はどこか」を WE_UP_AXIS の 1 か所にし、
端・外の柱・気体はそれを読む形にした(液体は T-0774)。作り手から見た違い:
- 案 z(仮): MR の世界の座標の z が高さ。気体の箱(GasConfig は z が上)と同じ並び。場面の書き方(world_cells)・物理・描画・PS の格子は y が上なので、
  PS と MR を比べる・場面を MR に載せる所で y と z を入れ替える(試験 multires_gas の NestPlace の形)。
- 案 y: MR の世界の座標の y が高さ。world_cells・物理・描画・PS の格子・MR の液体と同じで、答え合わせ(D-513)と場面の載せ替えに入れ替えが要らない。
  気体の箱へは WeAxisUpLast が x・z・y の順に写す(PS の GRID_AXIS_OF_GAS_AXIS と同じ)。
- 作業役のおすすめは y(入れ替えの場所が気体の箱の中の 1 か所だけになる)。切り替えは WE_UP_AXIS を 1 にする 1 行 + 液体を従わせる T-0774。

## 高さの基準
- レベル 0 のセル(0.5 m)の縦の座標 U の高さは海面から U × 0.5 m(セルの下の面)。仮の世界の気体の区画の床の高さ 0(floorHeightMillimeters の既定)と同じ。
  世界の床を海面からずらす値(作り手が書く)は今は無い(要るなら環境の値として足す。世界を作る所と一緒に決める)。
- MR の気体の floorHeightMillimeters は世界の箱の一番下の層の高さのまま(外の柱は箱の座標から高さを出す。両方を同じにする約束は世界を作る所で)。

## 判断待ち(仮〔ユーザー未確認〕。取り消しやすい)
1. 世界の上の軸(上の節)。
2. 上下の外は端の層の基準(柱の外の高さは端の行)。作り手から見た違い: 上を開くと、上の層は「同じ高さの基準の大気」と熱・気体をやり取りする
   (1 層上の高さの大気ではない)。PS の気体の開いた境界と同じで、気体の式(基準からのずれ p̃)では外が p̃ = 0 になり静かな大気が動かない。
3. 基準の大気が無い open は真空のまま + ベイクの注意(T-0721 の判断待ち 1 のおすすめ)。

## 司令塔へ
- 名前の変更: WeLevelZeroZ → WeLevelZeroUp・WeOutsideZ → WeOutsideUp・MultiresWorldEdges::columnBaseZ → columnBaseUp・
  MultiresGasOptions の top / bottom / sides と MULTIRES_GAS_EDGE_* は消した(気体の端は nest.edges.world に置く)。
- MR の気体の箱は「根のブロックの外接の箱」のまま。世界の箱 WeWorld と違う箱はまだ見ない(同じに置く約束。世界を作る所で 1 つにする)。
- MR の世界を作る所(パッケージの世界から箱を決める)と、表の差し替え(ホットリロード)で MakeMultiresWorldEdges → SetWorldEdges を呼ぶ口は、
  アプリが MR の世界を作るようになる縦切りの段で(今は呼ぶ所が無い)。

## 分けたもの
- **T-0774**(工学・CPU): T-0764 の液体の半分。MR の液体(multires_liquid)を世界の上の軸 WE_UP_AXIS に従わせる(液体の芯は y が上なので、
  気体と同じく WeAxisUpLast 型の写しで)+ 液体の端を nest.edges.world から読む。試験 multires_liquid は NestPlace 型の写しで仮の世界と比べる。

## 作業ログ
- 2026-10-17(wt3): multires_world_edges を足し、WarnWorldSetting に真空の注意。試験のパッケージ edges_open_top・edges_vacuum。
  最初は縦を y にしたが、司令塔の指示(上の軸はユーザーに確認中・仮は z・1 か所で切り替え)で WE_UP_AXIS の 1 か所に寄せ、T-0764 の気体の半分
  (端を WeWorld から・軸の写し)も入れた。外の柱は t-0720 が入れた GasAtmosphereLayers を使う形に替え、ファイルを bicameral_sim へ移した。
  device_bash が何度も切れて時間を使った(書き込みはクラウド側で作って base64 で送った)。
  試験: release のビルド警告 0・^multires_ 12 本(multires_gas 492 秒・multires_liquid を含む)・multires_edge・environment_package
  (open で大気が無い時に注意が出るように期待を直した)・world_atmosphere・world_layout・archmap --check。
  気づいたこと: 基準の大気の静かなセルはエネルギーが負(化学のぶん)なので、試験で温めるのはエネルギーの大きさを足す形にした。
- 2026-10-18(本体の作業役): main へマージ。ユーザーの決定 D-518(上は y)で WE_UP_AXIS = 1 にし、^multires_・environment_package・world_atmosphere・world_layout が通った。ADR-0338 の ① を確定、②③ は Q123。
