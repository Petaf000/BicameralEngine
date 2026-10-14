# T-0559 遊びの指標に範囲と場面の差分

- Status: Doing(範囲は済み。場面の差分は T-0580 に分けた)
- 種類: 工学
- PC: 必須
- 見積もり: 範囲 1 チャット(済み)/ 場面の差分 1 チャット(T-0580)
- マイルストーン: D-471 ⑤(T-0414 の作り心地の記録 4 から)
- 設計: docs/design/14-editor-tools.md §2 / 決定: D-482 ④・ADR-0206・ADR-0240・ADR-0251(Proposed)

## 目的(1〜2 行)
遊びの指標は世界全体の量しか測れず、「向こう岸の草が残ったか」を世界全体の量の暗算で読んでいた。指標に範囲を書けるようにし、
指標ごとに場面(初めの世界)の違いを書けるようにする(川と土の隙間・防火帯あり/なしを同じ場面のフォルダで比べる)。

## 完了条件(チェックできる形で)
- [x] 指標に範囲 `min_cell`・`size_cells`(world_cells と同じ書き方。両方書くか両方省く。省けば世界全体)。3 つの量すべてに効く
- [x] 格子に収まらない範囲はベイクで誤り。範囲の中だけの量を CPU の読み戻し(GPU の写しと CPU リファレンスで同じ関数)で測る
- [x] 場面 grass_fire_river の指標を岸ごとの範囲で書き直し、前の版の値を測り直す
- [ ] 指標ごとの場面の差分(world_cells の鍵を足す・書き直す)→ **T-0580** に分けた

## 作り手から見た違い(判断待ち。仮。ユーザー未確認)
- 範囲の書き方: **仮 = 指標の欄に直接 `min_cell`・`size_cells`**(world_cells の領域と同じ名前・同じ意味)。代案 = `region = { min_cell, size_cells }` の
  入れ子(欄がまとまるが world_cells と形が違う)。作り手から見た違いは書き方だけ(測れるものは同じ)。おすすめ: 仮のまま(覚える形が 1 つ)。
- 範囲は箱 1 つだけ。箱の和(L 字の岸など)・物質を初めに持っていたセルだけ、は今は書けない(要れば箱を並べた別の指標を足す)。

## T-0580 への引き継ぎ(場面の差分)
- 形の案: 指標に `world_cells = { <鍵> = { layer, min_cell, size_cells, amounts, temperature_mk } }`(場面の world_cells と同じ形。同じ鍵は書き直し・
  新しい鍵は足す)。読み手は world_layout_package の領域の読みを共通にして使う(ReadRegionPlace などは今は無名の名前空間)。
- 測り方: PlayMetricMeasurer は初めの世界 S(0) の元の世界(ProbeSim)を 1 つだけ持つ。差分がある指標は、差分を当てた組み立てから
  S(0) を作り直す(ProbeSimOptions::initialWorld に MakeLayoutInitialWorld の結果を渡すか、元の世界をもう 1 つ作る。VRAM は世界 1 つぶん増える)。
  CPU リファレンス(MeasurePlayMetricReference)も同じ組み立てから。gpu_play_metric で差分ありの指標の GPU と CPU のビット一致を足す。
- 場面: grass_fire_river に「川の代わりに土の隙間」「手前に防火帯(砂)」の指標を足す(T-0414 §3 の試しを場面のフォルダの写しなしで)。

## 作業ログ
- 2026-10-15(wt3): 範囲を足した(PlayMetricRegion。読み = play_metrics_package・ベイク = BakePlayMetrics・量 = MeasurePlayMetricQuantity の
  MeasureInRegion)。{ x, y, z } の読みを package_fields(ReadVector3・ReadVector3Field)へ移して world_cells と共通に。場面の指標を岸ごとに
  (grass_fire_reaches_river = 手前の岸の草・far_grass_left_after_10_min = 向こう岸の草)。ADR-0251(Proposed)。
  試験(release): play_metric(範囲の読み・ベイク・箱の中だけの量)・world_layout・standard_packages・luau_type_check が通過。
  run(RTX 3070 Ti): 場面の 3 指標を測り直した。川に着くまで 16 秒(量は手前の岸だけ 205,702,939 = 前の世界全体 − 向こう岸 1,536,000,000)・
  向こう岸の草は 10 分後も 1,536,000,000(全部残る)・消えるまで 26 秒。前の版のファイルを書き直した(--metrics-save は exe の横に書くので写した)。
