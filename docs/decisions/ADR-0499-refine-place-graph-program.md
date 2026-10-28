# ADR-0499 木を細かくする段の置かれ方は、別の Work Graph のプログラム(multires_graph_place)で走らせる

- Status: Accepted(2026-10-29。工学の形。CPU とのビット一致で答えは決まり、作り手から見た違いは無い)
- 日付: 2026-10-29
- 決めた人: Claude(wt2 の作業役。T-1188)
- 関係: T-1188・T-1176・T-1233・T-0397・T-0124・D-664・D-546・ADR-0373・ADR-0469
- 試作: shaders/sim/multires_graph.hlsl(MR_REFINE_PLACE)・shaders/sim/multires_refine_place.hlsli・engine/src/sim/gpu_multires.{h,cpp}(GraphProgram)・
  tests/gpu_tree_refine_placement_test.cpp

## 背景
T-1188 で、木を細かくする段(multires_graph.hlsl の RefineLevel)に「寄せるセルの中身を置かれ方で配る」段(PlaceRefineChildren。
512 スレッド・グループ共有 6 KiB・64bit の割り算の回し)を足した。CPU とのビット一致(gpu_tree_refine_placement の HW・WARP)は合格したが、
**置かれ方を使わない木の HW の gpu_multires(chains-real)が RTX 3070 Ti で DEVICE_HUNG**になった(WARP は合格・main では合格)。
- 置かれ方の段の呼び出しを外すと通る。グループ共有を 12 → 6 KiB にしても止まる。
- 置かれ方の段を**同じプログラムの別の入口のノード**(RefinePlaceNode)に分け、使わない世界は軽い RefineNode を通しても止まる
  = 同じ Work Graph のプログラムに重いノードがあるだけで、ほかのノードも止まる。

## 案
1. **別のプログラム**: multires_graph.hlsl を `MR_REFINE_PLACE=1` でもう 1 つコンパイルし(multires_graph_place)、置かれ方の世界
   (`nest.refineCells.species` が空でなく溢れを使わない)だけそのプログラムで細かくする / 粗くする / 影を扱う。既定のプログラムは置かれ方の段を持たない(main と同じ)。
2. **細かくする段の置かれ方を Compute へ**(D-546 の既定)。細かくする鎖は 1 回の DispatchGraph で何段も再帰し、2 段目は 1 段目が配った頁を読むので、
   レベルごとに Graph を切って間に Compute を挟む形になる(鎖の作りが変わる。大きい)。
3. 同じプログラムのまま(1 つのノードか別の入口)で、止まる根を消す。

## 決めたこと
- **案 1**。`GpuMultires` は木を変えるプログラムを 2 つ持ち(`GraphProgram` = Work Graph・初期化済みか・入口 4 つの番号)、
  `RecordUpload` が世界の木で選ぶ(`m_activeGraph`)。GPU の入力の見出しが使う入口の番号(`g_graphEntries`)も選んだプログラムの番号。
  溢れを使う世界(multires_graph_wide)は置かれ方を使わないので 2 つ目を作らない。
- 置かれ方の段は `#ifdef MR_REFINE_PLACE` の中だけ。RefineNode の入口は 1 つに戻した(RefinePlaceNode は無くした)。
- 案 2 は取らない: 案 1 で HW の止まりが消え、細かくする鎖を割る大きな作り直しが要らない。置かれ方の世界の大きい場面で止まる・遅いと分かったら案 2 に移る(打ち切りの合図)。

## 根の切り分け(2026-10-29。T-1233 で確かめた)
- wt5 の T-1233(シミュの共通の HLSL の動的な添字の配列を消した。multires_activity_graph の alloca 171 → 9)を、前の形(同じプログラムに
  RefinePlaceNode)に一時的に merge すると、**HW の gpu_multires・gpu_multires_stress・gpu_multires_activity・gpu_multires_quiet・gpu_lab_box・
  gpu_tree_refine_placement が合格**した(2 回)。止まりの根は T-0397 と同じ種類(プログラム全体のローカルの配列 = alloca の重さ)と見る。
- それでも案 1 を残す: 置かれ方を使わない世界のプログラムは重いノードを持たない(main と同じ)ので、共通の HLSL に重いものが足されても
  置かれ方の段が巻き込まない(逆も)。T-1233 が main に入った後に 1 つのプログラムへ戻すかは、測って決める(戻すなら RefineNode の place を定数で選ぶ前の形)。

## 結果(数値は docs/perf.md)
- release・RTX 3070 Ti・HW と WARP: gpu_multires(全部。前からの不具合の implicit 系・near_fold を除く)・gpu_tree_refine_placement・
  gpu_placement_refine・gpu_lab_box が合格。CPU とビット一致。
- 要求 1 つを処理する段の GPU の時間: (T-1188 の測り。下の perf.md の行)

## 影響
- 作り手から見て: 変わらない(置かれ方の世界の答えは CPU リファレンスと同じ)。
- 費用: 溢れを使わない世界の `GpuMultires` は Work Graph のプログラムを 1 つ多く作る(作る時間・裏のメモリが 2 つ分。パイプラインの使い回し〔T-0427〕で 2 回目からは裏のメモリだけ)。
- 層(T-1220・T-1221)の配りは GPU の置かれ方にまだ無い(層を持つセルを置かれ方で細かくする GPU は T-1240)。
