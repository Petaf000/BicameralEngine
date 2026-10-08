# T-0134 陰解法の多重格子の段・128bit の重み・値で決まる段の数を GPU で作る(この版)

- Status: Done(範囲を絞った: 多重格子の段〔親への縮約・CPU と同じ番号〕・128bit の割り算の重み・値で決まる段の数を GPU で作り、
  CPU の BuildImplicitGrid と毎刻みビット一致・それを写して解いた結果も一致・費用を測る。節の並び・ImTail・間接の Dispatch・
  GpuImplicit の大きさを上限から決めるのは **T-0135**)
- 種類: 工学
- PC: 必須
- 見積もり: 作業役 2 時間以内(始める前の見積もり: 全部〔段 + 重み + 段の数 + 節の並び + ImTail + 間接の Dispatch + 上限から Create〕は
  2 時間に入らない → 「段と 128bit の重み・値で決まる段の数」に絞り、残りを T-0135 に分けた。実際は約 2 時間〔前の作業役の書きかけを含む〕)
- マイルストーン: M2 の並走(D-432)
- 設計: docs/design/17-multiresolution.md §6 / 決定: D-428・D-432・D-434・D-436 / ADR-0019

## 目的(1〜2 行)
T-0129 で GPU が作れるようになった陰解法の系(セル・面・セルの面の一覧)から、多重格子の段(節・隣・重み・親子)も GPU で作る。
CPU の BuildImplicitGrid と番号まで同じにして、GpuImplicit にそのまま写して解ける形にする。

## 完了条件(チェックできる形で)
- [x] 段 0(セル)と、最も細かいレベルの節を親のセルへ縮約した段を GPU で作る(engine/src/sim/gpu_implicit_levels.*・shaders/sim/implicit_levels.hlsl)。
      節(自分の重み・縮約の重み・隣の範囲・親・色・子の範囲)・隣(重み・先)・子の一覧が GpuImplicit の並び(gpu_implicit.cpp の MakeNodes)と同じ
- [x] 重みは 128bit の割り算(ImWeight。CPU と同じ関数。前の作業役が ImWideRatio などを implicit_conduction.hlsli へ移した)
- [x] 段の数は値で決まる(重み < 1/2 の節がある・上限 64 より少ない・縮約で節が減った時だけ次の段。回は上限まで積み、要らない回は述語で飛ばす)
- [x] 熱い点(6 刻み)・鎖(12 刻み)・たくさんの要求(4 刻み)で、作った段が CPU と毎刻みビット一致。GpuImplicit の段の重みを 0 にして
      GPU が作った節・隣・子の一覧で上書きして解いた結果(エネルギー・端数・V の回数・安全網)も CPU の StepImplicit と一致
      (tests/gpu_multires_implicit_build_test.cpp。HW・WARP、debug・release)
- [x] 組み立ての費用を測る(下の「結果」・docs/perf.md)
- [ ] 節の並び(長い行・色ごとの並び)・ImTail の境・間接の Dispatch の引数・GpuImplicit の大きさを上限から → **T-0135**

## 何をしたか(形。決めたのは Claude〔実装の細部〕)
- **親の番号 = CPU が初めて出会う順**: 細かい段の節ごとに親の鍵(最も細かいレベルなら (レベル − 1, 座標 ÷ 2)、ほかは自分)を開番地法の表に
  「その鍵の最小の節の番号 + 1」で入れ(CAS で場所を取り、同じ鍵なら atomic の最小)、自分が最小なら 1 の接頭和で番号を付ける。
- **親の行 = (子の番号, 隣の番号) の順に初めて出会う順**: 細かい段の隣ごとに (親, 隣の親) の組を同じ形の表に入れ、組の最小の隣だけが
  親の行に仮に置かれ、行の中で自分より小さい番号の数で置き直す(T-0129 の SortLists と同じ形)。細かい隣は行の数の接頭和で 1 本に並ぶので、
  隣の通し番号の順 = CPU の (子, 隣) の順。
- **行の係数は 128bit の繰り上げつきの atomic の足し算**(語ごとに足し、足す前の値から自分の桁あふれを知って上の語へ。和は順によらない)。
  縮約した親の行は ÷ 8、端の片方が縮約した親なら ÷ 2(galerkin = false だけ)。÷ 2 の後の係数を書き戻して次の回が縮約する。
- **熱容量は子の一覧の順に足す**(親の子は 8 個まで。子の一覧は GpuImplicit の並びのまま使う)。
- **段の数**: 回(段 d → d + 1)は上限 − 1 回積む。回の最後の RoundEnd(述語の外)が次の回の述語を書く。節が減らなかった段は置かない(CPU と同じ)。
- 入力のセルの座標は T-0129 の BuildCells が作業場の後ろに残す(CellKeyWord。8 語 / セル)。
- 自分のルート署名(UAV 7・定数 12)。GpuImplicitBuild の系と作業場を u0・u1 に結ぶ(GpuMultires のルート署名は 63 / 64 語で足せないため)。
- GpuImplicit::RecordCopyLevels(新): 節・隣・子の一覧(面の一覧の後ろ)を写す。節の並び・ImTail は Create の grid のまま(T-0135)。
- 試験用: MakeGpuImplicitLevelImages(CPU の系を GpuImplicit の並びに。gpu_implicit.cpp の MakeNodes を使う)。

## 結果(2026-10-09、release、RTX 3070 Ti。多重格子の段を作る段だけ。暖機 20 回の後 5 回の最小)
| 場面(セル・節・段の数) | 多重格子の段(上限 64 = 948 Dispatch) | 上限 = 段の数 | 系を作る段(T-0129) |
|---|---|---|---|
| 熱い点(512・585・4 段) | 2.38 ms | 0.47 ms(48 Dispatch) | 0.27 ms |
| 鎖(1907・5055・5 段) | 1.55 ms | 0.48 ms(63 Dispatch) | 0.14 ms |
| たくさんの要求(20996・100723・8 段) | 2.67 ms | 2.08 ms(108 Dispatch) | 0.21 ms |
- 本体・wt3・wt4 のランナーが idle の時に測った(並走の負荷ありの 1 回目は 2.6〜3.2 / 0.6〜2.0 ms)。
- 上限 64 の差(1〜2 ms)は述語で飛ばした空の回の Dispatch とバリア(1 Dispatch 約 2 µs)。T-0127 の解く段(鎖 1.48〜2.91 ms)と同じくらい重いので、
  T-0135 で減らす(回を束ねて飛ばす・間接の Dispatch・形が変わらない刻みは作り直さない)。たくさんの要求の上限 = 段の数でも 2.08 ms あるのは
  節 10 万の段ごとの直列(1 回 14 段)と 1 スレッドの接頭和の段・Sort の長い行の数え上げ(段ごとの内訳は未計測。T-0135 で測る)。

## 判断待ち
- なし(番号の付け方・段の作り方・上限は実装の細部。遊びへの影響なし。費用は T-0135 で減らす工学)。

## 分けたもの(新しいチケット。ROADMAP には司令塔が足す)
- **T-0135**(工学): 陰解法の節の並び(長い行の節を後ろに・色ごとの並び・縮約の並び)・ImTail の境(節 1024 以下・隣 32 以下・最も粗い段)を GPU で決める・
  間接の Dispatch の引数(段の数・節の数・隣の数から。今は上限から投げて超えたスレッドは何もしない)・GpuImplicit の大きさを上限から決める
  (今は CPU の系の数で Create)・形が変わらない刻みは作り直さない(重みだけ毎刻み)形を測って決める。
  あわせて多重格子の段を作る費用を減らす: 上限 64 の空の回(述語で飛ばしても Dispatch とバリアが残る。上の計測で 1〜2 ms)を束ねて飛ばす、
  1 スレッドの接頭和の段・Sort の長い行の O(n²)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09(作業役、wt2・約 2 時間): 前の作業役の書きかけ(128bit の道具を implicit_conduction.hlsli へ移す)を引き継いだ。始める前の見積もりで
  全部は 2 時間に入らないと判断し、段・重み・値で決まる段の数に絞った(節の並び・ImTail・間接の Dispatch・上限から Create は T-0135)。
  親と行の番号は「鍵の表の atomic の最小 + 接頭和 + 一覧の中の順位」で CPU の初めて出会う順を再現し、行の係数は 128bit の繰り上げつき atomic で足した。
  最初のビルド(HLSL の構造体の三項演算子・float 検査の `round` という名前を直した後)で debug の WARP・release の HW・WARP がビット一致。
  debug の HW も通過(約 7 分)。main(T-0130)へ rebase した後、release で gpu_multires_implicit_build(_warp)・multires_implicit(_tree)・
  gpu_multires_implicit_warp の 5 本が通過。tidy は時間の約束で流していない(マージ前に司令塔か T-0135 で)。

## 引き継ぎメモ(HANDOFF に載せるもの。司令塔がマージ後に反映)
- 状態: T-0134 完了(範囲を絞った)。陰解法の多重格子の段(節・隣・重み・親子・子の一覧)を GPU で作り(GpuImplicitLevels・implicit_levels.hlsl)、
  CPU の BuildImplicitGrid と番号まで毎刻みビット一致・それを GpuImplicit に写して解いた結果も一致(熱い点・鎖・たくさんの要求)。段の数は GPU が値で決める。
  節の並び・ImTail・間接の Dispatch・GpuImplicit の大きさを上限から決めるのは T-0135。
- 動いているもの: `-Filter "^gpu_multires_implicit_build(_warp)?$"`(release の HW・WARP 合わせて約 2.3 分〔計測込み〕/ debug の WARP 約 1.6 分・HW 約 7 分)。
  計測は `job.py run -Preset release -Exe gpu_multires_implicit_build_test -- --queue compute --measure-only`。
- 壊れているもの: なし。
- 決めたこと(Claude・実装の細部): ADR-0019 追記(T-0134: 親と行の番号は鍵の表の atomic の最小 + 接頭和 + 順位・係数は 128bit の繰り上げつき atomic・
  段の数は回ごとの述語・自分のルート署名)。
- 判断待ち: なし。
- 注意: 多重格子の段を作る費用は上限 64 で 1.6〜2.7 ms(空の回 948 Dispatch の固定費が主)・上限 = 段の数で 0.5〜2.1 ms と重い(T-0135 で減らす。docs/perf.md)。
  GpuImplicitLevels の RecordCopyTo・RecordReadback は RecordBuild と同じコマンドリストで呼ぶ(バッファは COMMON から UAV に昇格させて使うため)。
  GpuImplicit はまだ CPU の系で Create する(節の並び・ImTail も CPU から)。implicit_build.hlsl の作業場の後ろにセルの座標(CellKeyWord)を足した。
