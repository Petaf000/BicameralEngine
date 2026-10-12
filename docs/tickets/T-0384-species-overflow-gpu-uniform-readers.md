# T-0384 GPU の段が一様の値の溢れを読む(評価・広げる・細かくする・粗くする・伝導。T-0312 から分けた)

- Status: Doing(2026-10-12 一部: 評価・頁に広げる〔伝導なしの刻み〕まで。細かくする・粗くする・頁を返すは T-0389、伝導は T-0390)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052(追記 13・15)。関係: T-0199(CPU)・T-0276・T-0312・T-0237・T-0329

## 目的
溢れを使う GPU の世界で、一様なブロックの値 1 つの溢れ(WideUniformArea(slot) のセル 0 の範囲。T-0312)を、一様の値を読む段が全部読む。
これが無いと、溢れのある一様の値(畳む〔T-0312 の残り〕・CPU から写した世界)を GPU が溢れを落として刻む。

## やること(CPU の LoadWideUniform・StoreWideUniform を使う所と同じ所)
1. 一様の値の評価: 待ちの丸めの評価(multires_wait_step.hlsli の EvaluateUniformWait・multires_conduct.hlsli の EvaluateUniformCells)が、
   溢れのある一様の値は上限の無い形で評価する(CPU の MrUniformWaitOf(…, LoadWideUniform, …, true))。上限の無い形を 512 スレッドの段に
   持たせない(T-0290 の型。溢れのある一様の値は「変わる」として頁に広げて刻む段に回すか、64 スレッドの段で 1 回だけ評価する)。
2. 頁に広げる(wait_step の 185 行あたり・conduct の 421 行あたり。TreeExpand → 刻む): 頁の今の面へ一様の値の溢れを覆っていないセル全部に写す。
   頁の塊は TreeExpand の累積和で配る(512 × 溢れの数の成分。TreeWideReserve と同じ型)か、写す前に WideAllocate の要求にする。
3. 細かくする: 一様な親の一様な子(multires_graph.hlsl の 173 行あたり)は子の一様の値の領域へ溢れを写す(塊は TreeWideReserve)。
   一様な親から子の頁を作る所(RefineWideTails の一様な親の場合)も一様の値の溢れを写す。
4. 粗くする: 一様な子と一様な親の「同じなら頁が要らない」(multires_tree.hlsli の MrCoarsenKeepsUniform。CPU の CoarsenKeepsWideUniform)と、
   和集合の子の形(GpuWideChildren)・塊の上限(ChildSpeciesCount・CoarsenTailBound)が一様な子の溢れを読む。粗くして親が一様のままなら親の値は変わらない。
5. 頁を返す・ブロックを返す時: 前に使った枠の一様の値の溢れを空にする(CPU の PlaceRootBlock・FoldPage の uniformOverflow = {})と、
   要らない塊を返す(TreeWideTrim と同じ型で一様の値の領域も見る)。
6. 試験: CPU から溢れのある一様の値を写した世界(TestExpandWide・TestFoldWideExact の後半〔細かくする・粗くする〕の場面)を GPU でも刻み、
   CPU と HW・WARP が毎刻みビット一致・保存がビット一致。gpu_multires_test の RunUniformOverflowRoundTrip を広げる。

## 完了条件
- [ ] 溢れのある一様の値を持つ世界を GPU で刻んで、CPU の溢れの世界と毎刻みビット一致(評価・広げる・細かくする・粗くする・伝導)
- [ ] 既定の世界(wideCells を使わない)のシェーダー・VRAM・結果は変わらない

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-12(作業役。並走の作業ツリー wt5。約 1 時間 40 分。一部): 伝導なしの刻みの 2 段が一様の値の溢れを読む。
  評価(multires_wait_step.hlsli の EvaluateUniformWaitWide): 溢れの変種は一様の値をインライン + 一様の値の溢れ(WideUniformArea のセル 0)の
  速い形(RxGpuMidCell)で評価する(溢れの無い値も速い形。8 種以下はインラインとビット単位で同じで、9 種目を作るセルはどちらでも広げる。
  インラインの評価を置き換えるので反応の核の展開の数は増えない。64 スレッドの段)。頁に広げる: TreeExpand の溢れの変種
  (multires_tree_wide_expand。ReserveExpandChunks)が配った頁の面 0 に「溢れの数 × 覆われていないセル」の塊を一覧の順の累積和で配り、
  StepExpandedWait の CopyUniformTails が始まりの表と成分を写してから刻む。既定の世界は #ifdef の外を変えていない。
  試験: gpu_multires_test の RunUniformOverflowSteps(--part coarsen・wide。11 種の一様な根を写して 8 刻み。反応のある表 = 広げて刻む・
  反応の無い表 = 一様のまま)。debug のビルドは通った。**HW・WARP の試験の結果は未確認**(GPU の順番待ちが 1 時間を超え、
  2 時間の約束に間に合わなかった。wt5 のランナーに HW〔--queue compute --part coarsen〕→ WARP〔--warp〕の run を積んだ。結果は wt5 の runner/logs)。

## 引き継ぎメモ
- 道具: UniformTailCount(slot)(世界の枠だけ。観察の枠は 0)・UncoveredCellCount(block)(multires_wide_access.hlsli)。
  一様の値を速い形で読むのは LoadUniformMid(multires_wait_step.hlsli。33 種以上は false)。
- 伝導ありの刻みの TreeExpand も溢れの変種になったので塊は配るが、ConductPrepare はまだ写さない(T-0390)。配った塊は頁に付いたまま
  (面 0。刻みの WideAllocate は書く面の余りしか返さない)= 置き場の勘定(空き + 頁 + 端数 + 一様 = 全部)は崩れない。
- 一様の値の領域の塊は、ブロックを広げた後も付けたまま(CPU も uniformOverflow を残す。読まない)。返すのは T-0389(頁を返す・ブロックを返す時)
  か T-0312 の残り(畳む時に使い直す)。
- 費用: 溢れの世界の一様なブロックの評価が全部速い形になった(未計測。空気だけの溢れの世界の ns/セル を T-0389 か T-0390 で測る)。

## 判断待ち
- なし(実装の細部だけ)。仮(ユーザー未確認・取り消しやすい): 33 種以上の一様の値は GPU では評価せずに頁に広げる(CPU は上限の無い形で評価して
  変わらなければ一様のまま = その場面だけ食い違う)。プレイヤーへの影響: 33 種以上が混ざった静かな一様の場所が、GPU では毎回頁に広げて刻まれる
  (VRAM と刻みの費用が増える。反応の結果・保存は変わらない)。T-0389 で大きい形の評価(WideDeferred と同じ 64 スレッドの段)にする。

## 分けたもの
- T-0389: 細かくする(一様な親 → 一様な子・子の頁)・粗くする(MrCoarsenKeepsUniform・GpuWideChildren・塊の上限)・頁とブロックを返す時に空にする が
  一様の値の溢れを読む(前の「やること」3・4・5)。33 種以上の一様の値の評価(大きい形)。
- T-0390: 伝導の段(EvaluateUniformCells・ConductPrepare の広げる)が一様の値の溢れを読む(前の「やること」1・2 の伝導の分)。
