# T-1214 工学 R-T2: 早送りの待ちの見積もり(D-661)に刻みを選ぶ段の費用を足し、起動時の測り(D-662)でも測る

- Status: Done(2026-10-27)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分の一部(T-1213 と同じブランチ)
- マイルストーン: 試作の列 6(docs/plan/ROADMAP.md の R-T2。T-1112 から分けた)
- 設計: docs/design/20-transport-substrate.md §14.2 / 決定: D-661・D-662・D-672・ADR-0472・ADR-0493

## 目的(1〜2 行)
早送りの待ちの見積もり `EstimateTimeTreeForward` は解きの費用だけを数えていた。出来事ごとに 1 回走る刻みを選ぶ段(GpuTimeTreeChoose)の
費用を式に 1 項足し、その値も起動時の測り `ProbeTimeTreeSolveCost` で、その PC で測って決める(口の形はそのまま)。

## 完了条件(チェックできる形で)
- [x] `TimeTreeSolveCost` に `chooseNs`(1 出来事の固定費)・`chooseUnitPs`(単位 1 つ)。`TimeTreeChooseNs(cost, units)` = 固定費 + 単位 × 傾き
- [x] `EstimateTimeTreeForward`: 出来事の数 × `TimeTreeChooseNs(木の単位の数)` を待ちに足し、`chooseMicroseconds` に分けても出す
- [x] `ProbeTimeTreeSolveCost`: 中身を持たない箱の木(16³ = 4,096 単位・1 区画 / 32³ = 32,768 単位・8 区画)で選ぶ段の 1 出来事を
      測り(回の最小)、`FitTimeTreeSolveCost` が 2 つの時間から固定費と傾きを解く(測っていなければ既定の値)
- [x] 起動時の測りの試験(ctest `gpu_time_step_event_cost`)・見積もりの試験(ctest `time_step_tree`)が通る

## 作り
- 既定の値(測らない時・CPU の試験): T-1213 の火事の出来事の測り(4,096 単位 21.5 µs・65,536 単位 42.1 µs → 固定費 20,100 ns・単位 334 ps)。
- 箱の木の記録: 半分(x < 辺 / 2)は望み 0(変化の速い所)・残りは上限 10。隣の差 1 段が境から広がり、区画の中を数回回す形(中身によらない)。
  打ち切りの連なる出来事(平均の 10 倍ほど)は入れていない: 火事では 265 出来事のうち 8 で、平均には入る(既定の値は平均から)。
  見積もりの上の値は今まで通り × marginPercent(200 %)。
- 測りの費用: 箱の木を 2 つ作って 8 回ずつ積む(GPU で 1 ms 未満。起動の約 0.4 秒〔D-672〕はほぼ変わらない)。`chooseSmallSide = 0` で測らない。

## 結果(2026-10-27・RTX 3070 Ti・release・compute キュー・`gpu_time_step_span_test --continuous`〔ctest gpu_time_step_event_cost と同じ〕)
| 測り | 箱の木 4,096 単位 | 箱の木 32,768 単位 | 解いた式 |
|---|---|---|---|
| 熱した後(初め・直後) | 24.6 µs | 36.9 µs | **固定費 22,822 ns + 単位 428 ps** |
| 熱さない | 24.6〜25.6 µs | 37.9〜39.9 µs | 25,747〜25,894 ns + 428〜464 ps |
| (前の版: 静かな所の望み 10) | 33.8 µs | 70.7 µs | 28,528 ns + 1,285 ps(火事の 1.6〜2.7 倍に出た) |
- 熱した式の当て: 4,096 単位 24.6 µs(火事の 0.5 m の木: 平均 23.9 µs・最小の平均 21.5 µs)・65,536 単位 50.9 µs(広い木: 平均 42.8〜50.8 µs・最小の平均 42 µs)。
- 火事 18 分(4,096 単位・53,246 出来事)の見積もりに足す選ぶ段: 既定の値で 53,246 × 21.5 µs ≈ 1.1 秒(測った値で 1.3 秒)。
- 測りの費用: 選ぶ段の 2 つの木で GPU 1 ms 未満(起動の全体 757〜897 ms のうち、熱すのが約 0.41 秒)。

## 判断待ち(仮で進めたもの)
- なし(D-661 の口の形はそのまま・式に 1 項。作り手から見て: 見積もりの待ちが選ぶ段の分だけ長く出る〔火事 18 分で約 1.1 秒〕。
  ゲームの中身へ: なし)。

## 土台に足りないもの(司令塔へ)
- なし。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-27(wt4・ブランチ t-1213-choose-fixed-cost): TimeTreeSolveCost に chooseNs・chooseUnitPs、TimeTreeChooseNs、見積もりの chooseMicroseconds、
  起動時の測りに箱の木 2 つの選ぶ段を足した。静かな所の望みを上限 10 にした初めの箱は火事の 1.6〜2.7 倍に出たので 3 にして合わせた(熱した後 22.8 µs + 428 ps/単位)。
  ctest time_step_tree・time_step_event_merge・gpu_time_step_event_cost・time_step_choose_dump・gpu_time_step_choose 通過。
