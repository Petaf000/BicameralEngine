# T-0187 成分の二段: 多重解像度の世界(CPU)のセルに溢れを持たせる(T-0175 から分けた)

- Status: Done(2026-10-09。世界のセルの溢れ・反応・伝導の熱・細かくする・粗くする和集合・影を作る・要約・保存量。影の引き戻し・畳む・覗き窓は T-0199)
- 種類: 工学
- 設計: 02 §3・§6・17(R-MULTI-4)。関係: T-0175(核のセルの形・RxWideCell)・T-0176(GPU。この後)・T-0163・T-0022・D-401・D-418・D-428・QUESTIONS Q18・Q19

## 目的
T-0175 で反応の核はセルの形(RxCell = インライン 8 / RxWideCell = 上限なし。engine/src/sim/reaction_wide_cell.h)を選べるようになった。
ここでは CPU の多重解像度の世界(MultiresNest)のセルを「インライン K(RxCell。今の頁と同じバイト列)+ セルごとの溢れ(可変長)」にし、
世界の刻みで 9 種目以上の生成物と、8 種ずつ違う子を粗くする和集合が、待たず・断らずに進むようにする。

## やること
1. MultiresNest に溢れの領域(頁のセルと同じ添字の可変長。空なら今と同じ)と設定(溢れを使うか。既定は使わない = GPU と比べるテストは今のまま)。
   読む所・書く所(StepPagedBlock の反応・伝導の熱容量と伝導率・細かくする〔MrRefine の分配〕・粗くする〔MrCoarsenCell・MrAppendCoarsened・MrCoarsenFits〕・
   畳む〔MrFoldsCell・MrFoldStats〕・一様の判定〔MrSameCell〕・影の引き戻し・HashWholeNest)を RxWideCell で読み書きする版に。
   溢れのあるセルは、溢れまで同じでなければ一様・畳む扱いにしない(保存を守る)。
2. multires.hlsli の粗くする和集合(MrCoarsened の cell)を Cell 型で書けるようにする(核と同じ「セルの形ごとの道具」)。
3. 試験: multires_test の TestCoarsenFull・TestLimitsStep を「溢れを使う」設定でも流し、待たない・断らない・保存がビット一致。
   溢れを使わない設定では今と毎刻みビット一致(gpu_multires_* が今のまま通る)。

## 完了条件
- CPU の多重解像度の世界で、9 種目以上の生成物・8 種ずつ違う子を粗くする場面が、待たせる・断ることなく進み、元素とエネルギーがビット単位で保存される
- 溢れを使わない設定・上限に当たらない場面(試験の表・燃える木箱)は今と毎刻みビット一致

## 見積もり
1 チャット(2 時間)。読む所が多いので、溢れれば 1(反応と粗くする)と 1 の残り(畳む・伝導・影)で分ける。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役): 最初に t-0139・t-0178・t-0184 を合わせた main を release で 2 束(46 本・41 本)通過(直したものなし)。
  MultiresNest に溢れ(EnableWideCells・頁ごと / 端数の枠ごとの MultiresOverflowArea。セルの番号の順に詰める)と読み書き(LoadWideNestCell・StoreWidePageCell・LoadWideFraction・StoreWideFraction。
  engine/src/sim/multires_wide_cell.*)。粗くする和集合を MrCoarsenCellOf(子と結果の形のテンプレート)にし、上限なし版 MrCoarsenWideCell。StepPagedBlock・CellThermals・
  RefineRequestLevel・ApplyCoarsen・RefineShadowLevel・HashRealLeaves・HashWholeNest・ComputeConservedTotals が溢れを読む。溢れのある頁・端数の枠は畳まない。
  multires_test に TestCoarsenFullWide(断った 0・親のセル 16 種・刻んで 17 種)・TestLimitsStepWide(待たせた 0・最大 9 種)・TestWideMatchesInline(本物の鎖が毎刻みビット一致)。保存は毎刻みビット単位。
  2 時間の約束のため、影の引き戻し・畳む・静かな葉の許容差の判定・覗き窓・実験室の溢れは T-0199 に分けた。
  変更後: release の multires.*・reaction.* など 16 本、debug の multires・reaction、release の gpu_multires(_conduction)(_implicit(_tree))(_warp) 8 本が通過(溢れを使わない世界は GPU と今のまま一致)。

## 分けたもの
- T-0199: 溢れを使う多重解像度の世界(CPU)の残り(影の引き戻し・溢れのある頁を畳む・端数を帳簿へ返す・静かな葉の許容差・覗き窓・実験室)
