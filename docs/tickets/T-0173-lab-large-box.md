# T-0173 実験室の箱を大きく・細かく(根を並べる。一部を細かくするのは T-0314)

- Status: Review(作業役・wt6。司令塔のマージ待ち)。一部完了: 根を並べた大きい箱まで。一部を細かくする箱は T-0314 に分けた
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 1 体・2 時間の約束の中
- マイルストーン: M2 (docs/plan/ROADMAP.md。D-449)
- 設計: docs/design/14-editor-tools.md §2「実装(T-0173)」/ 決定: ADR-0113(新)・ADR-0037・ADR-0055・ADR-0075・D-449・D-439

## 目的(1〜2 行)
中身(反応表・魔素の値)を触って決めるために、実験室の箱を大きくする(根を並べる)。置く・温度・表の差し替え・計器・比べる・記録と再生が
これまでどおり働き、GPU と CPU が刻みごとにビット一致する。一部を細かくする箱(木の要求)は 2 時間に収まらないので T-0314。

## 完了条件(チェックできる形で)
- [x] 箱の形 `lab::LabBoxShape`(根を x・y・z に 1〜4 本。最大 4×4×4)。MakeLabBoxNest・LabBoxCapacity・GpuLabBox::Create・LabSession::Create が形を受ける
- [x] セルの番号 `lab::LabCellIndex`(根 1 つなら MrCellIndex と同じ)。1 セル・範囲のコマンドは形の中だけ当てる(GPU の lab_box.hlsl と CPU の ApplyLabCommands が同じ)。
      当てたセルを含む根だけつつく(表を替えた印は全部の根)
- [x] 記録 BLAB 版 5(箱の形)。版 1〜4 は根 1 つで読む。形の違う記録の Replay は断る
- [x] 計器(SampleLabGauge)・最初に違ったセル(FindLabMismatch)・CPU だけの計器(RunLabGaugeOnCpu)・比べる段取り(CutLabRecording が形を写す)が箱の全部の根を数える
- [x] パネル「実験室」で箱の大きさを選ぶ・作り直す。断面・スライダー・計器の見るセルが箱の形に合わせる。読んだ記録の形が違えばその形の箱を作って再生
- [x] lab_box(CPU): 根の枠の順と原点・根をまたぐ範囲 = 1 セルずつ・形の外・根の境を熱が通る・版 5 と版 4・計器
- [x] gpu_lab_box(HW・WARP): 4×1×2 の根の箱で根の境に置いた木を燃やし、表を替えても毎刻みビット一致・隣の根の木のエネルギーが増える・同じ形の別の箱で再生・違う形は断る
- [x] window_lab: `--auto-lab` が根 1 つの確認の後に 2×2×2 の根の箱で同じことを確かめる(人がいなくても確かめられる)
- [ ] 一部を細かくした箱(木の要求)→ **T-0314**

## メモ・参考
- 実装: common/lab_box.hlsli(LabBoxShape・LabCellIndex・LabCellSlot・LabRootBit・LabAllRoots・LabPackShape。LabCommandCell と LabRegionCommandValid が形を受ける)・
  sim/lab_box.hlsl(ApplyToCell・g_external1 = 箱の形)・sim/lab_box(LAB_SINGLE_ROOT_SHAPE・SameLabShape・LabBoxCellCount・LabCellPositionOf・BLAB 版 5)・
  sim/gpu_lab_box・sim/lab_session(Shape())・sim/lab_gauge・editor/lab_panel(BOX_SHAPES・BuildShapeChoice・ClampCells・RunAutoLargeBox)・editor/gauge_panel。
- 形を受ける関数は既定が根 1 つ(LAB_SINGLE_ROOT_SHAPE)。並走の wt3(T-0024 のパッチのコマンド)が載せ直す時に、今までの呼び方はそのまま通る。
- 1 刻み = 1/60 s なので、伝導で隣のセルが温まるのは数十刻みで数十 J(mK の桁)。確かめはエネルギーが増えたかで見る(温度の 1 K では見えない)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役・wt6): 2 時間に収めるため、初めに「根を並べた大きい箱」= T-0173、「一部を細かくする箱」= T-0314 に分けた。
  根は x → y → z の順に枠 0 から置き、セルの番号の上のビットに根の座標を入れて、コマンドの種類と古い記録の意味を変えずに広げた(ADR-0113)。
  debug のビルドは警告なし。lab_box・gpu_lab_box(109 秒)・gpu_lab_box_warp(37 秒)・window_lab(105 秒)・window_lab_compare(89 秒)・float_check 5 本が通過
  (lab_box は 1 回目に古い版の記録を手で作る試験が版 5 の形の分だけずれて落ちた → 形を抜いてから作るように直した)。

## 引き継ぎメモ(HANDOFF に載せる内容)
- 動いているもの: `bicameral --editor` のパネル「実験室」の「箱の大きさ」(1×1×1〜4×4×4 の根)→「箱を作る」/「この大きさで作り直す」。
  `--auto-lab` は根 1 つの確認の後に 2×2×2 の根の箱も確かめる(ctest window_lab)。
- 注意:
  - 箱の形は作る時に決まる(GPU の箱の容量)。形の違う記録は LabSession::Replay が断る(パネルは記録の形の箱を作り直して再生)。
  - 実験室のセルの番号は `lab::LabCellIndex`(根 1 つなら MrCellIndex)。箱のセルを読むのは `LabBoxCell(nest, index, shape)`(形を渡し忘れると根 1 つとして読む)。
  - BLAB は版 5(種の後ろに形)。古い版の記録を手で作る試験は形の 16 バイトを抜いてから(lab_box_test の WithoutShape)。
  - 大きい箱ほど刻みが重い(CPU リファレンスで全部を刻み、毎刻み読み戻して比べる)。4×4×4 はまだ測っていない。

## 判断待ち
- 無し(ゲームの中身に関わる判断は無かった)。Claude が決めた実装の細部(ADR-0113): 根を並べる(1 軸 4 本まで。範囲の語の 5 ビットに合わせた)・
  形は作る時に決める・セルの番号の上に根の座標・つつくのは当てた根だけ・記録の版 5。
- 仮(ユーザー未確認・取り消しやすい): パネルの大きさの選択肢は 7 つ(1×1×1・2×1×1・4×1×1・2×2×1・2×2×2・4×4×1・4×4×4)。
  プレイヤーと遊びへの影響: 無し(作る人の道具)。要る形があれば lab_panel.cpp の BOX_SHAPES に足すだけ。

## 分けたもの(司令塔が ROADMAP に足すか決める)
- **T-0314 実験室の箱の一部を細かくする(木の要求)**: 根を並べた箱(T-0173)に、パネルで選んだ範囲を木の「細かくする」要求(17 §5)で 1〜数段細かくし、
  置く・温度・表の差し替え・計器・比べる・記録と再生が細かい所でも働き、GPU と CPU が刻みごとにビット一致すること。決めることがある:
  (1) 細かくする要求を記録に残す形(コマンドにするか。刻みの前に ProcessRequests / GPU の要求の処理を通す)、(2) 置く操作が細かくしたセルに当たる時
  (粗いセルの座標で置くと子の 8 セルに分けるか・細かいセルの座標を持つか。量の単位はレベルで変わる)、(3) 静かになった所を粗くする・畳むを箱で呼ぶか
  (ADR-0037 は畳まない)、(4) 細かいレベルの熱の小刻み・陰解法(17 §4)を箱で使うか。計器・FindLabMismatch・断面は根の頁だけでなく子のブロックも読む。
- (候補・小)大きい箱の 1 刻みの重さを測る(4×4×4 で debug と release。CPU リファレンスと読み戻し)。重ければ ADR-0075 の
  「VRAM の保存点か裏のキューで流す」を決め直す。
