# T-0389 GPU の細かくする・粗くする・頁を返す時が一様の値の溢れを読む(T-0384 から分けた)

- Status: Todo(T-0384 の一部の後。T-0312 の残り〔溢れまで畳む〕の前に要る)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- 設計: 02 §3・17(R-MULTI-4)・ADR-0052(追記 13・15)。関係: T-0199(CPU)・T-0237・T-0312・T-0329・T-0384

## 目的
溢れを使う GPU の世界で、一様の値の溢れ(WideUniformArea(slot) のセル 0 の範囲)を、細かくする・粗くする・返す段が読み書きする。

## やること
1. 細かくする: 一様な親の一様な子(multires_graph.hlsl の 173 行あたり)は子の一様の値の領域へ溢れを写す(塊は TreeWideReserve)。
   一様な親から子の頁を作る所(RefineWideTails の一様な親の場合・ReserveRefineChunks の「一様な親の頁は溢れを持たない」)も一様の値の溢れを写す。
2. 粗くする: MrCoarsenKeepsUniform(CPU の CoarsenKeepsWideUniform)・和集合の子の形(GpuWideChildren)・塊の上限(ChildSpeciesCount・
   CoarsenTailBound)が一様な子の溢れを読む。粗くして親が一様のままなら親の値は変わらない。
3. 頁を返す・ブロックを返す時: 前に使った枠の一様の値の溢れを空にする(CPU の PlaceRootBlock・FoldPage の uniformOverflow = {})・要らない塊を返す。
4. 33 種以上の一様の値の評価(T-0384 の仮): 速い形に入らない値を大きい形で評価する(64 スレッドの段。WideDeferred と同じく反応の核を 1 か所)。
5. 試験: TestFoldWideExact の後半(細かくする・粗くする)の場面を GPU でも。CPU と HW・WARP が毎刻みビット一致・保存がビット一致。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
