# T-0312 GPU の影・畳む・静かな葉が溢れを読む(T-0237 から分けた)

- Status: Todo(T-0311 の後。畳むは一様の値の溢れが要る)
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

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
