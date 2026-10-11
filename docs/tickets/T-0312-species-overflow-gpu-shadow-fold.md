# T-0312 GPU の影・畳む・静かな葉が溢れを読む(T-0237 から分けた)

- Status: Doing(2026-10-11 一部: 一様の値の溢れの領域〔写す・読み戻す〕まで。残りは溢れまで畳む〔T-0384 の後〕。影・静かな葉は T-0383 へ分けた)
- 種類: 工学
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052(追記 5)。関係: T-0199(CPU で同じことをした)・T-0237・T-0311・T-0275

## 今(T-0237 の後)
- 影を作る(RecordRefineShadow → RefineNode の影)は、溢れを使う世界では子の頁(観察の枠の頁)の今の面を空にするだけで、親の溢れを写さない。
  影の引き戻し(PullBackNode)はインラインの形(MrPullBackShadow)のまま。
- 頁を畳む(TreeFoldCheck・TreeFold)は溢れを見ない: 溢れのある頁を畳むと溢れを落とす(GPU の一様の値はインラインだけ)。
  畳んで返す頁の溢れも空にしない。静かな葉の許容差(TreeQuietCheck の MrCoarsenGroupWithin)は 9 種目からの量を比べない。

## やること
1. 影を作る: 観察の枠の頁へ溢れを写す(塊は CPU の入力の時に配る。TreeWideReserve と同じ型)。引き戻しを上限の無い形(MrPullBackShadowOf)で。
2. 一様の値の溢れ(CPU の MultiresNest::uniformOverflow・StoreWideUniform の GPU 版)と、溢れまで同じ頁を畳む・ほぼ同じ頁(MrWideFoldStats の GPU 版)・
   畳んで返す頁の溢れを空にする・端数の溢れを帳簿へ返す。一様の値の評価・細かくする・粗くする・頁に広げるも一様の値の溢れを読む。
3. 静かな葉の許容差を MrCoarsenGroupWithinOf の上限の無い形で。
4. 試験: multires_wide_test の影・畳む・静かな葉の場面を GPU でも流し、CPU と HW・WARP が毎刻みビット一致。

## 完了条件
- 溢れを使う GPU の世界のどの要求の処理も成分を落とさず、CPU の溢れの世界と毎刻みビット一致

## 見積もり
2 チャット(影で 1、一様の値の溢れと畳むで 1)。

## T-0347 の後(2026-10-11)
- 溢れの世界の TreeFoldCheck に変種ができた(multires_tree_wide_fold_check)。端数の溢れは帳簿へ返す・返した枠の溢れは空・塊は TreeWideTrim が返す。
- 当座の仮: 変種は**溢れのある頁を畳まない**(畳むかを調べない)。ここで一様の値の溢れ(StoreWideUniform の GPU 版)と MrWideFoldStats の GPU 版を
  入れたら、この仮を外す(multires_tree.hlsl の TreeFoldCheck の `foldable`)。畳んで返す頁の溢れを空にするのも、仮を外す時に要る。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役。並走の作業ツリー wt5。約 1 時間 10 分。一部): 一様の値の溢れ(CPU の uniformOverflow)の GPU の置き場を作った。
  頁・端数の枠と同じ見出し・2 面・同じ塊の置き場の領域を世界の枠ごとに持ち(見出しは空きのスタックの後ろ・端数の枠の見出しの前)、値の溢れは
  セル 0 の範囲。領域の番号は MR_WIDE_UNIFORM_AREA | 枠(WideUniformArea。WideAreaHeader が引く)。C++ は写す(AppendOverflowImage)・
  読み戻す(ReadOverflow)・OverflowFits・ReadWideChunkUse(uniformChunks)が一様の値の領域も扱う(前は写す時に黙って落とした)。ADR-0052 追記 13。
  試験: gpu_multires_test の RunUniformOverflowRoundTrip(--part coarsen・wide。11 種の一様の値を写して読み戻すと CPU とビット一致・塊 1)が
  HW・WARP で通った。GPU の段はまだ一様の値の溢れを読み書きしない(読む段は T-0384、溢れまで畳むはこのチケットの残り、影・静かな葉は T-0383)。

## 引き継ぎメモ(溢れまで畳む〔このチケットの残り〕。T-0384 の後)
- 一様の値の領域: WideUniformArea(slot)。値の溢れは WideRange(area, 今の面, 0)。書く時は始まりの表のセル 0 = 0・セル 1〜512 = 溢れの数
  (ReadOverflow・C++ の像と同じ約束。始まりの表 513 語を持つので 1 塊)。空にするのは ClearWideSideWords(area, …)。
- 畳む(TreeFoldCheck の溢れの変種)の案: (a) ビット単位で同じ = 頁の今の面のセル(値のセル)の溢れまで比べる(FoldsExactly に溢れの範囲の比較を足す。
  覆われたセルは溢れ 0)。(b) ほぼ同じ = MrWideFoldStats の GPU 版(成分を ID の昇順に 1 つずつ集める今のループを RX_GPU_WIDE_SPECIES まで回し、
  9 種目からの集計〔present・low・high・sumLow・sumHigh〕は groupshared の配列に置く。tooManySpecies は使わない)。
  (c) 一様の値の領域の塊: TreeFold(1 グループ・枠の順の累積和)で MrWideChunksFor(値の溢れの数) を空きのスタックから配ってから、TreeFoldCheck では
  なく TreeFold の後の 512 スレッドの段で書く(TreeFoldCheck は塊が配られる前に走る)か、頁の今の面の塊の表を一様の値の領域と入れ替える
  (ビット単位で同じ時は塊が足りる。始まりの表のセル 0・1〜512 を書き直すだけ。ほぼ同じで和集合が頁の溢れより大きい時は足りない)。
  (d) 畳んで返す頁の溢れを空にする(ClearWideSideWords。CPU の ReleaseFoldedPage)。端数の溢れを帳簿へ返すのは T-0347 で済み。
  (e) 溢れの無い頁を畳んだ時は一様の値の領域を空にする(CPU の FoldPage の uniformOverflow = {})。
  これが入ったら TreeFoldCheck の `foldable = WideTotal(block.page) == 0`(仮)を外す。
- 試験の場面: multires_wide_test の TestFoldWideExact・TestFoldWideNear(畳んでから細かくする・粗くする・広げる)を GPU でも。
- **main(8cbf977)で gpu_multires_test の「端数の枠の塊」が HW・WARP とも落ちる**(T-0327 と T-0347 を合わせた後。本体の release の ctest も同じ)。
  試験の側の読み方(ReadWideChunkUse が T-0327 の MrWideSideOffset を使っていなかった)はここで直した。残り(粗くした後の空きの塊の数が組ごとに
  125 → 119)は T-0385。

## 判断待ち
- なし(実装の細部だけ。ADR-0052 追記 13)

## 分けたもの
- T-0385: main で「端数の枠の塊」の試験が落ちる(T-0327 と T-0347 を合わせた後。粗くした後の空きの塊の数が組ごとに違う)
- T-0383: 影を作る・引き戻す・静かな葉の許容差が溢れを読む(前の「やること」1・3)
- T-0384: GPU の段(評価・広げる・細かくする・粗くする・伝導)が一様の値の溢れを読む(前の「やること」2 の後半。畳む前に要る)
