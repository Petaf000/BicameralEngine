# T-0955 C-0 の 8: V サイクルの回数を世界のデータで選ぶ口

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(小さい)
- マイルストーン: 試作の列 1(C-0。T-0918 から分けた)
- 設計: docs/design/20-transport-substrate.md §9.2・§13.1 / ADR-0397・ADR-0406 / 根拠: T-0900 M-1(水と空気の境で V 2 は 69 Pa・V 4 は 2.7 Pa)

## 目的
部品は V の回数を solver.cycles(EquilibriumOptions・GpuEquilibriumOptions)で持つ。これを法則ごとに世界のデータ(パッケージの値)から入れる口を作る。
R1 で本物の硬さの水(V 4)が候補になる。

## 完了条件
- [ ] 世界のデータの法則ごとの値(既定 2)→ 部品の solver.cycles(CPU・GPU とも)
- [ ] GPU の記録の形(GpuImplicit::ShapeFrom・fixedCycles)が回数ごとに正しく積まれる。V 1・2・4 で CPU とビット一致(gpu_gas_projection の --t0915 の保存の組を世界のデータの値から回す形に)
- [ ] 値が上限(例 16)を超えたら読み込みで知らせる(書き間違いの知らせ。20 §1.4)

## メモ
- 判定で早く止める口は持たない(§9.2 ユーザー決定)。回数はちょうど。

## 作業ログ
