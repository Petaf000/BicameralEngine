# T-1369 lab_box の 6 件(試験の前提)を直して DISABLED を外す

- Status: Todo(2026-11-12 wt5 が T-1368 から分けた)
- 種類: 工学(試験の側だけ)
- PC: 必須
- 見積もり: チャット 0.3 回分(1 時間)
- マイルストーン: 列 4
- 設計: docs/tickets/T-1368-lab-probe-failures.md の表 L1〜L6 / 決定: D-637・ADR-0505

## 目的(1〜2 行)
lab_box(CPU)の 6 件の失敗はどれも試験の前提が現実の値(D-637)と合っていないもので、多重解像度の放射(T-1155)を待たずに直せる。直して ctest に戻す。

## 完了条件
- [ ] TestFire(208)・TestGauge(426): 燃えた印を炭素の酸化物(CO2 + CO)で見る・計器は CO の速さの和 = 量の差(炭の大過剰の熱いセルは Boudouard で CO になる)
- [ ] TestTableMark(347・348): 刻み SWAP_TICK に燃えていない木 (4, 3, 3) を 600 K に(gpu_lab_box の SWAP_IGNITE_CELL と同じ)。ハッシュは刻み SWAP_TICK、セルロースは SWAP_TICK + 10 で比べる
- [ ] TestRefinedHeatSpeed(727・769): `MakeFastConductionTestTable()` の表で回す(ADR-0505 の決定 2 に lab_box を 1 行足す)。前の倍率の表で基準が 0〜1 に入るかを先に確かめる
- [ ] tests/CMakeLists.txt の lab_box の DISABLED と止めの注を外す。lab_box・gpu_lab_box〔HW・WARP〕合格・release 警告 0

## メモ・参考
- T-1368 の一時の表示で (2) の差が出ることは確かめた(刻み 101 ハッシュ違う・刻み 110 セルロース 243106631 / 231600000。刻み 300 では同じ)。
- 判定を緩めるのではなく、確かめる物を物理に合わせる(燃えた・表を替えた刻みから変わる・頭打ちの時に陰解法が理論の値に合う、の意図は変えない)。

## 作業ログ
