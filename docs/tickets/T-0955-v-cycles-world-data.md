# T-0955 C-0 の 8: V サイクルの回数を世界のデータで選ぶ口

- Status: Done(2026-10-22・wt2。T-0922 と同じブランチ t-0922-warmstart)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(小さい)
- マイルストーン: 試作の列 1(C-0。T-0918 から分けた)
- 設計: docs/design/20-transport-substrate.md §9.2・§13.1 / ADR-0397・ADR-0406 / 根拠: T-0900 M-1(水と空気の境で V 2 は 69 Pa・V 4 は 2.7 Pa)

## 目的
部品は V の回数を solver.cycles(EquilibriumOptions・GpuEquilibriumOptions)で持つ。これを法則ごとに世界のデータ(パッケージの値)から入れる口を作る。
R1 で本物の硬さの水(V 4)が候補になる。

## 完了条件
- [x] 世界のデータの法則ごとの値(既定 2)→ 部品の solver.cycles(CPU・GPU とも): 環境の `balance_cycles = { gas_pressure, liquid_pressure, support }` →
      `WorldPhysics::balanceCycles` → `ApplyBalanceCycles`(気体の決まった回数の道。GPU は CPU の solver を写す)・`ApplySupportCycles`(支えの 2 つの解き)。
      仮の世界の気体(ProbeGasField)は表の環境から毎刻み入れる(決まった回数の道だけ。許容差で止める道は上限のまま)
- [x] GPU の記録の形が回数ごとに正しく積まれる。V 1・2・4 で CPU とビット一致: gpu_gas_projection_t0915 の FullOptions を世界のデータの値から入れる形にした(保存の組 V 1・2・4)
- [x] 値が上限 16 を超えたら読み込みで知らせる: 範囲 1〜16 の外・小数・知らない法則・表でない値は名前付きの誤り(environment_package)。ホットリロードの検査(CheckWorldPhysics)も同じ範囲

## メモ
- 判定で早く止める口は持たない(§9.2 ユーザー決定)。回数はちょうど。

## 仮(ユーザー未確認)
- 鍵の名前 `balance_cycles` と法則の名前(gas_pressure・liquid_pressure・support)・上限 16(費用は回数に比例。16 は V 2 の 8 倍)。ADR-0427 Proposed。
- 液体は今 C-0 の部品に乗っていない(案 B の R1 は熱の多重格子を許容差で止める形。T-0868)ので、`liquidPressure` は読む所が来た時(液体が部品に移る時)に使う。R1 の水を V 4 にするかは T-0971 で測ってから(ゲームの中身ではなく法則の定数。値で選べる)。
- 環境の値なので世界ごとに 1 つ(場所ごとに変えない)。

## 作業ログ
- 2026-10-22(wt2): 環境の 1 行・WorldPhysics・読み込みの誤り・ホットリロードの範囲・気体と支えへ渡す口。gpu_gas_projection_t0915 の V 1・2・4 を世界のデータから。型の定義(data/types/bicameral.d.luau)に BalanceCycles。ADR-0427。
