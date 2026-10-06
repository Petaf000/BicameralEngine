# T-0125 GPU の伝導の段を待ちの丸めに・畳みと引き戻しで tc を書き直す

- Status: Done(2026-10-06)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: 02 §3.1・17 §5 / 決定: ADR-0018 / 前のチケット: T-0121

## 目的(1〜2 行)
T-0121 の残り: 熱の伝導を入れる刻み(ConductApply・小刻みの終わり・起こす一覧)を待ちの丸めにし、許容差つきで畳んだ時(CPU の FoldQuietPages・GPU の TreeFoldCheck)と
影の引き戻し(PullBackShadowChain・PullBackNode)で tc を書き直す。gpu_multires_conduction・subcycle・near_fold も待ちの丸めで比べ、cutoffRounding を伝導のテストから外す。

## 完了条件(チェックできる形で)
- [x] GPU の伝導の段が待ちの丸めで刻み、見出しを CPU の StepBlocks と同じに書く(RecordStep・RecordStepActive の FX_ASSERT を外す)
- [x] 許容差つきで畳んだ・影を引き戻したブロックの busyTick を書き直す(CPU・GPU。「古い乱数・新しい f」を無くす)
- [x] gpu_multires_conduction・_subcycle・_near_fold(HW・WARP)が待ちの丸めで毎刻みビット一致

## 決めたこと(Claude。実装の細部。ADR-0018 追記〔T-0125〕)
- 伝導の段は待ちの丸めだけ(今までの丸めの反応を残さない。RecordStep は conduction と cutoffRounding の組を FX_ASSERT で断り、
  RecordStepActive は cutoffRounding なら false)。伝導を入れる全部を刻む刻みも先に起こす段 WakeDue。
- 一様なブロックの評価は最初の小刻みの印の段(ConductMarkBlock)、頁のブロックの見出しは足して反応の段(ConductApplyBlock)が
  RecordWaitResults と同じに書く。64bit の最小は 32bit 2 回(GroupMinTick)。次の刻みの種は書かない(AppendSeedOnce は頁の不足だけ)。
- 許容差つきで畳んだら PokeBlock(まずビット単位で調べ、一様ならつつかずに畳む)。影を引き戻したら busyTick = MR_BUSY_POKED(種なし)。

## 注意(T-0124 から)
- ハードウェア(RTX 3070 Ti)の Work Graph は、反応の核(RxStepCell 系)を 1 つのノードに 3〜4 か所展開すると DEVICE_HUNG(BACKLOG)。伝導の段の Work Graph 版
  (multires_conduct_graph.hlsl)に待ちの丸めの反応を足すなら、1 ノード 2 か所まで(今までの丸めの反応を同じノードに残さない)。
- シミュの 64bit の式に「足して溢れたら全部 1」(飽和する足し算)の形を書かない(HW で下位 32bit だけになった。reaction.hlsli の RxWakeTickOf)。
- RecordStepActive は今 `conduction != cutoffRounding` なら false(伝導を待ちの丸めにしたらここを外す)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-06: 伝導の段(multires_conduct.hlsli の ConductMarkBlock・ConductApplyBlock)を待ちの丸めに、許容差つきの畳み(FoldQuietPages・TreeFold)は
  PokeBlock、影の引き戻し(PullBackLevel・PullBackNode)は busyTick = POKED。伝導のテストから cutoffRounding を外した。
  multires_uniform の「ほぼ同じで畳む頁が無ければ許容差なしと一致」が落ちた(ビット単位で一様でも平均の道を通ってつついた)→ 許容差つきでもまずビット単位で調べる。
  release で伝導・near_fold・subcycle・多重解像度の GPU と CPU が通過(HANDOFF に範囲)。
