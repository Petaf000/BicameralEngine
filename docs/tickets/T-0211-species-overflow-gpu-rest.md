# T-0211 GPU のセルの溢れの残り: やり直し・使う分だけの VRAM・伝導・要求の処理・覗き窓(T-0176 から分けた)

- Status: Done(2026-10-10。一部: 伝導の段〔陽解法〕が溢れを読む まで。残りは T-0236〔やり直し・使う分だけの VRAM・計測〕・T-0237〔要求の処理〕・T-0238〔覗き窓・実験室・陰解法〕)
- 種類: 工学
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052。関係: T-0176・T-0187・T-0199・T-0124・D-401・D-428

## 今(T-0176 の後)
- GpuMultiresOptions::wideCells の世界は、全部を刻む Compute(RecordStep の伝導なし)だけが溢れを読み書きする(multires_step_wide_*)。
  溢れは u6 の後ろに頁ごと 2 面・1 面 MR_WIDE_PAGE_ENTRIES = 1024 成分・1 セル RX_GPU_WIDE_SPECIES = 24 種まで(当座。超えると待たせて数える)。
- 伝導(RecordStep の伝導あり。FX_ASSERT で止める)・要求の処理・活性のグラフ(RecordStepActive は false を返す)・覗き窓はまだ溢れを読まない。

## やること
1. 頁の溢れが足りない時: 印(「大きい溢れが要る」)→ 刻みの中で配り直し → そのブロックの溢れるセルだけ同じ刻みでやり直す
   (T-0176 の ③ は書く前に入るかを決めるので、入らないブロックは溢れるセルが書かれていない = 元の値が残っている)。
   使う分だけの VRAM: 溢れの塊を空きのスタックから枠の順に配る(端数の枠と同じ型)。空気だけの頁は溢れを持たない。
2. 伝導の段の熱容量・伝導率(CellThermals)が溢れを読む。伝導の差を足すセル(ApplyCell)も溢れを保つ。
3. 要求の処理: 粗くする和集合(MrCoarsenCellOf を GPU の上限なしの形で。MR_STATUS_SPECIES_FULL を使わない)・細かくする分配・影を作る/引き戻す・
   畳む(溢れのある頁は畳まない)・頁を返す/配る時に溢れを空にする。
4. 覗き窓・実験室が溢れを読む(T-0199 の CPU と合わせて)。
5. 計測: 溢れを使う世界の空気だけの刻みの ns/セル(docs/perf.md)。

## 完了条件
- TestCoarsenFullWide・TestLimitsStepWide(伝導あり)の場面が CPU と HW・WARP で毎刻みビット一致・保存がビット一致
- 頁の溢れが足りない場面でも待たせず、同じ刻みで終わる

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-10(作業役): 見積もり(1 から 5 まで全部で 4〜5 チャット)から、最初の 1 つの機能を「2. 伝導の段が溢れを読む」にした。理由: 1(やり直し・VRAM)は
  伝導の段と全部を刻む段の両方の ②③ に入るので、先に伝導の段を同じ ①②③ の形にそろえておくと 1 回で済む。3・4 は CPU の T-0199 の後の方が合わせやすい。
  ③ の共有の関数(PlanWideSide・RestepWideCell・SwapWideSide・BeginWideCells・MarkWideCell。shaders/sim/multires_wide_step.hlsli)を作り、StepPagedWaitWide も使うようにした。
  伝導の段(multires_conduct.hlsli): セルの熱は BlockCellThermal(溢れのあるセルだけ上限の無い形)・ApplyCell は溢れるセルを DeferWideCell で ③ へ回し
  RestepConductWideCells で刻み直す・ConductPrepareBlock は配った頁の溢れを空に。6 段の MR_WIDE_CELLS の変種 multires_wide_conduct_* を GpuMultires が選ぶ(ADR-0052 追記)。
  gpu_multires_test の RunLimitsStepWide に伝導ありを足した(12 刻み、待たせた 0・最大 9 種、伝導ありとなしの要約が違う = 熱が流れている)。HW・WARP で CPU と毎刻みビット一致。

## 引き継ぎメモ
- 溢れを使う GPU の世界で使えるのは「全部を刻む刻み(伝導あり〔陽解法〕・なし)」。陰解法は RecordStep の FX_ASSERT で止める(T-0238)。
  要求の処理・活性のグラフ(RecordStepActive は false)・覗き窓はまだ溢れを知らない(T-0237・T-0212・T-0238)。
- 頁の溢れ 1 面 1024 成分・1 セル 24 種を超えると、伝導の段でも「変化だけ足した値」のまま待たせて MR_COUNTER_LIMIT_PRODUCTS に数える(T-0236 でやり直しにする)。
  ②③ は共有の関数なので、T-0236 では PlanWideSide が false を返す所(StepPagedWaitWide と RestepConductWideCells の 2 か所)に「大きい溢れが要る」印を足す。
- 伝導の段の溢れの変種は 1 スレッド 1 セル(512 スレッド)で上限の無い形のセル(約 300 B)を持つので、HW ではレジスタが溢れうる(既定の世界は今のシェーダーで影響なし。
  ns/セル は T-0236 で測る)。
- 頁に広げたばかりのブロックの溢れを空にする所は 2 か所: StepExpandedWait(ClearWidePage)と ConductPrepareBlock(ClearWideSideWords)。頁を配る道を足したら同じく空にする。

## 判断待ち
- なし(実装の細部だけ。ADR-0052 追記)

## 分けたもの
- T-0236: 溢れが足りない時に同じ刻みでやり直す・使う分だけの VRAM(溢れの塊を空きのスタックから)・1 セル 24 種の上限・ns/セル と VRAM の計測
- T-0237: 要求の処理が溢れを読む(粗くする和集合・細かくする・影・畳む・頁を返す時に空に)+ TestCoarsenFullWide の GPU 版
- T-0238: 覗き窓・実験室・陰解法の段が溢れを読む
