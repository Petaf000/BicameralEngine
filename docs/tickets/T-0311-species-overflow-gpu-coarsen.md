# T-0311 GPU の粗くする和集合と端数の溢れ(T-0237 から分けた)

- Status: Todo(T-0237 の後)
- 種類: 工学
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052(追記 5)。関係: T-0187・T-0199・T-0236・T-0237・T-0312

## 今(T-0237 の後)
- 溢れを使う GPU の世界(GpuMultiresOptions::wideCells)の要求の処理は、細かくする(子の頁へ溢れを写す・親を覆う)と頁を返す・配る時に空にするまで。
- 粗くする要求(multires_graph.hlsl の CoarsenRequest)はインラインの核(MrCoarsenCell)のまま: 子に溢れがあると溢れを落とす(保存が崩れる)。
  和集合が 8 種を超えると TreeResolve が MR_STATUS_SPECIES_FULL で断る(CPU の溢れの世界は断らない)。
- GPU は端数の枠の溢れを持たない(GpuMultires::OverflowFits が、CPU の端数に溢れがあると写すのを断る)。上限の場面の豊かな子を粗くすると、
  端数は 16 種まで出る(子の量が乱数で 8 で割り切れない)。

## やること
1. 端数の溢れの置き場: 端数の枠ごとの見出し(頁の見出しの後ろ)と同じ塊の置き場(T-0236)。面は 1 つで足りるか(端数は刻まない)を決める。
   C++ の像(AppendOverflowImage・ReadOverflow・OverflowFits)も端数の溢れを写す・読む。
2. 粗くする和集合: GPU の上限の無い形の端数・子・結果(MrGpuWideFraction など。RX_GPU_WIDE_SPECIES まで)で MrCoarsenCellOf を呼ぶ
   (1 スレッドが 8 セルの和集合。重いので要求 1 件の費用を測る)。TreeResolve は溢れの世界では MR_STATUS_SPECIES_FULL を使わない。
3. 塊: 親の頁の書く面(八分の一の 64 セルが変わる。ほかのセルの溢れは写す)と親の端数の枠の溢れの塊を、TreeWideReserve で要求の順に配る。
   要る数は粗くするまで分からないので、上限(セルごとに min(24, 子の成分 + 端数の成分 − 8))で取って余りを返すか、数える段を先に回すかを測って選ぶ。
4. 試験: gpu_multires_test に TestCoarsenFullWide の場面(粗くしてから伝導ありで刻む)を足し、CPU と HW・WARP が毎刻みビット一致・保存がビット一致
   (断った 0・親のセル 16 種)。

## 完了条件
- TestCoarsenFullWide の場面が CPU と HW・WARP で毎刻みビット一致

## 見積もり
1〜2 チャット(端数の溢れの置き場で 1、和集合と試験で 1)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
