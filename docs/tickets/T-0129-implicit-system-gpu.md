# T-0129 陰解法の系を GPU で作る — 未知数・境のセル・面・セルの面の一覧(この版)

- Status: Done(範囲を絞った: 未知数・境のセル・面・セルの面の一覧を GPU の木から作り、CPU の系と毎刻みビット一致・それを解いた結果も一致・費用を測る。
  多重格子の段・重み・節の並び・間接の Dispatch は **T-0134**)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 2 時間以内(始める前の見積もり: 系の全部〔面の一覧 + 多重格子の段の形・128bit の割り算の重み・段の数が値で決まる・節の並び〕は
  2 時間に入らない → 時間の約束の目安どおり「未知数・境のセル・面の一覧」に絞り、多重格子の側を T-0134 に分けた。実際は約 2.1 時間。debug のテストの待ちが長かった)
- マイルストーン: M2 の並走(D-432)
- 設計: docs/design/17-multiresolution.md §6 / 決定: D-428・D-432・D-434・D-436 / ADR-0019

## 目的(1〜2 行)
T-0127 で GPU が解けるようになった木の陰解法の系を、CPU に頼らず GPU の木から作る道の 1 段目: 系のうち木を読む部分(未知数・境のセル・面とその係数・
セルの面の一覧)を GPU で作り、CPU の MakeSystem と番号まで同じにする。

## 完了条件(チェックできる形で)
- [x] GPU の木(GpuMultires のバッファ)から、未知数(基準より細かい Δk 1〜implicitMaxGap の本物のブロックの刻むセル)・境のセル・面(係数・レベルの差・
      粗い側の端数の枠)・セルの面の一覧を作る(engine/src/sim/gpu_implicit_build.*・shaders/sim/implicit_build.hlsl)
- [x] 熱い点(6 刻み)・鎖(12 刻み)・たくさんの要求(4 刻み)の場面で、作った系(セル・面・面の一覧と数)が CPU の系と毎刻みビット一致。
      それを GpuImplicit に写して解いた結果(セルのエネルギー・端数・V の回数・安全網)も CPU の StepImplicit と一致
      (CPU から写すセルのエネルギーと刻みの初めの温度は 0 にして、GPU が作った値で解いたことを確かめる。tests/gpu_multires_implicit_build_test.cpp。
      HW・WARP、debug・release)
- [x] 組み立ての費用を測る(下の「結果」・docs/perf.md)
- [ ] 多重格子の段(親への縮約・段の数は値で決まる)・重み(128bit の割り算)・節の並び(長い行・ImTail の段)・間接の Dispatch の引数を GPU で作る → **T-0134**

## 何をしたか(形。決めたのは Claude〔実装の細部〕)
- **番号の付け方を CPU と同じにする**(面の順は安全網の頭打ちの足し算で効くので、比べるのは番号まで): 未知数は (枠, セル) の昇順(枠ごとの数の接頭和
  + 枠の中の順)。面は「未知数 u × 6 + 面」の候補ごとの有無の接頭和(CPU が u・面の順に足すのと同じ)。境のセルは CPU が初めて出会う順 =
  そのセルを指す最初の候補(atomic の最小)だけを数える接頭和 + 未知数の数。セルの面の一覧は atomic で仮に置き、値ごとに「同じセルの一覧で自分より
  小さい値の数」を数えて置き直す(面の番号の昇順。置く順によらない)。
- 段は 16 個の Compute(GpuMultires のルート署名に外のバッファ u4 = 作業場・u5 = 系を結ぶ。GpuMultires::RecordExternalDispatch)。
  Dispatch の数は上限(GpuImplicitBuildLimits)から決め、超えたグループは何もしない(間接の引数は T-0134)。
- 作った系の並びは GpuImplicit のセル・面・面の一覧のバッファの先頭と同じ。GpuImplicit::RecordCopySystem(新)で写す(段の形は Create の grid のまま)。
- 凍った枠(頁が足りない)は、伝導の段の刻みの印(CONDUCT_MARK_FROZEN)を見る形を用意した(RecordBuild の useFrozenMarks)。試験の場面には
  凍った枠が無いので、試験は見ない形で比べる(凍った枠がある刻みは試験が止める)。伝導の段から呼ぶ時(T-0132)に確かめる。
- MultiresNest::captureImplicitNest(既定 false。試験用): 陰解法の系を作る時の木(陽解法の流れの後・変化を足す前)と凍った枠を写す。

## 結果(2026-10-06、release、RTX 3070 Ti。系を作る段だけ。木は CPU の写しを GpuMultires に写したもの。暖機 20 回の後 5 回の最小)
| 場面(系のセル・面・世界の枠) | 系を作る段(2 回測った) | 重い段(ms) |
|---|---|---|
| 熱い点(512・1344・1) | 0.24〜0.38 ms | NumberUnknowns 0.045・CountBlocks 0.039(1 グループ) |
| 鎖(1907・6192・16) | 0.34〜0.56 ms | FaceEntries 0.078・NumberUnknowns 0.048・CountBlocks 0.047・Faces 0.031・SortLists 0.027 |
| たくさんの要求(20996・68688・192) | 段ごとの和 0.22 ms(1 回通しは 0.22〜1.42 ms とぶれた) | Faces 0.066・CountBlocks 0.027・NumberUnknowns 0.025 |
- 小さい場面ほど重いのは 16 段の直列の遅延(1 グループの段でも 1 段 15〜50 µs。GPU の時計が上がらない時に大きい)。T-0127 の解く段(鎖 1.48〜2.91 ms)に
  対して約 2〜4 割を足す。形が変わらない刻みは作り直さない形(T-0134)と、伝導の段の後ろに続けて積む形(T-0132。時計が上がった状態)で減る見込み(未確認)。
- 直す前(1 グループの塊の接頭和・1 スレッドの挿入の並べ直し)は鎖 0.94 ms(SortLists 0.82)・たくさんの要求 1.4 ms(ScanEntries 0.48・SortLists 0.82)。
- 注意: 境のセルの一覧が長いと SortLists は O(n²)(1 値ごとに一覧を数える)。Δk 8 の粗い面では 1 面に 4^8 の細かい面が来うる(試験の場面では最大 208)。

## 判断待ち
- なし(番号の付け方・段の分け方は実装の細部。遊びへの影響なし)。

## 分けたもの(新しいチケット。ROADMAP には司令塔が足す)
- **T-0134**(工学): 陰解法の系の多重格子の段・重み・節の並びを GPU で作る。親への縮約(CPU の BuildImplicitGrid と同じ組み方・同じ番号)・
  行の係数の和と重み(128bit の割り算)・段の数は「自分の重み < 1/2 の節があるか」で GPU が決める・赤黒の色・節の並び(長い行の節を後ろに・
  ImTail の段)・間接の Dispatch の引数(この版は上限から投げている)・GpuImplicit の大きさを上限から(今は CPU の系の数で Create)。
  形が変わらない刻みは作り直さない(重みだけ毎刻み)形を測って決める。T-0129 の系(セル・面・面の一覧)の後ろにつなぐ。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-06(作業役、wt2・約 2.1 時間): 始める前の見積もりで系の全部は 2 時間に入らないと判断し、未知数・境のセル・面・セルの面の一覧に絞った
  (多重格子の側は T-0134)。CPU の番号の付け方(初めて出会う順の境のセル)を接頭和だけで再現する形にして、最初のビルドで HW・WARP ともビット一致。
  計測で 1 グループの接頭和(12 万候補で 0.48 ms)と 1 スレッドの並べ直し(隣 208 の境のセルで 0.82 ms)が重かったので、3 段の接頭和と値ごとの順位に変えた
  (鎖 0.94 → 0.34〜0.56 ms・たくさんの要求 1.4 → 0.22 ms〔段ごとの和〕)。debug・release の HW・WARP で通過、tidy の指摘 2 件を直した。

## 引き継ぎメモ(HANDOFF に載せるもの。司令塔がマージ後に反映)
- 状態: T-0129 完了(範囲を絞った)。陰解法の系のうち未知数・境のセル・面・セルの面の一覧を GPU の木から作り(GpuImplicitBuild・implicit_build.hlsl)、
  CPU の系と番号まで毎刻みビット一致・それを GpuImplicit で解いた結果も一致(熱い点・鎖・たくさんの要求)。多重格子の段・重み・節の並び・間接の Dispatch は T-0134。
- 動いているもの: `-Filter "^gpu_multires_implicit_build(_warp)?$"`(release の HW 約 80 秒〔計測込み〕・WARP 約 35 秒 / debug の HW 約 6.5 分・WARP 約 1.5 分)。
  計測は `job.py run -Preset release -Exe gpu_multires_implicit_build_test -- --queue compute --measure-only`。multires_implicit(_tree)・
  gpu_multires_implicit_warp・gpu_multires_implicit_tree_warp も通る。
- 壊れているもの: なし。
- 決めたこと(Claude・実装の細部): ADR-0019 追記(T-0129: 番号は CPU と同じ・境のセルは最初の候補の atomic の最小・接頭和は 3 段・一覧は値ごとの順位で置き直す)。
- 判断待ち: なし。
- 注意: 系を作る段は 0.22〜0.56 ms(16 Dispatch の直列の遅延が主。docs/perf.md)。凍った枠(頁の不足)を見る形(useFrozenMarks)は試験の場面に無いので未確認(T-0132 で)。
  GpuImplicit はまだ CPU の系の数で Create する(上限からにするのは T-0134)。MultiresNest::captureImplicitNest(既定 false)は試験用。
- 注意: tidy(release)はこのチケットのファイルの指摘を直した(gpu_multires.cpp:248 の CreatePipelines の readability-function-size は main から。触っていない)。
