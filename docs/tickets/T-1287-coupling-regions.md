# T-1287 区画が 2 つ以上の世界で境の面の案 (c) の GPU の刻みを選ぶ段を確かめる(CPU とビット一致。既定は None のまま)

- Status: Done(2026-10-30 wt6・t-1287-coupling-regions。既定は None のまま)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 6(docs/plan/ROADMAP.md)
- 設計: docs/design/20-transport-substrate.md §14.2 / 決定: ADR-0493・ADR-0507(Proposed)

## 目的(1〜2 行)
T-1271 で GPU の刻みを選ぶ段に足した案 (c) の表は、試験がどれも区画(1 グループが受け持つ 4,096 単位)1 つだった。
区画が 2 つ以上の世界(区画が接する・離れる)で、区画をまたぐ道(区画の外の隣を g_ttcState から読む・掃く回の 2 回目以降・仕上げ TtcFixup)を (c) で通し、CPU とビット一致を確かめる。

## 完了条件(チェックできる形で)
- [x] time_step_tree_test の choose-dump に根の並び(roots=XxYxZ)。shape= の木を広げて区画を 2 つ以上に
- [x] gpu_time_step_choose_test に --expect-crossing(区画が 2 つ以上で、区画をまたいで縮みが伝わった出来事があることを求める = 道を通ったことの確かめ)
- [x] ctest: 火事 16×1×1(2 区画)・火事 8×2×2(4 区画。接する・離れる)・池 32×1×1(4 区画)を (c) c0 で、GPU と CPU がビット一致
- [x] 前からの試験(gpu_time_step_choose・_coupling・_coupling_night)はそのまま通る
- [x] release のビルド警告 0・archmap --check

## メモ・参考
- 区画の並び: 単位はセルの番号(ブロックの枠 × 512 + セル)の順。根は x → y → z の順に置くので、8×2×2 の木の 1 区画 = x の根 2 つ × y 2 × z 2。区画は x に並び、
  区画 0 と 1 は接し、0 と 2 は離れる(仕上げは「最後の回に変わった区画の隣」だけを印すので、離れた区画に印が行かないことも通る)。
- (c) の規則は単位ごとの表 loose と隣の今の e だけを読む(隣の e について単調)ので、区画の外の隣の古い値(大きいだけ)を読んでも同じ最大の不動点に落ちる
  はず(T-1271 のメモ)。ずれたら、区画の外の隣を読む所(g_ttcState)と表の添字(区画の始まり + local)を先に疑う。

## 結果(2026-10-30・RTX 3070 Ti・release・compute キュー・`--no-gbv`。どれも border=c0・CPU とビット一致〔窓の上限・選んだ e・後の詰めた記録〕)
| 試験 | 木 | 単位 / 区画 | 出来事(書いた) | 打ち切り | 隣の差で縮んだ(CPU) | 変わった掃く回の最大 | 区画の中の回の最大 | 仕上げの区画の最大 |
|---|---|---|---|---|---|---|---|---|
| gpu_time_step_choose_coupling_fire_2 | 火事 0.5 m 16×1×1 | 8,192 / 2 | 277 | 37,504 | 38,736 | 2 | 63 | 0 |
| gpu_time_step_choose_coupling_fire_4 | 火事 0.5 m 8×2×2 | 16,384 / 4 | 277 | 68,096 | 60,368 | 4 | 16 | 3 |
| gpu_time_step_choose_coupling_night_4 | 池 0.5 m 32×1×1 | 16,384 / 4 | 11 | 0 | 117,476 | 4 | 63 | 3 |
- 区画をまたぐ道はどれも通った(1 回目の後の掃く回で区画が変わった = 区画の外の隣を g_ttcState から読んで縮んだ。4 区画の 2 本は仕上げ TtcFixup も 3 区画を回した)。直すものは無かった。
- 前からの 3 本(gpu_time_step_choose・_coupling・_coupling_night。区画 1 つ)もそのまま合格。CPU の dump は 31 秒・99 秒・10 秒(ctest の上限 900 秒の内)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-30(wt6・t-1287-coupling-regions): choose-dump に roots=XxYxZ(shape= の木の根の並び)、gpu_time_step_choose_test に --expect-crossing(区画 2 つ以上で、
  2 回目以降の掃く回か仕上げで区画が変わったことを求める)、ctest 3 組(火事 2 区画・火事 4 区画・池 4 区画。c0)を足した。release 警告 0・6 本の GPU 試験と dump が合格・
  archmap --check OK。シェーダー・エンジンのコードは変えていない(試験だけ)。
