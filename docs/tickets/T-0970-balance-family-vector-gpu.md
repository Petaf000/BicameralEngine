# T-0970 C-0 成分を持つ節の GPU 版(8 色・k × k のブロック)

- Status: Todo(T-0919 から分けた)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列 1・列 3(支えの法則)
- 設計: docs/design/20-transport-substrate.md §13.1 / ADR-0406・ADR-0411・ADR-0418

## 目的
T-0919 の SolveEquilibriumBlocks(CPU)を GPU の Compute に載せ、CPU とビット一致させる。支えの GPU 版(T-0946)の下地。

## 完了条件
- [ ] 式を shaders/common/ の hlsli に移し CPU と共有(節ごとの LDLᵀ・ブロックの残差・延ばし方と縮約。決まった数の回し + FX_UNROLL。書き換えた構造体の配列を動的な添字で読まない〔T-0397〕)
- [ ] 8 色を色ごとの Dispatch で。Galerkin の粗い段(Pᵀ K P の 128bit の和)は CPU で作るか GPU で作るかを決める
- [ ] CPU と GPU のビット一致(支えの張り出し・片持ち梁・アーチ・洞窟を毎刻み)・2 回の実行でハッシュ一致
- [ ] 1 節の費用(k = 3 で k² 倍の見込み)と VRAM を docs/perf.md に

## メモ・参考
- 口と結果は docs/tickets/T-0919-balance-family-vector-nodes.md・ADR-0418。作業場の詰め方は ADR-0411。

## 作業ログ
