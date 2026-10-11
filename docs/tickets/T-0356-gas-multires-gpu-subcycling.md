# T-0356 気体: 多重解像度のレベルごとの 2^σ の小刻みを GPU で(T-0209 から分けた)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 1 体(2 時間以内)
- マイルストーン: M3 (docs/plan/ROADMAP.md)
- 設計: docs/design/07-transport.md §2.5・ADR-0117 追記(T-0320・T-0209)/ 関係: T-0320(CPU)・T-0209(σ = 0 の GPU)

## 目的
T-0320 の時間線(いちばん細かい小刻みの列。各 micro ではそこで始まるレベルの面とセルだけ動かす)を GPU の段の列に組み、gas_tree の
`GasTreeOptions{.maxSubcycleGap = 1}` の木で HW・WARP が CPU と毎刻みビット一致する。

## 完了条件
- [ ] `GpuGasTree::Create` の σ > 0 の拒否を外す。セルごとに forced・next(と端数)・committedOutflow・taken(成分ごと)を粗い小刻みの間持つ(バッファを足す)
- [ ] 段: 始めるレベルのセルだけ導く値・forced・next を作り直す / 始めるレベルの面だけ計算(面の一覧をレベルで分けるか、段の中で飛ばす)/ 粗い側への力積と塊は next へ /
      終えるレベルのセルが next を受ける。乱数の小刻みの通し番号はレベルごと(ルート定数に micro とレベルごとの番号の作り方を渡す)
- [ ] 試験: gpu_gas_tree_test に分けた形の場面(gas_tree_test の SUBCYCLED の閉じた箱・静止大気)を足し、HW・WARP で毎刻みビット一致

## メモ
- T-0209 の段はそのまま σ = 0 の特別な場合になるように書く(1 刻みの段の数は いちばん細かい小刻みの数 × 9。空の段は抜けるか、セル・面の数 0 で Dispatch しない)。
