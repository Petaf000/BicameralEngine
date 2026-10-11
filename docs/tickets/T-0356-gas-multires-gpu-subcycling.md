# T-0356 気体: 多重解像度のレベルごとの 2^σ の小刻みを GPU で(T-0209 から分けた)

- Status: Done(2026-10-11)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 1 体(2 時間以内)
- マイルストーン: M3 (docs/plan/ROADMAP.md)
- 設計: docs/design/07-transport.md §2.5・ADR-0117 追記(T-0320・T-0209)/ 関係: T-0320(CPU)・T-0209(σ = 0 の GPU)

## 目的
T-0320 の時間線(いちばん細かい小刻みの列。各 micro ではそこで始まるレベルの面とセルだけ動かす)を GPU の段の列に組み、gas_tree の
`GasTreeOptions{.maxSubcycleGap = 1}` の木で HW・WARP が CPU と毎刻みビット一致する。

## 完了条件
- [x] `GpuGasTree::Create` の σ > 0 の拒否を外す。セルごとに forced・next(と端数)・committedOutflow・taken(成分ごと)を粗い小刻みの間持つ(バッファを足す)
- [x] 段: 始めるレベルのセルだけ導く値・forced・next を作り直す / 始めるレベルの面だけ計算(面の一覧をレベルで分けるか、段の中で飛ばす)/ 粗い側への力積と塊は next へ /
      終えるレベルのセルが next を受ける。乱数の小刻みの通し番号はレベルごと(ルート定数に micro とレベルごとの番号の作り方を渡す)
- [x] 試験: gpu_gas_tree_test に分けた形の場面(gas_tree_test の SUBCYCLED の閉じた箱・静止大気)を足し、HW・WARP で毎刻みビット一致

## メモ
- T-0209 の段はそのまま σ = 0 の特別な場合になるように書く(1 刻みの段の数は いちばん細かい小刻みの数 × 9。空の段は抜けるか、セル・面の数 0 で Dispatch しない)。

## 作業ログ
- 2026-10-11(Claude・作業役 wt2): gas_tree_step.hlsl の 9 段を T-0320 の時間線に(段 1 = 始めるセルの導く値・forced・carry の作り直し、段 3 = 同じ小刻みの面の
  力積は forced・細かい小刻みの面の力積は next、段 9 = 塊を next へ・carry を足す・終えるセルだけ next を状態へ。面の段は始めないレベルの面を飛ばす)。
  バッファを 3 つ足した(u8・u9 = next とその端数、u10 = `GasTreeGpuCarry`〔committedOutflow・taken〕)。ルート定数は tick・micro・substeps・σmax(8 個)。
  `GasTreeGpuLevel` の空き欄を `subcycleExponent` に。CPU の `StepGasTree` に試験用の `GasTreeStepStats`(抑えを通った回数。状態には効かない)を足した。
- 初めのビルドで通り、HW・WARP とも 9 場面(T-0209 の 5 + subcycled_closed・subcycled_static・subcycled_open_wind・subcycled_fast)が初めから毎刻みビット一致。
  subcycled_fast(粗いセルの列が 1500 m/s で細かい範囲へ)の 4 刻みで CPU の抑えは 割合 1847 回(粗い風上 203・使える分 < 1 が 52)・物質量 837 回(taken を引いた 72)。
- 試験の時間(debug、6 つのランナーの並走の負荷の下): gpu_gas_tree(HW。GPU-based validation)228 秒・gpu_gas_tree_warp 47 秒・gas_tree 153 秒。
- clang-tidy(gpu_gas_tree.cpp・gas_tree.cpp・gpu_gas_tree_test.cpp): 自分の分 0 件(LimitMovedAmounts の大きさの警告は数える所を関数に出して直した。
  fixed.hlsli の designated initializer の警告は前からある)。archmap --check: OK。ADR-0117 追記・07 §2.5・map.yaml。

## 引き継ぎメモ
- 使い方は T-0209 と同じ(`GpuGasTree::Create(device, tree)`。tree = `MakeGasTree(..., GasTreeOptions{.subcycleBaseLevel = 0, .maxSubcycleGap = 1})` でもよい)。
  1 刻みの段の数 = substeps × 2^σmax × 9(レベル 0 が 24・レベル 1 が 48 なら 48 × 9 = 432。σ = 0 で substeps 48 の T-0209 と同じ数)。
- 始めないレベルの面・セルは段の中で飛ばしている(Dispatch はいつも全部の面・セル)。面の一覧をレベルで分けて小さく Dispatch するかは費用を測ってから(T-0358)。
- `GasTreeStepStats` は試験で抑えの道を通ったかを見るためだけ(gas_tree_test は使っていない)。

## 判断待ち
- なし(実装の細部だけ。ADR-0117 追記)

## 分けたもの
- なし(費用の計測は前からある T-0358)
