# T-0384 GPU の段が一様の値の溢れを読む(評価・広げる・細かくする・粗くする・伝導。T-0312 から分けた)

- Status: Todo(T-0312 の一部〔一様の値の溢れの領域〕の後。T-0312 の残り〔溢れまで畳む〕の前に要る)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052(追記 13)。関係: T-0199(CPU)・T-0276・T-0312・T-0237・T-0329

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
