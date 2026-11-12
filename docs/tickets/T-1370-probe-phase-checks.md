# T-1370 probe_phase の場面の線だけを外した版を ctest に戻す

- Status: Todo(2026-11-12 wt5 が T-1368 から分けた)
- 種類: 工学(試験の側だけ)
- PC: 必須
- 見積もり: チャット 0.2 回分(30 分 + 流す 11 分)
- マイルストーン: 列 4
- 設計: docs/tickets/T-1368-lab-probe-failures.md の probe_phase の表 / 決定: D-665・D-666・T-1160 の判断待ち 3

## 目的(1〜2 行)
probe_phase は場面の線(湯気・池・氷の量)だけが落ちるのに、DISABLED のせいで 1 セルの釣り合い・潜熱・密度のある石の保存・溶岩の上の水の気体の器・決定性まで
2026-10-24(T-1066)から ctest で見ていない。場面の線を外した版を戻して守る。

## 完了条件
- [ ] probe_phase_test に引数 `--no-scene-lines`(outcomeChecks = false で TestTable から ReportQuietPond まで全部流す)
- [ ] ctest `probe_phase_checks`(TIMEOUT 1800)を足す。probe_phase(場面の線つき)は T-1219 まで DISABLED のまま
- [ ] probe_phase_checks・probe_phase_porous_stone 合格・release 警告 0

## 作業ログ
