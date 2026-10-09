# T-0154 陰解法の V サイクルの空の間接の Dispatch の固定費を減らす

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 2 時間以内(始める前の見積もり: 記録の形を刻みごとに選べるようにする〔gpu_implicit.* と ImTail の 1 行〕と試験・計測で 1.5 時間。
  Work Graphs で V サイクルを回す案は 2 時間に入らないので測らずに見送る、と決めて入った)
- マイルストーン: M2 の並走(D-432)
- 設計: docs/design/17-multiresolution.md §6 / 決定: D-428・D-432 / ADR-0019(T-0154 の追記)

## 目的(1〜2 行)
T-0136 で段の数を GPU が決めるようになり、CPU は記録の上限の段・上限の回まで Dispatch を積む。使わない段・止めた後の回も 0 グループの
ExecuteIndirect とバリアが残り、小さい場面(熱い点)が 0.26 → 0.6〜0.8 ms/刻みに重くなった。これを解いた値を変えずに減らす。

## 完了条件(チェックできる形で)
- [x] 記録の形(段ごとに積む段の数・段ごとに積む回の数)を刻みごとに選べる。どの形でも解いた値は CPU と毎刻みビット一致(HW・WARP)
- [x] 前の刻みの GPU の数から形を選ぶ関数(GpuImplicit::ShapeFrom)
- [ ] idle で前後を測って docs/perf.md に残す(下の「結果」)

## 何をしたか(形。決めたのは Claude〔実装の細部〕。ADR-0019 の T-0154 の追記)
- 費用の元: 熱い点は V サイクルを 3 回で止めるが、記録は上限 16 回 × 41 Dispatch(4 段ぶん・全部 0 グループ)。止めた後の回は述語で
  Dispatch を飛ばしてもバリアは残る(D3D12 の述語はバリアに効かない)ので、1 刻み約 650 個の空の Dispatch + バリアが残っていた(T-0136 の前は 16 × 3)。
  GPU の側で記録を短くすることはできない(バリアは CPU が積んだ数だけ走る)ので、CPU が積む数を減らすしかない。
- `GpuImplicitRecordShape { dispatchLevels, coarsestDispatch }` を RecordStep に渡す(既定は今までと同じ = 上限まで・最も粗い段の掃き出しも積む)。
  - dispatchLevels: 段ごとの Dispatch を積む段の数。ImPlanArgs も同じ数で ImTail の境を切る(下りがそれより深い系は ImTail が回す。値は同じ)。
  - coarsestDispatch: 最も粗い段の掃き出し(色 × 4 回 = 8 Dispatch)を積むか。false なら ImPlanArgs が最も粗い段をいつも ImTail にする。
  - 全部の段が ImTail の系(熱い点)は V 1 回 3 Dispatch(ImTail・判定・回の終わり)に戻る。
- 計画に「形で切る前の ImTail の境」(IM_PLAN_WANTED_TAIL)を足し、GpuImplicitCost::wantedTailDepth で読む。切った後の値から形を選ぶと、
  一度浅くした形が深くなった系に戻れない(最初の版で鎖が段 2 に張り付いたのを計測で見つけて直した)。
- `GpuImplicit::ShapeFrom(前の刻みの GpuImplicitCost)`: 段は前の刻みで下りが止まった段まで、最も粗い段の掃き出しは前の刻みが ImTail を
  使わなかった時だけ。形が深くなった刻みは、その分を ImTail が回す(値は同じ・その刻みだけ遅い。次の刻みで形が追いつく)。前の数は遅れて読んだものでもよい。
- 試した後に外したもの: 止めた後の回を「ImTail が段 0 から全部」の安い回にする(fullCycles)。回が前の刻みより増えた刻みで大きい系を 1 グループで
  回す崖があり(たくさんの要求を全部 ImTail で回すと 9.7 → 71 ms/刻み・負荷あり)、全部 ImTail の系ではもともと同じ数になるので要らない。
- 試験: gpu_multires_implicit_tree_test は 3 つの形(前の刻みから・全部 ImTail・上限まで)で CPU と毎刻み一致を確かめる(全部 ImTail のたくさんの要求は release だけ)。
  gpu_multires_implicit_build_test(GPU の系を上限から作った GpuImplicit で解く)は前の刻みから選ぶ形で解く。計測に形の前後を足した。
