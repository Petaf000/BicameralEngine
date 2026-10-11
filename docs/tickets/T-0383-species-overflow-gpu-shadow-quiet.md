# T-0383 GPU の影を作る・引き戻すが溢れを読む・静かな葉の許容差が溢れを読む(T-0312 から分けた)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052(追記 6)。関係: T-0199(CPU で同じことをした)・T-0237・T-0312

## 目的
溢れを使う GPU の世界で、観察の枠(影)と静かな葉の判定が 9 種目からの成分を落とさない(CPU の溢れの世界と毎刻みビット一致)。

## やること(T-0312 の「やること」1・3 をそのまま移した)
1. 影を作る(RecordRefineShadow → RefineNode の影): 観察の枠の頁へ親の溢れを写す(塊は CPU の入力の時に配る。TreeWideReserve と同じ型)。
   今は子の頁(観察の枠の頁)の今の面を空にするだけ。引き戻し(PullBackNode)を上限の無い形(MrPullBackShadowOf)で(今はインラインの MrPullBackShadow)。
   上限の無い形のセルを 512 スレッドの段・Work Graph のノードに持たせない(HW で止まる型。読む位置だけ持つ子の形・作業場で)。
2. 静かな葉の許容差(TreeQuietCheck の MrCoarsenGroupWithin)を MrCoarsenGroupWithinOf の上限の無い形で(9 種目からの量の幅も比べる)。
   一様な葉はいつも粗くできる(CPU と同じ)。
3. 試験: multires_wide_test の影(TestShadowWideOverflow)・静かな葉(TestQuietWide)の場面を GPU でも流し、CPU と HW・WARP が毎刻みビット一致。

## 完了条件
- [ ] 影・静かな葉の場面が CPU の溢れの世界と HW・WARP で毎刻みビット一致・保存がビット一致
- [ ] 既定の世界のシェーダー・VRAM・結果は変わらない

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
