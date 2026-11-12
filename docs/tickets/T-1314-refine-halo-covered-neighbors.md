# T-1314 木を細かくする段の隣の写し: 子に覆われた隣のセルを読む(T-1257 の不具合候補)

- Status: Done(2026-11-12 本体が main へ。wt5・t-1314-halo-covered: release ビルド警告 0・CPU 6 本と GPU HW / WARP 4 本合格。前線の順 = 一度に がビット単位で同じ。runner/ready/ に印)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列・土台の変更要求の束(docs/plan/ROADMAP.md。司令塔が載せる)
- 設計: docs/design/20-transport-substrate.md §10.5 / 決定: D-664 / 前: T-1176・T-1189・T-1257

## 目的(1〜2 行)
前線が通って隣のブロックが先の刻みに細かくなった後でも、細かくする段が隣の列を読めるようにする(D-664 の「前線が通って細かくなった所で砂山が勝手に動かない」)。
今の隣の写し(common/multires_refine_halo.hlsli)は「子に覆われたセル」を無い(MR_HALO_ABSENT = 横は傾き 0・下は詰まり切る)と読む(T-1176 の 1 本目の仮)。

## 見つけたこと(T-1257 の測り。`case_b_r3_test 15`)
- 0.25 m の 25° の斜面: 根ごとに 1 刻み 1 つずつ細かくする(前線の順)と、空気 0.16 / 0.85 / 0.69 m/s(0.25・0.5・4 秒)・粒 2.2 m/s。
  一度に(隣がまだ粗い)なら 1 mm/s・粒 0。違うのは根の境の列(39 セル。差の最大 150 ‰)。
- ブロックの中(同じ親)の覆われたセルは置かれ方が読む(読まない形の一度に ⇔ 前線の順がほぼ同じ)ので、食い違いはブロックの外の写しだけ。
- 要求の確定は親のブロック 1 つに 1 刻み 1 つ(multires_tree.cpp の claims)なので、同じ根の八分の一は必ず別の刻みになり、前線では隣が先に細かくなるのが普通。

## 完了条件(チェックできる形で)
- [x] 隣の写しが、子に覆われたセルを子から読む(案は下)。CPU(SnapshotRefineHalos)と GPU(TreeRefineHalo)で同じ・ビット一致(gpu_tree_refine_placement に前線の順の場面 5 つ。HW・WARP)
- [x] `case_b_r3_test 15` の「隣を読む・前線の順」が 1 mm/s・粒 0 になる(表を T-1257 の形で。下の「結果」)
- [x] 保存・2 回の実行でハッシュ一致・tree_refine_placement の端の場面が今と同じ(覆われたセルの無い場面)

## 案(作り手から見て違いは無い。どれも D-664 の約束を守るための作りの違い)
- (a) **覆われたセルは子 8 つの詰まりの平均(粗くする規則 = 和 ÷ 8)を読む**(おすすめ。子が動いた後も今の値。費用は細かくする出来事の時だけ・写しの段で子のブロックを 1 段引く)。
- (b) 覆われた親のセルに残る値を読む(ブロックの中と同じ読み方。安いが、子が流れた後は古い値になりうる)。
- 下の隣が粗いブロックだけの時(T-1189 の仮: 詰まり切る)も同じ口で読むかは T-1189 の判断のまま(この表の場面では効かない)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-05(wt5・T-1257 の作業役): T-1257 の測りから分けた。

## 作り(2026-11-05 wt5・司令塔の一旦の決定 = 覆われたセルは子の値から読む。案 (a))
- **覆われたセルの詰まり = 覆う子のブロックの子のセル 2³ の量を成分ごとに足し(層の分も成分ごと)、子 8 つの器(8 × V0)で読む**
  (CPU `CoveredFullness`・`CoveredSettleSum`〔engine/src/sim/multires_refine_cells.cpp〕/ GPU 同名〔shaders/sim/multires_refine_halo.hlsl〕)。
  分け方は MaterialsOf と同じ(層の分は沈む側・層を使う世界の粒の残りは舞う粒)。体積は成分ごとの和から求める(子ごとの体積を足すと切り捨てがずれる)ので、
  細かくしたばかりの子(和 = 親 × 8 ちょうど)なら細かくする前の親を読んだのとビット単位で同じ = 前線の順 ⇔ 一度にが同じになるはず。
  和か平均か: 量は和(保存の形)・詰まりは子 8 つの器で割る = 子の平均。粗くする規則の和 ÷ 8 の切り捨ては通らない。
- 子のセル 2³ は子のブロックの同じ八分の一にあるので、孫に覆われていれば全部 → 無い(孫までは降りない。2:1 の木では接する所に来ない。仮)。
- **ブロックの中の覆われたセルも同じ**(前の作業ログの「中は読む」は誤り: Present は中の覆われたセルも無いと読んでいた)。写しを 3 面 → 6 面
  (MR_HALO_CELLS 108 → 216。面 3〜5 = P の中を向く面〔ビット 0 → 4・1 → 3〕。common/multires_refine_halo.hlsli の MrHaloIndexOf・MrHaloInWindow)。
  CPU の Present / FullnessAt は覆われたセルを写しから、GPU は PlaceRefineChildren の 1 段目で覆われたセルを PlaceCoveredFullness(写しの中の面)から。
  写しの段は 256 スレッド。置き場は世界の枠 × 216 語(枠 1 万で 8.6 MB。前は 4.3 MB)。写しが無い(MR_HALO_NONE)所は前と同じ。
- 試験の器: tests/tree_refine_placement_scene.h に EdgeRequestInside・MakeEdgeFrontWorld を足した(試験本体はまだ)。
- 作り手から見た違いは無い(D-664 の約束を守る直し)。【エンジン】の問いは無し。

## wt2 との重なり(2026-11-05 読んで確かめた)
- wt2(T-1305)は multires_refine_place.hlsli の 54〜145 行(物質の並びを PlaceUseCell で 1 回作る)と PlaceChildEnergy を書き換え中。
  この枝の変更は同じファイルの PlacePresent の前(PlaceCoveredFullness)と PlaceRefineChildren の 1 段目・頭の注釈 → 行は重ならないが同じファイル。
  multires_refine_halo.hlsl・.hlsli・multires_refine_cells.cpp は wt2 は触っていない。写しの段は PlaceUseCell → PlaceLayerOf(n) の口だけ使う(wt2 の形でも同じ口)。
  gpu_tree_refine_placement_test.cpp は wt2 も触る(場面を足す時に衝突しやすい)。

## 引き継ぎ(2026-11-05 wt5。巡目の止めで中断)
- 次の一手: ① release のビルド(未実行)② tree_refine_placement_test に TestFront(B → A と A0 → A1 の A の子 = A だけの子 BlockCellsHash(…,1,8)・保存・2 回で同じ)
  ③ case_b_r3_test の TreeSlope に EXPECT(neighbors.grain == neighborsFront.grain)(読まない・前線の順も一度にと同じになるはず)・表を測り直して T-1257 の形で
  ④ gpu_tree_refine_placement に前線の順の場面(初め = MakeEdgeWorld(first)・要求 = second・期待 = MakeEdgeFrontWorld。層・舞う粒も)
  ⑤ CPU 先: case_b_r3_tree_slope・tree_refine_placement・placement_refine・multires・multires_layers・dxil_alloca_check → GPU 最後に 1 回(HW・WARP)・archmap --check
  ⑥ 変わる試験を数える(覆われた隣のある場面だけ変わるはず)・設計 20 §10.5 の「子に覆われたセルは無いと読む」を直す・rebase main → 印。
- 気になる所: GPU の CoveredSpeciesSeen / CoveredSpeciesSum は成分ごとに子 8 つを読み直す(最悪 8 × 8 × 8 × 8 読み。写しの段だけ・細かくする出来事の時だけ)。費用は測っていない。

## 結果(2026-11-12 wt5。release・RTX 3070 Ti)
- `case_b_r3_test 15`(4 秒の空気の速さ 0.25 / 0.5 / 4 秒・粒の最大): 隣を読む・一度に 1 / 1 / 1 mm/s・粒 0 / **隣を読む・前線の順 1 / 1 / 1 mm/s・粒 0**(T-1314 の前は 160 / 850 / 690 mm/s・粒 2.2 m/s)/
  読まない・一度に = 読まない・前線の順 321 / 898 / 3487 mm/s・粒 6192 mm/s。子の粒(z = 0 の断面)は前線の順と一度にで 0 セルの差(EXPECT にした)。読まない形も前線の順 = 一度に(ブロックの中の覆われたセルも子から読むため)。
- tree_refine_placement の TestFrontOrder(隣のブロック B・中の八分の一 0・両方を先の刻みに → A の子は A だけの刻みと同じ。層なし・層・舞う粒)・前の端の場面は前と同じ答え。
- gpu_tree_refine_placement(HW・WARP)に前線の順の場面 5 つ: 全部 CPU とビット一致。「B が先」の要約は前からある「同じ刻みに (B, A)」と同じ(最後の状態が同じ)。
  要求を処理する段(8 回の最小・HW): B が先 0.76 ms・中が先 0.26・両方 0.29・層 1.32・層 + 舞う粒 1.36(同じ回の端・同じ刻み (A, B) 0.165・層 1.37)。細かくする出来事の時だけの費用。
- 変わった試験: 無し(記録したハッシュの取り直しは要らなかった。覆われた隣のある場面は前線の順の場面だけ)。

## 作業ログ(続き)
- 2026-11-12(wt5): ビルド(警告 0)・前線の順の試験(CPU の器・case_b_r3 の EXPECT・GPU の 5 場面)・CPU 6 本(case_b_r3_tree_slope・tree_refine_placement・placement_refine・multires・multires_layers・dxil_alloca_check)・
  GPU 4 本(gpu_tree_refine_placement・_warp・gpu_placement_refine・_warp)合格・設計 20 §10.5 を直した・archmap --check OK。気になる所(覆われたセルを読む費用の最悪)は上の測りで細かくする段 0.26〜0.76 ms。
