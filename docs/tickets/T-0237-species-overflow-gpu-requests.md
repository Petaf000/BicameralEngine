# T-0237 GPU の要求の処理が溢れを読む: 粗くする和集合・細かくする・影・畳む・頁を返す(T-0211 から分けた)

- Status: Todo(T-0236 の後。T-0199 の CPU と合わせる)
- 種類: 工学
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052。関係: T-0176・T-0187・T-0199・T-0211・T-0236

## 今(T-0211 の後)
- 溢れを使う GPU の世界(GpuMultiresOptions::wideCells)で溢れを読み書きするのは、全部を刻む Compute(伝導なし・伝導あり〔陽解法〕)だけ。
  要求の処理(multires_tree.hlsl の TreeResolve・Settle・Allocate など)は溢れを知らない(使うと溢れが食い違う)。

## やること
1. 粗くする和集合: MrCoarsenCellOf を GPU の上限なしの形(RxGpuWideCell)で。MR_STATUS_SPECIES_FULL を使わない。結果を親の頁の溢れへ
   (1 スレッドで 64 セルを和集合するので重い。要求 1 件ごとの費用を測る)。
2. 細かくする分配(MrRefine): 親の溢れを子の頁へ。覆った親のセルは溢れも空に(CPU の multires_tree.cpp の「親を覆う」)。
3. 影を作る・引き戻す(MrPullBackShadow の上限なしの形。CPU は T-0199)。
4. 畳む: 溢れのある頁は畳まない(CPU と同じ)。頁を返す・配る時に溢れを空にする(T-0236 の塊なら塊を返す)。
5. 試験: gpu_multires_test に TestCoarsenFullWide の場面(粗くしてから伝導ありで刻む)を足し、CPU と HW・WARP が毎刻みビット一致・保存がビット一致。

## 完了条件
- TestCoarsenFullWide の場面が CPU と HW・WARP で毎刻みビット一致(断った 0・親のセル 16 種)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
