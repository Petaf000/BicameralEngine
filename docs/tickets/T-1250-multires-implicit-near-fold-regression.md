# T-1250 main で落ちる HW の陰解法・近くの畳みの GPU 試験の切り分け

- Status: Done
- 種類: 工学(測り・切り分け)
- PC: 必須(HW の GPU。release)
- 見積もり: チャット 1〜2 回分
- マイルストーン: 土台の変更要求の束の前提(docs/plan/ROADMAP.md 今の順)
- 設計: docs/design/(多重解像度の陰解法・伝導)/ 決定: ADR-0008(決定性)

## 目的(1〜2 行)
main の HW の GPU で落ちる陰解法(gpu_multires_implicit 系)と近くの畳み(gpu_multires_near_fold)の試験が、いつ(どのコミット)から落ちるかを決めて直す。

## 落ちている形(2026-10-29 本体 12 回目・main f668e8e2・release・HW)
- gpu_multires_implicit: 「計測: 鎖を測れない」(tests/gpu_multires_implicit_test.cpp:512)
- gpu_multires_implicit_tree(--scene hot,chain): SegFault(鎖を CPU で作った直後。GPU の前)
- gpu_multires_implicit_build: SegFault(アダプタの表示の後すぐ。GPU の前)
- gpu_multires_near_fold: 4000 刻みは一致するが「頁が全部畳まれる・端数の枠を返す、になっていない」(畳んだ頁 0。tests/gpu_multires_conduction_test.cpp:742)
- gpu_multires_implicit_tree_stress は合格
- wt5 の t-1233 でも同じ形で落ちる(T-1233 の変更のせいではない)。T-1221 の前の da710a72 の上でも落ちていた。
- 最後に通ったのは wt2 の 2026-10-21 のログ(未確認: どのコミットか)。

## 完了条件(チェックできる形で)
- [x] 落ち始めたコミットを決める(wt2 の 10-21 のログのコミットから main まで二分。SegFault の 2 本は CPU 側なので WARP でも再現するかを先に見る)
- [x] gpu_multires_implicit_conduction_blocks・_blocks_chain・_levels・_levels_chain が同じ組か確かめる(wt5 が確かめ中。結果を作業ログへ)
- [x] 原因を直し、上の 4 本と同じ組の試験が HW・WARP とも合格(CPU リファレンスとビット一致)
- [x] 試験の期待を変えるなら、変えた理由(仕様の変更か不具合か)をこのチケットと ADR に書く

## メモ・参考
- SegFault は「鎖を CPU で作った直後」「GPU の前」なので、場面を作る CPU 側(tests/multires_implicit_scene.h・multires_conduction_scene.h)か、場面が依存する木の置き方・層(T-1220・T-1176・T-1188 系)の変更を先に疑う(未確認)。
- near_fold は「畳んだ頁 0」= 畳みの段の条件(層・内訳の欄の比べ)が増えて同じと見なされなくなった可能性(未確認)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29 本体 13 回目: チケットを作った(司令塔の配り)。
- 2026-10-29 作業役(wt4)1 回目: 入れ物の再起動で中断。**未着手**(ビルド・試験は 1 本も投げていない)。wt4 に枝 t-1250-implicit-fold-regression を main から切っただけ。
  - 中断の時、device_bash が runner/ の一覧で何度も止まった(ファイル数が多い所の ls -t が返らない)。次は find -newermt か名前を決めて見る。
  - 引き継ぎ(次の一手): 1) 71930728(T-0931)の前 = 71930728^ と後 = 71930728 を `git switch --detach` → release でビルド → gpu_multires_implicit_build(CPU の SegFault)を先に 1 本流して境目を確かめる。違えば 10-21 の wt2 のログ(20261021-122121-test.log)のコミットから main までを二分。
  - 2) 境目が T-0931 なら、tests/multires_implicit_scene.h・multires_conduction_scene.h が成分の幅のセルの頁(ADR-0417)に追いついているかを先に見る(試験の側の直しか、エンジンの側かをここに明記)。
  - 3) CPU 版の multires_implicit・multires_implicit_tree も同じ根で落ちるか確かめる。最後に HW と WARP で全部・`git rebase main`。
- 2026-10-29 作業役(wt4)2 回目: **境目は T-1066(21ceb6e0。D-637 で試験の伝導率の倍率を外した)**。原因と直しは ADR-0505(Proposed)。
  - 確かめ: 71930728^・71930728(T-0931)の上で gpu_multires_implicit_build_warp 合格・multires_implicit_tree 合格。T-0931〜main で陰解法・木の伝導の場面に触れたのは T-1066 だけ(d28bb4dc は途中の版で HLSL がビルドできない。21ceb6e0 が最初に通る版)。
  - 仕組み: 基準のレベル(MrSubcycleBaseLevel)が 2 → 7 になり、レベル 6 までの場面に陰解法の系ができない(CPU のログ「基準 7」・V サイクル 0)。
    SegFault 2 本は release の HW の計測が空の `ticks.back()` を読んだため(CPU 側。WARP は確かめる側の空の確認で「刻みが無い」)。gpu_multires_implicit は系が空で「鎖を測れない」。
    near_fold は現実の伝導率で 4000 刻みに鎖が均されない(畳んだ頁 0)。**alloca(T-0397)の根ではない**(wt5 の t-1233・t-1234 と関係なし)。
  - 直し(試験の側だけ・エンジンと既定の試験の表は変えない): `MakeFastConductionTestTable()`(前の倍率 気体 5000・凝縮相 20000 を相で掛ける)を
    陰解法の GPU 試験 4 本の実行ファイル・multires_implicit_tree・near_fold に。計測の側にも空の刻みの確かめ。今の式 + 前の倍率で基準 1。
  - 通した(release): CPU multires_implicit・multires_implicit_tree / HW gpu_multires_implicit・_tree・_build・_conduction_blocks_chain・_levels_chain・gpu_multires_near_fold / HW・WARP gpu_multires_implicit_warp・_tree_stress・_tree_warp・_build_warp・implicit_conduction・_conduction_chain・_conduction_warp・gpu_multires_near_fold_warp(16 本全部合格・CPU とビット一致)。release ビルド警告 0・archmap OK。
  - 司令塔へ: 現実の伝導率では陰解法(D-434 案 a)が働くのは約 2 mm より細かいセルだけ。既定の世界で陰解法の段を持つ意味の見直しの材料(ADR-0505「残る問い」)。
