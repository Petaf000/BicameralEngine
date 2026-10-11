# T-0350 実験室の細かくした所の計器・断面・見るセルと、窓の自動の確認

- Status: Done(2026-10-11)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 1 体・2 時間の約束の中
- マイルストーン: M2 (docs/plan/ROADMAP.md。D-449)
- 設計: docs/design/14-editor-tools.md §2「実装(T-0350)」/ 決定: ADR-0137(追記 1)・ADR-0113・ADR-0075・D-449

## 目的(1〜2 行)
T-0314 で細かくできるようにした実験室の箱で、細かいセルを計器で数え・見るセルに選べ・断面に描けるようにし、
窓の自動の確認(`--auto-lab`)が細かくした箱でも通ること(T-0314 から分けたもの)。

## 完了条件(チェックできる形で)
- [x] `--auto-lab` の細かくした箱の確認が窓で止まる原因を突き止めて直し、RunAutoRefinedBox を呼ぶ(window_lab)
- [x] 計器(SampleLabGauge)が葉のセルを数える(8^-k µmol を µmol に直す。端数は切り捨て)。見るセルをセルの語(細かいセルも)にできる
- [x] パネル: 実験室の「細かさ (段)」「細かいセル」・計器の「見るセルの細かさ (段)」・断面に細かいセル(4×4)・セルの欄は葉
- [x] lab_box(CPU): 細かくしても量が同じ・細かいセルの量・粗い葉の中の細かいセル・LabLeafAt・LabFineSlice・CPU だけの計器
- [x] window_lab: 細かくした箱の GPU の計器 = CPU だけで流した値(全刻み)・窒素の合計・見る細かいセルの木の量・断面の火の所

## メモ・参考
- 実装: sim/lab_gauge(SampleLabGauge・LabLeafAt・LabFineSlice)・sim/lab_session(SetGaugeCell がセルの語)・editor/lab_panel
  (RunAuto・RunAutoRefinedBox・CheckRefinedGauge・BuildSlice・BuildSliceCell・BuildFineCellControls・SelectedPosition・BuildCell)・
  editor/gauge_panel(見るセルの細かさ)・main.cpp(ReportCrtFailuresToStderrWhenAutomatic)。
- **窓で止まった原因**: `LabPanel::RunAuto` が材料の一覧 `m_materials` の反復子(木)を持ったまま、RunAutoLargeBox の CreateSession が
  一覧を作り直した後に `wood->contents` を読んでいた。debug の STL の反復子の検査(「can't dereference invalidated vector iterator」)が
  ダイアログを出して止まり、押す人がいないので ctest・run の時間切れまで戻らなかった(標準出力も書き出されずに切れる)。
  根を並べた箱の確認(RunAutoLargeBox)も解放した一覧の成分を読んでいた(置けたかを見ていなかったので通っていた)。
  直し方: 成分を写してから使う・置けたかを見る。`--auto-*` の起動では debug の CRT の検査(assert・STL・abort)を標準エラーへ出して
  すぐ落ちる形にした(ダイアログで止まらない)。RecordProcessRequests・debug layer は関係なかった。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役・wt6): `--auto-lab` に段ごとの標準エラーへの印と CRT の検査の出し先を仕込んで流し、根を並べた箱の後で
  「can't dereference invalidated vector iterator」と分かった(材料の一覧の反復子を箱を作り直した後に読んでいた)。成分を写して直し、
  RunAutoRefinedBox を呼んだ。計器を葉のセルを数える形にし(ADR-0137 追記 1)、見るセル・置くセルを細かいセルで選べるように、
  断面に細かいセル(4×4)を重ねて描くようにした。lab_box(98 秒)・window_lab(370 秒)が通過。

## 引き継ぎメモ(HANDOFF に載せる内容)
- 動いているもの: 「実験室」で細かくできる箱を作って細かくすると、断面が 4×4 に分かれて葉の温度を描き(「細かい断面」で箱のセルの中の z)、
  押した所の葉を選ぶ。「細かさ (段)」と「細かいセル」で 1 セルの「置く」「温度だけ」を細かいセルに当てる。「計器と比べる」の
  「見るセルの細かさ (段)」で細かいセルを見る。計器の量は葉を µmol に直した和(端数は切り捨て)。
- 自動の確認: `--editor --auto-lab` が根 1 つ → 根を並べた箱 → 細かくした箱(計器・断面も)を確かめる。`--auto-*` の起動では debug の
  CRT の検査が標準エラーに出て落ちる(ダイアログで止まらない。ほかの窓の自動の確認にも効く)。
- 注意:
  - 計器の量は µmol の切り捨てなので、細かいセル 1 つの量は 64 分の 1 µmol 未満が落ちる(反応の速さのグラフが ±1 µmol ずれることがある)。
  - 見るセルの温度は重なる葉の温度の体積の平均(熱容量の重みなし)。
  - 断面は CPU の箱を毎フレーム葉を引いて描く(4×4×4 の根の箱で細かくした所が多いと重くなりうる。測っていない)。

## 判断待ち
- なし(実装の細部だけ。ADR-0137 追記 1)。T-0314 の判断待ち(細かい所の熱の陰解法 = T-0351 候補)はそのまま。

## 分けたもの(司令塔が ROADMAP に足すか決める)
- なし。
