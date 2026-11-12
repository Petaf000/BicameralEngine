# T-1370 probe_phase の場面の線だけを外した ctest probe_phase_checks を足す(試験の側だけ)

- Status: Done(2026-11-13 wt5)
- 種類: 工学(試験の側だけ)
- PC: 必須
- 見積もり: チャット 0.3 回分
- マイルストーン: 列 4(元: T-1368 の probe_phase の表)
- 設計: docs/design/07-transport.md / 決定: D-637・D-665・D-666 / 元: T-1368・T-1160・T-1219

## 目的(1〜2 行)
probe_phase は場面の線(湯気・池・氷の結果。EXPECT_OUTCOME の 6 本)が現実の伝導率の仮の世界で成り立たず DISABLED のため(T-1368 P1〜P6)、
2026-10-24 から場面の線以外の段(1 セルの釣り合い・潜熱・密度のある石の保存・溶岩の上の水・決定性)も ctest で見られていなかった。それを毎回見る。

## 完了条件
- [x] probe_phase_test に `--no-scene-lines`(outcomeChecks = false で、全部の段を流す)
- [x] ctest probe_phase_checks(TIMEOUT 1800)を足した。probe_phase(場面の線つき)は T-1219 まで DISABLED のまま(止めの注を T-1110 から T-1219 へ直した)
- [x] probe_phase_test --no-scene-lines が release で合格

## メモ・参考
- 場面の線を戻すのは、場面を世界の木へ移す時(T-1219。T-1160 の判断待ち 3 の仮)。
- 流す時間は 640 秒前後(CPU。T-1368 の測り)。main 10213abf の上の ctest では 1063 秒(ほかの作業ツリーのビルドと重なった。TIMEOUT 1800)。

## 作業ログ
- 2026-11-13(wt5 の作業役): 引数と ctest を足し、release の exe(`probe_phase_test --no-scene-lines`)を raw で直接流した: すべて通った(FAILED 0)。
  場面の線以外の段(表と 1 セル・湯気 2 回と氷の保存・溶岩の上の水 気体のぶんの最大 389.458 mol・決定性)が合格。ctest の probe_phase_checks は同じ引数。
