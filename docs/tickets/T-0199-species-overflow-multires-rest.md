# T-0199 成分の二段: 溢れを使う多重解像度の世界(CPU)の残り(T-0187 から分けた)

- Status: Done(2026-10-11。影の引き戻し・畳む・端数を帳簿へ返す・静かな葉の許容差。覗き窓・実験室は T-0275)
- 種類: 工学
- 設計: 02 §3・17(R-MULTI-4)。関係: T-0187(世界のセルの溢れ・粗くする和集合)・T-0176(GPU)・T-0175・QUESTIONS Q19

## 目的
T-0187 で CPU の多重解像度の世界は `EnableWideCells` で「インライン 8 + 頁ごと・端数の枠ごとの溢れ」を持ち、反応・伝導の熱・細かくする・
粗くする・影を作る・要約・保存量が溢れを読む。ここでは、まだインラインの形のまま(溢れを読まない・畳まない)所を溢れに対応させる。

## やること
1. 影の引き戻し(PullBackLevel・MrPullBackShadow)を Cell 型で(今は溢れのある影で FX_ASSERT)。
2. 溢れのある頁を畳む: ビット単位で一様(溢れまで同じ)なら値 1 つ + 枠ごとの溢れ・ほぼ同じ頁(MrFoldStats を Cell 型で。tooManySpecies を無くす)・
   端数の枠を帳簿へ返す(ReturnFractionsToLedger。今は溢れのある端数の枠は返さない)。
3. 静かな葉を粗くする許容差の判定(MrCoarsenGroupWithin)と覗き窓(probe_peek)・実験室(lab_box)が溢れを読む。
4. 試験: 溢れを使う世界で影・畳む・静かな葉を粗くする場面を刻み、保存がビット一致・溢れを使わない世界と(上限に当たらない場面で)毎刻みビット一致。

## 完了条件
- 溢れを使う世界のどの操作も成分を落とさず、元素とエネルギーがビット単位で保存される

## 見積もり
1 チャット(2 時間)。畳む(2)が重ければ 1・3 と 2 で分ける。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-11(作業役。並走の作業ツリー wt5): 影の引き戻しを MrPullBackShadowOf(セルと子 2³ の形のテンプレート。C++ の MrWideShadowFamily)にし、
  溢れを使う世界は上限の無い形で引き戻す(FX_ASSERT を外した)。一様なブロックの値にも溢れ(MultiresNest::uniformOverflow・LoadWideUniform・StoreWideUniform)を足し、
  溢れのある頁も溢れまで同じなら畳む(頁の溢れは空にして返す)・一様の値は溢れごと細かくする・粗くする(CoarsenWidePageNeed)・頁に広げる・待ちの丸めで評価する。
  MrFoldStats の集計を集計の形のテンプレート(MrWideFoldStats は tooManySpecies が起きない。MrFoldStatsValueOf)にし、9 種以上のほぼ同じ頁も畳む。溢れのある端数の枠も帳簿へ返す。
  静かな葉の許容差は MrCoarsenGroupWithinOf(9 種目からの量も比べる)。GPU のシェーダーはインラインの包みで同じ結果。試験 tests/multires_wide_test.cpp(6 場面)。
  テスト(release): multires.*・reaction・reaction_wait・reaction_contention・lab_box の 13 本、gpu_multires・_activity・_quiet・_uniform・_near_fold・_conduction(HW)が通過。
  main(45bf465)へ rebase 後: release の上の CPU 13 本 + multires_wide + gpu_multires・_quiet・_uniform・_near_fold(17 本)、debug(警告なし)の multires・multires_wide・
  multires_uniform・multires_activity・multires_quiet が通過。WARP・tidy は流していない。

## 分けたもの
- T-0275: 覗き窓・実験室が溢れを読む(どちらも GPU の世界を写す・比べる作りなので、GPU の溢れ〔T-0237・T-0212〕の後)
- T-0276: 一様なブロックの評価が 9 種目の生成物を待たせる(溢れを使う世界の CPU と GPU の両方。見つけたもの。案は「待たせた印があれば頁に広げる」)。
  CPU だけ上限の無い形で評価すると gpu_multires の「大きい溢れ」が刻み 0 で食い違ったので、CPU は溢れの無い一様の値をインラインで評価するまま(GPU と一致)

## 引き継ぎメモ
- 溢れを使う世界で一様の値を書く所は StoreWideUniform(または UniformAt を書いて uniformOverflow[slot] を空に)。頁に広げる所は LoadWideNestCell を
  StoreWidePageCell で(ExpandWantedPages・ApplyCoarsenOf の一様な親)。頁を返す所は ClearPageOverflow(FoldQuietPages の ReleaseFoldedPage も)。
- HashWholeNest は使っている一様のブロックの一様の値の溢れも入れる(溢れを使わない世界の要約は前と同じ)。
- 共有のシェーダー(multires.hlsli・multires_activity.hlsli)のテンプレート化: MrPullBackShadowOf・MrUniformWaitOf<Cell>・MrFoldStatsHasRoom/WithRoom・
  MrFoldInsertSpecies/MrAddFoldCell/MrFoldStatsWithin<Stats>・MrFoldStatsValueOf・MrCoarsenGroupWithinOf。GPU は今の名前の包みを呼ぶ。

## 判断待ち
- なし(実装の細部だけ。一様の値の溢れを枠ごとの可変長で持つのは Claude が決めた)

