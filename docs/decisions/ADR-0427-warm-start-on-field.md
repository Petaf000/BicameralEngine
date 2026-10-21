# ADR-0427 釣り合う家族の温めた始まりを C-1 の欄に置く口・V の回数を世界のデータで選ぶ口

- Status: Accepted の所(欄に置く口の形・ビット一致の確かめ方)/ Proposed の所(環境の鍵の名前 balance_cycles・既定 2・上限 16。仮・ユーザー未確認)
- 日付: 2026-10-22
- 決めた人: Claude(wt2 の作業役。T-0922・T-0955)。芯はユーザー決定 D-608((A) 解き始めをハッシュとセーブに入れる・重ければ切れる)・D-541(V は法則ごとの定数)
- 関係: ADR-0373(温めた始まりをセーブに入れる)・ADR-0397(部品)・ADR-0399(欄の種類)・ADR-0403(疎な頁)・ADR-0418(成分を持つ節)
- 設計書: docs/design/20-transport-substrate.md §9.2・§10.4・§13.2 の 3・§13.3

## 背景
- C-0 の部品は温めた始まり(前の刻みの答え)を法則の控え(`EquilibriumState::values`・`EquilibriumBlockState::values`・`GasProjection`)に持っていた。
  控えは保存点にもハッシュにも入らないので、保存点から続けると最初の刻みの解き始めが違い、答えがずれる(D-608 の (B) の振る舞い)。
- C-1 は温めた始まりの印(`SF_FLAG_WARM_START`)を持つ欄を用意した(ADR-0399)。20 §13.3: C-0 の温めた始まりは C-1 の印ができたら付け替える。
- V の回数は部品の `solver.cycles`・`blocks.cycles`(既定 2)にあり、世界のデータから入れる口が無かった(M-1: 水と空気の境で V 2 は 69 Pa・V 4 は 2.7 Pa)。

## 決定
1. **欄に置く口**: 部品の解きの設定に `EquilibriumWarmStart { nest, field, component }` を持たせる(`EquilibriumOptions::warmStart`・
   `EquilibriumBlockOptions::warmStart`。使い手は `GasProjectionOptions::warmStart`・`SupportOptions::warmStart` で渡す)。
   null でなければ、部品は解く前に節の答えを欄から読み(`LoadWarmStart`)、解いた後に書く(`StoreWarmStart`)。法則の控えは写しになる。
   節 → 欄の引き方: 部品の節は木のセル 1 つ(`EquilibriumNode` のレベルと座標)なので、`LookupBlock`(原点 = 座標 & ~7)→ `MrCellIndex`。
   眠った(一様な)ブロックは眠った時の平均を読み、書かない(起きている所だけ解く約束。畳む時の平均は ADR-0399)。
2. **登録は 1 行**: `RegisterWarmStartField(nest, name, phase, k)` = `SfMakeDerivedDesc(SF_PLACE_CELL, SF_TYPE_I64, phase, true, k)`(T-0854 の申し送りの形)。
   成分ごとに解く法則(支えの遅らせる形)は 1 つの欄の値 component + c を成分 c に使う。
3. **欄に置かない道は前のまま**(既定 null)。欄に置いても答えはビット単位で同じ(読む値 = 前に書いた値)。
4. **V の回数は環境の 1 行**: `environment.balance_cycles = { gas_pressure, liquid_pressure, support }`(1〜16。書かない法則は 2)→
   `WorldPhysics::balanceCycles` → `ApplyBalanceCycles`(気体の決まった回数の道の `solver.cycles`。GPU は CPU の solver を写すので同じ)・
   `ApplySupportCycles`(支えの `blocks.cycles` と `equilibrium.solver.cycles`)。範囲の外・知らない法則・小数は読み込みで名前付きの誤り。
   ホットリロードの検査(`CheckWorldPhysics`)も同じ範囲を見る。液体は C-0 の部品に移る時(案 B の R1。T-0868)に `liquidPressure` を読む。

## 採らなかった案
- 部品が木を知らず、法則が解く前後に欄と控えを写す: 使い手ごとに同じ写しを書くことになり、書き忘れると保存点からずれる(D-608 の約束が使い手任せになる)。
- 閉じた領域の種の控え(`sourceRhs`・`diagonals`・`residuals`)も欄に置く: 閉じない今の使い手には要らない。閉じる法則で保存点から続けると最初の刻みが全部種になる
  (答えは変わりうる)。要るかは閉じた領域の使い手が来た時に測る(T-0993)。
- V の回数を法則ごとの別の鍵(gas_pressure_cycles など)にする: 釣り合う家族の法則が増えるたびに鍵が増える。1 つの表にまとめた。

## 結果
- しやすく: 釣り合う家族の新しい法則は欄を 1 行登録して `warmStart` に渡すだけで、保存点から続けてもビット一致(D-608 (A))。(B) へは `SF_WARM_START_IN_STATE` の 1 行。
- 確かめ(tests/warm_start_save_test.cpp): 気体の陰の圧力 5 場面 16³ と爆発 32³・支え(1 つの系の片持ち梁・洞窟、遅らせる形の片持ち梁)で、欄に置いた答えが控えだけと毎刻み一致・
  保存点から続けて HashGas / SupportLaw::Hash と HashWholeNest が毎刻み一致・欄を 0 にする((B) の模擬)とずれる。セーブの大きさと費用は T-0922 のチケットと docs/perf.md。
- 残り: 本物の世界の陰の圧力・支えの欄は、それらが MR の木の上に来た時(T-0981・案 B の R1)に同じ口を使う。GPU の部品(GpuEquilibrium・GpuEquilibriumBlock)の前の答えは
  GPU のバッファにあり、欄が GPU に来る段(T-0920)で同じ欄を読む(T-0994)。
