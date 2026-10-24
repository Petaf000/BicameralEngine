# T-1111 工学 R-T2: 同じレベルの木で 1 出来事の系を GPU で解く(段 0 は CPU から写す)・ビット一致・1 出来事の時間

- Status: Todo(2026-10-24: 着手の見立てだけ。T-1089 の測りの後で 2 時間の線に届かず、作りは次の巡目)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列 6(docs/plan/ROADMAP.md の R-T2 の行。T-1048 から分けた)
- 設計: docs/design/20-transport-substrate.md §14.2 / 決定: D-546・ADR-0465・ADR-0456

## 目的(1〜2 行)
CPU の試作(time_step_tree)が 1 出来事に組む系(SpanSystem = 未知数 δ・節の容量・面の係数 × N・地面・反応の熱・値 0 を固定した外の面)を、
C-0 の GPU 版(GpuEquilibrium)で解き、CPU の SolveEquilibrium とビット一致を確かめて、1 出来事の時間と 1 セルの ns を V 1・V 2 で測る(T-0951 の気体からの見積もりの置き換え)。

## 作りの見立て(読んだ所: engine/src/sim/gpu_equilibrium.{h,cpp}・shaders/sim/equilibrium.hlsl・equilibrium_core.hlsli・gpu_equilibrium_block.h)
- GpuEquilibrium は段 0 を法則のシェーダーが組む形(気体は gas_projection.hlsl が EqGpuFinishNode・EqGpuSetRhs を呼ぶ)。熱の試作の段 0 は CPU が組んでいるので、
  **段 0 を写すだけの法則の段**(新しい `shaders/sim/equilibrium_upload.hlsl`: 節ごとに容量〔FxU128〕・6 面の隣と係数〔FxU128〕・右辺の項〔EqSourceTerm の形〕を
  バッファから読んで EqGpuFinishNode・EqGpuSetRhs を呼ぶ)と、それを積む小さな口(試験の中か `sim/gpu_equilibrium_upload.{h,cpp}`)を作る。部品(GpuEquilibrium・GpuImplicit)は変えない。
- 段の形は CPU の BuildImplicitGrid(SolveEquilibrium の中と同じ)で作って RecordUpload で写す。同じレベルの木(0.5 m の根だけ。単位 4 m = ブロック)なら今の GPU 版の範囲
  (違うレベルの節は T-0939)。
- GpuEquilibriumBlock(段 0 の係数を写せる k = 1 の版)も候補だが、CPU の参照が SolveEquilibriumBlocks で試作の SolveEquilibrium と違うので、ビット一致の相手にならない。
- 試験: `gpu_time_step_span_test`(新規)。試作の火事(shape=fine)を CPU で進め、決めた刻み(着火の後・前線の途中など 8 か所)の e = 0 の系を取り出して GPU で V 1・V 2 で解き、
  値 δ を CPU と比べる(ビット一致)。時間は GPU の時刻印(式〔写した段 0〕・段の重み・V・読み戻しを除く)を 8 回の最小で。系の大きさは 1 出来事のセルの数(約 600〜1,300)。

## 完了条件(チェックできる形で)
- [ ] 段 0 を写す法則の段と小さな口(部品は変えない)
- [ ] 火事の e = 0 の系 8 か所で GPU と CPU の値がビット一致(V 1・V 2)
- [ ] 1 出来事の時間(ms)と 1 セルの ns(V 1・V 2・段の重みあり / 無し)を RTX 3070 Ti で(GPU の順番待ち)。T-1090 の見積もりの表を置き換える
- [ ] T-1109 の最初の形(段の重みを形が変わった時だけ作り直す)をこの上で

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-24(wt4・ブランチ t-1063-rt2-budget): 着手前に部品を読んで作りの見立てを書いた。T-1089 の測りで 2 時間の線に近づいたので、作りは次の巡目。コードは変えていない。
