# ADR-0422 成分を持つ節の釣り合いの GPU 版の形(式は 1 つの源・k ごとのシェーダー・粗い段は GPU で作る)

- Status: Accepted の所(式の共有・8 色の Dispatch・ビット一致の確かめ方)/ Proposed の所(粗い段を GPU で作る・段 0 は今は写す。仮・ユーザー未確認)
- 日付: 2026-10-22
- 決めた人: Claude(wt2 の作業役。T-0970)。芯は ADR-0418(成分を持つ節の口)・ADR-0406(C-0 の GPU 版)・D-546(Compute が既定)
- 関係: ADR-0397・ADR-0406・ADR-0411・ADR-0418
- 設計書: docs/design/20-transport-substrate.md §9・§13.1 / 22-support-law.md §3.2

## 決定
1. **式は 1 つの源**: 節の LDLᵀ(EqbFactorize)・その解き(EqbSolveFactor)・k × k × k の 128bit の和(EqbAddProduct)・残差・延ばし方(EqbProlong)・
   Galerkin の 3 つの積(EqbAddTriple)・置き場の番地は shaders/common/equilibrium_block.hlsli に置き、CPU の SolveEquilibriumBlocks と GPU の段が同じ関数を呼ぶ。
   CPU の EquilibriumBlock・EquilibriumVector・EquilibriumBlockFactor はこの hlsli の型(値の作りは T-0919 のまま。support_law・equilibrium の試験が同じ値)。
2. **成分の数 k はシェーダーの定数**(EQB_K。k = 1・2・3・6 を別のシェーダーに)。回しは決まった数(6)+ FX_UNROLL で k を超える所は if で外す。
   構造体の配列はいつも決まった添字で読む(T-0397)。k を実行時の値にすると、回しを開けずに局所の配列になる。
3. **1 回の解き = 色ごとに 1 回の Dispatch**(8 色 × 掃き出しの数 × 段)+ 段ごとの縮約 1 回・延ばし 1 回。同じ色の節は隣でないので(斜めの面・粗い段の 27 点も)、
   色の中を並べて解いても CPU の 1 本の回しと同じ値(R3)。粗い段の直しは縮約の段が 0 にする(CPU の VCycle の 0 から始める直しと同じ)。
4. **段の形(節・隣・親子・色の一覧)は CPU の BuildEquilibriumBlockGrid が作ったものを写す**(既定のヒープの写し。毎段読む大きい SRV をアップロードのヒープから読まない)。
   **粗い段の係数と節の LDLᵀ は GPU が作る**(RecordPrepare: 段 0 の LDLᵀ と右辺 → 段 1 から Galerkin。和は 2^128 を法として厳密に足してから 1 回ずらすので、
   足す順〔子の順・行の順〕は結果に効かない)。係数が変わらない刻み(支えの今の場面)は RecordSolve だけ。
5. **段 0 は今は写す**(CPU の段 0 の対角と面のブロック・固定した面・右辺の項)。法則が自分の段で段 0 を組む形(陰の圧力の EqGpuFinishNode に当たる所)は
   使い手ごと(支え: セルが VRAM にあって GPU が面のブロックを組む・面の力と帳簿を GPU で作るのは T-0981)。
6. **ビット一致の確かめ方**(gpu_equilibrium_block_test): 支えの 10 場面(張り出し 2・片持ち梁・両端を固定した梁・アーチ 2・洞窟 3・掘った直後。CPU の SupportLaw の
   1 つの系)を 30 刻み毎刻み節 × 3 の値で、k = 1・2・3・6 の箱を 5 刻みで比べる。決定性は、色の一覧・行・子・固定した面の順を逆に積んだ GPU も毎刻み一致。

## 理由
- 式を 2 か所に書くと、LDLᵀ の丸め(Q30・切り捨て・支えの無い向き)が少しでもずれればビット一致が崩れる。1 つの源なら一致は作りから出る(初回でビット一致した)。
- 粗い段を GPU で作るのは、係数が刻みで変わる使い手(割れた後の支え・温度で硬さが変わる)で毎刻み段を写すと、写しの費用が解きより大きくなるため
  (k = 3 の係数は 822 B/節。写すのは形だけにし、係数は GPU に置いたまま作り直す)。

## 検討した代案
- **粗い段の係数も CPU で作って写す**: 作りは簡単だが、係数が変わるたびに 822 B/節(k = 3)を PCIe で写す。形は変わらず係数だけ変わる刻みが多いので GPU で作る。
- **節ごとに 1 スレッドで V サイクルを Work Graph で回す**: 段の間の同期が要り、Compute の Dispatch の列の方が単純(D-546。速さは測ってから)。
- **色を 2 色にして Jacobi を混ぜる**: ADR-0418 の通り、斜めの隣が同じ色になり順に依存する。

## 影響
- 作り手: 書く値は変わらない。成分を持つ節の法則(支え・3 次元の支え)が GPU で CPU と同じ答えになる。
- エンジン: GpuEquilibriumBlock(engine/src/sim/gpu_equilibrium_block)が増えた。CPU の equilibrium_block は共有の hlsli を呼ぶ形になった(値は同じ)。
- 費用(RTX 3070 Ti。docs/perf.md 2026-10-22): 1 回の解き(V 2 回)64³ で k = 1 73 ns/節・k = 3 57 ns/節。小さい系(支えの洞窟 3328 節)は Dispatch 252 本の固定費で 1.6 ms。
  VRAM は k = 1 180・k = 3 830・k = 6 2857 B/節(粗い段の 27 点の行 × k² が大半)。小さい系の固定費を下げる(段を 1 つの Dispatch にまとめる・小さい段は 1 グループで回す)のは T-0982。
