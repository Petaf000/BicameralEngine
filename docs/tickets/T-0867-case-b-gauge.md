# T-0867 B1: 案 B の試作の場面と計器(CPU)

- Status: Done(2026-10-21)
- 種類: 工学
- PC: 必須(ビルド・ctest)
- 見積もり: チャット 1 回分(T-0866 と同じ作業役)
- マイルストーン: ③'(案 B の判定。docs/design/21 §5.3)
- 設計: docs/design/21-motion-case-b.md §5.2・§5.4 / 決定: ADR-0376

## 目的(1〜2 行)
R1〜R6・M1 の合格(21 §5.2)を測る場面と計器を作る。同じ場面から 0.5 m と 2 m(と 0.25 m)の箱を作って比べられるようにする(B-c)。

## 完了条件(チェックできる形で)
- [x] 5 場面(砂山・砂の堤・ダムの決壊・サイフォン・閉じた空気の下の水)+ 弱い差の戻り(B-c 用)が 0.25 m・0.5 m・2 m で読め、凝縮相の量が厚さあたりで同じ(気体 ±0.1 %)
- [x] 計器: 動く面の数・最大の速さ・費用(ns/セル/刻み)・保存・ハッシュ・範囲ごとの相の質量・水位・粒の高さ・崩れる角度・水面のセルの圧力の幅・範囲の平均の圧力・流れの向きの口(#91。T-0866 の `CellPhaseVelocity`)
- [x] 列: 揺り戻しの回数(ヒステリシス)・戻る時間・届いた時・最大とその刻み・2 つの値の差(‰)
- [x] 分かっている入力で計器が正しい(静かなダムの水位 2 m・水面のセルの数・34° の斜面 → 34・減衰する揺れの回数・過減衰の戻る時間)
- [x] release のビルド警告 0・`case_b_gauge` が通る

## 作ったもの
- `engine/src/sim/case_b/case_b_scene.h/.cpp`: 場面 = 絵(0.5 m の文字の行。上が上)+ 凡例(凝縮相の体積 %・空きの気体の圧力と組成・物の占める %)+ 物質 + 見る範囲。
  `BuildScene(scene, level, layout)` が細かさを選んで箱を作る(粗いレベルは絵の区画の平均、細かいレベルは 1 文字を分ける。z は 1 セル)。
  場面: `MakeSandPileScene`・`MakeSandLeveeScene`・`MakeDamBreakScene`(T-0610 と同じ形)・`MakeSiphonScene(crestCells)`(頂上の高さを引数に。28 で 10 m)・
  `MakeTrappedAirScene`・`MakePressureRelaxScene`(左半分 +2 kPa の閉じた空気)。
- `engine/src/sim/case_b/case_b_gauge.h/.cpp`: `RunScene`(刻みを進めて毎刻み `GaugeSample`)・`Recording`(費用・保存・ハッシュの列)・列の関数・`SlopeAngleDegrees`(tan の表で整数)・`FormatFixed`。
- `tests/case_b_box_test.cpp --gauge`。

## 仮・判断待ち
- 仮: 場面の z は 1 セルの厚さ(2 m の箱は 0.5 m の箱の 4 倍の厚さ)。厚さあたりの量で比べる。3 次元の場面が要れば絵を層にする(R3 の 3 cm の山・R6a の間欠泉)。
- 仮: 砂の堤の場面は「染みる・崩れる」まで(浸食・決壊の溝は 2 周目 R5。21 §2.6)。
- 仮: 閉じた空気の下の水の「釣り合いの計算」は R1 の作業役が場面の形(口は水面の 2 m 下・中の空気 3 m)から出す(外の水位の下がりを入れて約 0.4 m 上がる見込み・未確認)。
- 仮: 計器の水面は y が上の場面だけ(重さの軸を変えた場面は計器を足す)。
- 未確認: サイフォンが切れる高さは本物の水で約 10.3 m(大気 ÷ ρg)。28 セルの頂上は水面の 10 m 上で、ぎりぎり流れる側(R1 は 26・30 などを並べて切れる所を見る)。

## R1 以降の作業役への申し送り
- 場面: `case_b_scene.cpp` に `Make*Scene` を足し、`AllScenes` に入れる(凡例は `StandardLegend` に足すか場面ごとに `scene.legend.push_back`)。物質は `StandardMaterials` か場面ごとに。
- 測り: `RunScene(box, stages, solver, ticks, regions, afterTick)` → `RegionSeries(recording, i, &RegionSample::liquidLevelUm)` などの列 → `FirstTickAtLeast`(ダムが壁に届く)・
  `PeakOf`(壁際の最高)・`CountReversals`(揺り戻し)・`SettleTick`(戻る時間)・`DifferencePermille`(0.5 m と 2 m の比べ)。合格の値は 21 §5.2(仮の目安は直してよい)。
- 試験は `tests/case_b_box_test.cpp` に足すか、R ごとに `tests/case_b_r1_test.cpp` のように分けて `tests/CMakeLists.txt` の case_b の行の後に登録する(`bicameral_case_b` にリンク)。
- 0.5 m と 2 m の比べは `BuildScene(scene, 0)` と `BuildScene(scene, -2)`。動かした後の形を粗くするのは `CoarsenBox`(今は動き出す前の箱だけ)。

## 作業ログ
- 2026-10-21: T-0866 と同じ作業役・同じブランチ(t-0866-b0b1)。6 場面と計器・列の関数。release 警告 0・`case_b_gauge` が通る。
