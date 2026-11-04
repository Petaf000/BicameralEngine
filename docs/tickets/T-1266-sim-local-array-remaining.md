# T-1266 シミュの共通のコードの残りの局所の配列(T-1233・T-1234 の続き)

- Status: Review(2026-11-05 wt5。t-1266-multires-no-local-array を runner/ready/ に。残りの候補は下の「残り」)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: M? (docs/plan/ROADMAP.md。司令塔が置き場所を決める)
- 設計: CLAUDE.md §4・T-0397・T-1233・T-1234 / 決定: D-546

## 目的(1〜2 行)
T-1233・T-1234 の後も DXIL の alloca が残るシミュの共通のコードを、同じ形(決まった数の回し + `FX_UNROLL` で選ぶ・書く)で直し、
tools/dxil_alloca/limits.txt を下げる。結果はビット単位で同じ・費用は直す前を超えない(D-468)。

## 完了条件(チェックできる形で)
- [x] release のビルド(警告 0)・float_check・`dxil_alloca_check` を --update した表で通す(1144 → 847)
- [x] 触ったシェーダーの GPU 試験(HW 20 本と WARP 8 本)が CPU リファレンスとビット一致・記録したハッシュが変わらない
- [x] 費用(多重解像度の `gpu_multires_test --measure-wide`・物の熱の `gpu_object_heat_test --measure-only`)が直す前以下。docs/perf.md へ
- [x] `python3 tools/archmap/archmap.py --check`

## 候補(2026-10-29 の grep・T-1234 のメモから)
- multires.hlsli: MrFraction の species・amounts(MrFractionAmountOf・次の種を探す回し)・MrSplit の parent.species[p]・
  MrFold の stats の差し込み(MrFoldInsertSpecies の詰め直し)・分けた結果の result.cell.species[result.cell.speciesCount] への書き込み
- multires_tree_settle 27(T-1233 で 20 → 27): [8 x i32/i64] 17・[64 x i32/i64] 8・[16 x i32] 2。畳み・MrFraction の動的な添字と見る(未確認)
- cell_view.hlsli(CvAddSpeciesParts の回し)・light.hlsli(吸収の回し)・probe_place.hlsli(置く成分の検査)・multires_table_words.hlsli(セルを語から作る)
- object_heat.hlsli の気体の欄(flow.amounts[g] など。OH_MAX_GASES の決まった回しなら開いてそのままでよいか asm で確かめる)
- main で入った新しいシェーダー(2026-10-29 に表へ足した): multires_graph_place 60(T-1188。multires_graph と同じ数)・phase_layer_cells 6(T-1211)・cascade_light_tree_grid 2(T-1117)
- 多い所(2026-10-29 main 4d60f487 の表): multires_step_wide_wait・multires_graph_wide 82・multires_graph 60・multires_step_wide_expanded_wait・multires_step_wide_big。
  速い形 RxGpuMidCell(32 種)は開くと命令が増えすぎるので動的な添字のまま(ADR-0052 追記 14)。physics_*(83・65・62)は物理の側で別。

## メモ・参考
- 遅くなる形に注意(T-1234 で測った): 回しの中で毎回 RX_MAX_CELL_SPECIES 個・16 個から選ぶ形は、局所の配列が減っても遅くなることがある。
  版ごとにシミュのシェーダーだけ作り直して比べる道具 runner/t1234/ab.ps1(ab_revs.txt)を使い、直す前と交互に測る。

## 2026-10-30 wt5 の進め方(1 チャット目)
- 基準(main 9f620e21 の release の asm): 合計 1141。multires_tree_settle 27(8×[8 x i32/i64]・2×[16 x i32]・4×[64 x i32]・4×[64 x i64])・tree_resolve 26・
  graph 60・graph_place 60(どちらも 8×[64 x i32]・8×[64 x i64])・graph_wide 82・tree_fold_check 21・tree_quiet_check 35。
  [64 x …] は MrChildren(子 8 × 成分 8)を子の番号 j の動的な添字で読む形、[8 x …] は MrFraction・MrFoldStats の成分の欄を成分の数までの回しで読み書きする形と見る(未確認 → asm で確かめる)。
- 手 1: 粗くする核(MrCoarsenCellOf の子の回し・MrNextSpecies・MrFractionOf・MrAmountOf・MrAppendCoarsened・MrRecordLost)と影の引き戻し(MrPullBackShadowOf)。
- 手 2: 畳む集計(MrFoldStats: MrFoldSpeciesPosition・MrFoldInsertSpecies・MrAddFoldCell・MrFoldStatsWithin・MrFoldStatsValueOf)。
- 形: C++ はそのまま(MrWide* の伸びる形も同じ関数)。GPU は決まった数の回しを開くか、決まった数から選ぶ・書く(reaction.hlsli の T-1233 の形)。順も答えも同じ。
- 手ごとに dxc で asm だけ作って数を比べ(runner/t1266/asm.ps1)、効いた手だけ残す。
- **打ち切り条件**: 手を入れたシェーダーの命令(dx.op の数・行)が 1.5 倍を超える、または費用(--measure-wide の上限の種のセル・物の熱の段)が直す前を超えて戻せない手は入れない(その手は「動的な添字のまま」とチケットに書く)。
  **退く道**: 減らせた所だけ入れ、表を下げる。全部効かなければ表はそのまま・調べた結果だけ残して閉じる。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-29 wt5: T-1233・T-1234 を main へ入れた時に T-1234 の「残り」から分けた(チケットを作っただけ)。
- 2026-10-30 wt5(t-1266-multires-no-local-array。main 9f620e21 の上): 手 1(粗くする核・影の引き戻し)と手 2(畳む集計)を入れた。形は C++ はそのまま、GPU だけ
  決まった数の回し + 選ぶ/書く(multires.hlsli の MR_LIST_END・MR_SPECIES_AT・MR_AMOUNT_AT・MR_SET_ENTRY・MR_CHILD_OF・MR_CHILDREN_UNROLL)。子 2³ の回しは開く(溢れの世界 MR_WIDE_CELLS は
  子が番地だけの形で、開くと命令 1.7 倍になったので開かない)。MR_CHILD_OF は溢れの世界では配列のまま(選ぶと multires_step_wide_wait 101 → 102〜105)。
  全体の release のビルド(e8 の形・警告 0)で alloca 合計 1141 → 812: tree_settle 27 → 0・tree_resolve 26 → 2・graph / graph_place 60 → 4・graph_wide 82 → 45・
  tree_fold_check 21 → 4・tree_quiet_check 35 → 2・conduct_* 13〜16 → 6・activity_graph 9 → 3・sun_column / sky_port / implicit_build_* → 0 ほか。
  e8 では step_wide_wait 101 → 102 と増えたので MR_CHILD_OF を溢れの世界で配列に戻した(e9。dxc だけで step_wide_wait 101・graph_wide 51・tree_wide_fold_check 6)。
- 2026-11-05 wt5(2 チャット目): main 244f7dd2 へ rebase・release ビルド警告 0・alloca の表を --update(1144 → 821。消えた lab_box_patch_boundaries の行も落とした。aa94bfc0)。
  CPU の試験 15 本(multires 系 8・object_heat_nest・float_check 5・dxil_alloca_check)合格(lab_box は main でも Disabled)。
  GPU の HW 20 本中 19 本合格・WARP 8 本合格(全部 CPU とビット一致)。**gpu_multires_quiet だけ ctest の上限 300 秒で Timeout**: ドライバのシェーダーの作り直し(冷えたキャッシュ)が
  長い(multires_step_wait.cso 74.5 s・Compute のパイプラインの作成 合計 201 s・Work Graph 41 s。main の quiet は 10〜22 s)。ほかも初回は長い: gpu_multires 340 s(main 7〜9 s・冷えた時 42 s)・
  gpu_multires_uniform 373 s(main 5 s)・gpu_multires_implicit_build 325 s(main 冷えた時 46 s)・gpu_lab_box 420 s。ほかの作業ツリー(wt2・wt4・wt6)の GPU の試験と重なっていた。
  dxc だけの比べ(runner/t1266/asm2.ps1): 子 2³ の回しを開かない形(E10)でも multires_graph の dx.op は 1686(e9 1643・main 818)で、命令が増えたのは子を開いたせいではなく
  成分を決まった数から選ぶ形(MR_SPECIES_AT など)の方。E10 は alloca が graph 11・tree_settle 6・tree_resolve 8 に戻る。graph の rawBufferLoad 298 → 815・atomicBinOp 20 → 50(関数の写しが増えた)。
  費用(runner/t1266/ab.ps1: 同じ実行ファイルに main 244f7dd2・e9・E10 のシミュのシェーダー 61 本を入れ替えて main → e9 → E10 を 2 周。wt2・wt4・wt6 の GPU の試験と重なった):
  刻みの費用は 3 つとも揺れの内で同じ(上限の種のセル・溢れ 伝導なし 827〜830 / 820〜827 / 768〜820 ns/セル・大きい形 伝導なし 467〜468 / 467〜520 / 469〜474・物の熱 燃える小枝 1000 本 1.108〜1.118 / 1.115〜1.123 / 1.111〜1.112 ms。
  表は runner/t1266/ab_result_1105.tsv)。違いはドライバのシェーダーの作成だけ: E10 の初回(冷えた)は Compute 1.07 s・Work Graph 1.46 s、e9 は上の通り数十秒〜数分。
  → 子 2³ の回しを開くのをやめた(E10。MR_CHILDREN_UNROLL を空に。仮・ユーザー未確認。下の【エンジン】)。alloca 合計 1144 → 847(e9 なら 821): graph / graph_place 60 → 11・tree_settle 27 → 6・
  tree_resolve 26 → 8(ほかは e9 と同じ)。release ビルド警告 0・dxil_alloca_check を --update・archmap --check OK。E10 の GPU の試験 HW 20 本・WARP 8 本 合格(ビット一致)。

## 判断待ち
- 【エンジン】子 2³ の回し(粗くする核・影の引き戻し・畳む集計)を GPU で開くか(T-1266。2026-11-05 wt5)。作り手から見た違い:
  - 開く(e9): 多重解像度の Work Graph(multires_graph)の局所の配列が 60 → 4(T-0397 の止まりの心配がいちばん小さい)。代わりに、ドライバのシェーダーのキャッシュが冷えている時
    (初めて起動した時・ドライバを入れ替えた後・シェーダーを変えた後の試験)のパイプラインの作成が 1 本 数秒〜数十秒・試験 1 本が数分に延びる(gpu_multires_quiet が ctest の 300 秒で切れた)。
  - 開かない(E10・**仮でこちら**): Work Graph の局所の配列は 60 → 11(子の並びの [64 x i32]・[64 x i64] が 2 つずつ残る)。パイプラインの作成は main と同じ程度(1 s 前後)。
  - 刻みの費用・結果(ビット一致)はどちらも同じ。戻すのは multires.hlsli の MR_CHILDREN_UNROLL を FX_UNROLL にするだけ(溢れの世界 MR_WIDE_CELLS は開かないまま)。
  - おすすめ: 開かない(E10)。局所の配列がどれだけ残ると HW の Work Graph が止まるかの境は未確認なので、11 で止まりが出たら開く方へ戻す(1 行で戻せる)。

## 引き継ぎメモ(2026-11-05 wt5)
- ブランチ t-1266-multires-no-local-array(main 244f7dd2 の上)。E10(子 2³ の回しを開かない・仮)で release ビルド警告 0・表 847・CPU 3 本(multires・multires_quiet・dxil_alloca_check)・
  GPU の HW 20 本・WARP 8 本 合格(e9 で落ちた gpu_multires_quiet も 15.9 s で合格)。E10 の初回(この実行ファイルで冷えていた)の試験の時間: gpu_multires_uniform 17.7 s(e9 373 s)・
  gpu_multires_implicit_build 26 s(e9 325 s)・gpu_lab_box 19 s(e9 420 s)。
- 残りの候補(このチケットの続き。急がない): multires_graph / graph_place の 11(子の並び [64 x i32/i64] 2 つずつ・[8 x i64] 5)・multires_graph_wide 51・multires_step_wide_*(速い形 32 種。動的な添字のまま = ADR-0052 追記 14)・
  multires_wide_conduct_* 15〜16・lab_box_apply 21・object_heat_react 15・physics_*。子の並びを消すなら「開く」以外の形(子を局所の配列に写さず、回しの中で頁から読み直す)を asm で試す。
- 冷えたキャッシュでの作成の時間を main と同じ条件で比べる測り方は未(シェーダーに結果を変えない違いを入れて作り直させる形が要る)。
- 道具(git の外・runner/t1266/): ab.ps1(版の入れ替えの費用。dxc の行は dxc_cmds.txt = build.ninja から作り直した 316 行)・sum.py(ab のログを表に)・asm2.ps1(tag.txt の「名前 DIR_xx」で写しのシェーダーを dxc にかける)・DIR_E10(E10 の写し)。

## 引き継ぎメモ(2026-10-30 wt5・入れ物の再起動で区切った)
- ブランチ t-1266-multires-no-local-array(このメモのコミットが先頭)。直しは shaders/common/multires.hlsli・multires_tree.hlsli・shaders/sim/multires_tree.hlsl・multires_graph.hlsl だけ(読み書きの形だけ・順も答えも同じのつもり。**試験はまだ一度も流していない**)。
- e9 の全体の release ビルドをキューに入れた所で区切った(runner/logs の 20261029-204114-build。途中まで警告 0)。
- 残り(この順): (1) release ビルド警告 0・float_check を確かめる → (2) `dxil_alloca_check --update` で limits.txt を下げる(増えた所が無いか、--report を runner/t1266/base_report.txt と比べる)→
  (3) CPU の試験(multires・multires_wide・multires_quiet・multires_uniform・multires_activity・multires_conduction・lab_box・object_heat_nest。C++ の形は変えていないので確かめだけ)→
  (4) GPU の HW(gpu_multires・_stress・_limits・gpu_multires_wide_*・gpu_multires_quiet・_uniform・_activity・_near_fold・gpu_lab_box・gpu_multires_conduction)と WARP(gpu_multires_warp・_wide_warp・_quiet_warp・_uniform_warp)を 4〜8 本の組で →
  (5) 費用: `gpu_multires_test --no-gbv --measure-wide` と `gpu_object_heat_test --no-gbv --measure-only` を直す前(main)と交互に(runner/t1234/ab.ps1 の形)。子の回しを開いたので命令は増えた(graph 818 → 1643 dx.op・tree_fold_check 201 → 279)。
  打ち切り条件(上の「進め方」)に当たったら、その手だけ戻す。
- 調べる道具: runner/t1266/asm.ps1(作業ツリーの shaders を dxc で作り alloca と dx.op を数える。tag.txt が名前)。残りの候補は multires_step_wide_*(速い形 32 種。動的な添字のまま = ADR-0052 追記 14)・multires_wide_conduct_flows 14・lab_box_apply 21・object_heat_react 15(T-1234 で残した)・physics_*。

