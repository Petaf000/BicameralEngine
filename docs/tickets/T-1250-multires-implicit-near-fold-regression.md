# T-1250 main で落ちる HW の陰解法・近くの畳みの GPU 試験の切り分け

- Status: Todo
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
- [ ] 落ち始めたコミットを決める(wt2 の 10-21 のログのコミットから main まで二分。SegFault の 2 本は CPU 側なので WARP でも再現するかを先に見る)
- [ ] gpu_multires_implicit_conduction_blocks・_blocks_chain・_levels・_levels_chain が同じ組か確かめる(wt5 が確かめ中。結果を作業ログへ)
- [ ] 原因を直し、上の 4 本と同じ組の試験が HW・WARP とも合格(CPU リファレンスとビット一致)
- [ ] 試験の期待を変えるなら、変えた理由(仕様の変更か不具合か)をこのチケットと ADR に書く

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
