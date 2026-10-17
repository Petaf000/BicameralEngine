# T-0716 液体の芯を共有 .hlsli へ(土台の段 2 ①)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 運ぶ計算の共通の土台 段 2 ①(runner/design/substrate-decided.md §2.3・§2.7・§4・§6)
- 設計: docs/design/07-transport.md §3 / 決定: 土台(2026-10-17 ユーザー決定。ADR は段 0)・ADR-0266・ADR-0276・ADR-0268

## 目的(1〜2 行)
液体の流れの固有の部分(柱の静水圧・面の力・摩擦・移流・速さの上限・位置/運動のエネルギー)を shaders/common/liquid_flow.hlsli の 1 つの源にし、
CPU(probe_liquid.cpp)と GPU(probe_liquid_tick.hlsl)が同じ関数を呼ぶ(heat_conduction.hlsli と同じ形)。値は今とビット単位で同じ。

## 完了条件(チェックできる形で)
- [x] release のビルド(警告 0)
- [x] probe_liquid・probe_phase・cell_volume が通る(CPU)
- [x] gpu_probe_liquid(HW・WARP)が通る = CPU と GPU が毎刻みビット一致
- [x] archmap --check

## メモ・参考
- 入力の分け方(T-0717 の MR の 1 段がそのまま呼べるように):
  - セルの見立て `LqCellView`(液体の質量・体積・空き・満ち具合の換算 equivalent・熱・熱容量。永続しない)。
    T-0713 の `MakeCellView` ができたら、その液体の分(相ごとの質量・体積・固体の体積・熱・熱容量)を `LqMakeCellView` に渡すだけにする。
  - 面とセルの欄(面の運動量・面の運動のエネルギー・位置のエネルギー・前の質量): 関数は値で受けて値で返す(置き場は呼ぶ側。後で登録の欄)。
  - 見立ての値 `LqParams`(重力・セルの 1 辺・摩擦・溢れの倍率・速さの上限・移流)。セルの 1 辺とセルの体積は引数なので、レベルの関数にできる。
- 周りの決まり(区画・面の列挙・成分の書き戻し・帳簿への足し方)は呼ぶ側のまま。出ていく上限 `LqLimitOutflow` と欄が物質に付く `LqFollowMassChange`
  は式だけ共有(段 1 ④ の当てる段ができたらそちらへ移す)。
- 区画をやめる・MR へ載せるのは T-0717 以降。セルの体積の分け合い(sim/cell_volume の ShareCellVolume・GPU の固体の体積の数え)は T-0713 の見立てに任せて触らない。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(wt6・作業役): shaders/common/liquid_flow.hlsli を作り、CPU と GPU の式を全部そこへ寄せた(LqMakeCellView・LqColumnPressure・
  LqClosedFace / LqOpenFaceForce / LqMinusWallPush・LqFollowMassChange・LqLimitOutflow・LqMovedShare / LqMovedEnergy・LqAdvectShare・
  LqSettleFace / LqReleasedShare・LqPotentialEnergy / LqKineticEnergy / LqMaxMomentum)。ProbeLiquidCellState は LqCellView の別名(56 バイトのまま)。
  二重書きの減り(空行・コメントを除くコード行): probe_liquid.cpp 598 → 510(−88)・probe_liquid_tick.hlsl 681 → 571(−110)・probe_liquid_gpu.hlsli −4。
  共有 .hlsli は 256 行(式は 1 か所)。移す前に旧 CPU の式と新しい関数を 200 万回の乱数入力で比べてビット一致を確かめた(コンテナの g++)。
  HLSL は構造体の三項演算子が使えない(DXC: conditional operator only supports ... numeric)ので if で選ぶ。
  試験(release): probe_liquid・probe_phase・probe_phase_porous_stone・cell_volume・gpu_probe_liquid(HW)・gpu_probe_liquid_warp が通る。警告 0。archmap OK。
  次(T-0717): MR の 1 段の液体は、T-0713 の見立てから LqMakeCellView を作り、LqParams のセルの 1 辺・体積をレベルの値にして同じ関数を呼ぶ。
