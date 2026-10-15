# T-0591 気体の成分の上限 4 を外す(気体の段をセルの成分の並びで回す)

- Status: Done(CPU。GPU と木は T-0598・比熱のパッケージと既定は T-0597)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 2 回分(CPU・GPU で分けてよい。番号は司令塔)
- マイルストーン: ROADMAP の T-0210(T-0590 から分けた)
- 設計: docs/design/07-transport.md §2.7 / 決定: ADR-0256

## 目的(1〜2 行)
気体の段(gas_reference・gas_tree・gas_projection・GPU の gas_step・gas_tree_step)の成分の欄(GAS_MAX_SPECIES = 4・GasCell は 64 バイトで GPU と同じ形)を、
セルの成分の並び(物質 ID・RxCell のインラインの 8 + 溢れ)で回す形にし、区画に何種類の気体があっても流れるようにする。

## 完了条件(チェックできる形で)
- [x] 5 種以上の気体(地球の空気 N2・O2・Ar・CO2 + 水蒸気 + CO)の区画が流れる(CPU。probe_gas_test の 6. 流さなかった刻み 0)
- [x] 4 種以下なら今の gas_reference・gas_tree・gas_projection の試験とビット一致(式は変えず、欄の入れ物だけ替えた。試験はそのまま通る)
- [x] 定積の比熱(仮。ADR-0259 Proposed): 表の選択 gasHeatCapacityAtConstantPressure でベイクが気体の c_v = c_p − R を作り、セルの温度もそれで読む。既定は今まで通り
- [x] 2 回の実行でハッシュ一致・保存則の検査(陽解法・投影。6 種)。GPU は T-0598(4 種まで。GpuGas::Create が断る)

## メモ・参考
- 乱数の丸めの「用途 + 成分」は成分の欄の番号ではなく物質 ID で(並びが変わっても同じ値)。
- GPU は書き換えた構造体の配列を動的な添字で読まない(T-0397)。成分の数で回すループは FX_UNROLL の決まった数で。

## 判断待ち(仮で進めたもの。作り手から見た違いで)
1. **比熱を定圧で書くか**(ADR-0259。T-0590 の判断待ち 3): 仮 = 表ごとの選択を足し、既定は今まで通り(書いた値を定積として読む)。
   おすすめは既定を「定圧で書く」(案 A): 作り手は資料の c_p をそのまま書け、閉じたセルの気体の温まり方・断熱の冷え方が正しくなる。
   違い: real_matter の世界の気体は同じ熱で約 1.4 倍温まる(N2 29.1 → 20.8)・場面と GPU の試験の数値が動く・凝縮相に condensed を書き忘れると
   比熱が R だけ小さくなるか断られる。パッケージの書き場所と切り替えは T-0597。
2. **1 セルの成分がインライン 8 を超える刻み**: 仮 = その刻みは区画の気体を流さない(何も変えず数える。skippedCellOverflow)。
   違い: 気体の種類が多い区画で、まれに煙が 1 刻み止まる(結果は変えるが保存は守る)。本当の直しは溢れのセル(R-MULTI-4。wt3 T-0558 が成分の数え方を触っている)。

## 並走との重なり
- engine/src/sim/probe_gas.cpp の Step だけを触った(ProbeReference::Advance は触っていない。wt4 T-0584/T-0585・wt6 T-0027 と重ならない見込み)。
- wt3 T-0558(1 セルの量の上限を相で分ける)が RxCell の成分の数え方を変えるなら、probe_gas の MergeCellSpecies(書き戻す成分)と合わせる。
- gpu_gas.cpp は帳簿の写し(GpuLedger)と断りだけ(wt5 T-0587 は投影の V サイクルで、別のファイルの見込み)。
- reaction_table.{h,cpp} に欄 1 つとベイクの分岐 1 つ(既定 false なので他の表と GPU の試験は変わらない)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-15(wt2): 1 レベルの箱の成分ごとの値を GasPerSpecies(4 つまで中・5 つ目から外)にし、丸めの鍵を GasSpecies::roundingKey(probe_gas は物質 ID)に。
  GPU と木は 4 つのまま断る(T-0598)。probe_gas の上限 4 を外し、インライン 8 を超える刻みだけ流さない。比熱の選択(ADR-0259)と ADR-0258。
  probe_gas_test に 6 種の空気・定積の比熱・費用の表示を足した(6 種: 最大 6 種・流さない刻み 0・保存・決定性 yes。
  費用は 4 種 258 ms/刻み・6 種 189 ms/刻みで並走の揺れの内。docs/perf.md)。4 種の燃える木箱の数値は丸めの鍵が物質 ID になって少し動いた
  (熱の重心 13.08 → 11.82 セル・CO2 の重心 10.07 → 9.39)。release ビルド警告 0・ctest(probe_gas・gas_reference・gas_projection・gas_tree・drag_model・world_atmosphere・environment_package・reaction_table_edit)通過。
