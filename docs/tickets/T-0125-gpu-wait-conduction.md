# T-0125 GPU の伝導の段を待ちの丸めに・畳みと引き戻しで tc を書き直す

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: 02 §3.1・17 §5 / 決定: ADR-0018 / 前のチケット: T-0121

## 目的(1〜2 行)
T-0121 の残り: 熱の伝導を入れる刻み(ConductApply・小刻みの終わり・起こす一覧)を待ちの丸めにし、許容差つきで畳んだ時(CPU の FoldQuietPages・GPU の TreeFoldCheck)と
影の引き戻し(PullBackShadowChain・PullBackNode)で tc を書き直す。gpu_multires_conduction・subcycle・near_fold も待ちの丸めで比べ、cutoffRounding を伝導のテストから外す。

## 完了条件(チェックできる形で)
- [ ] GPU の伝導の段が待ちの丸めで刻み、見出しを CPU の StepBlocks と同じに書く(RecordStep・RecordStepActive の FX_ASSERT を外す)
- [ ] 許容差つきで畳んだ・影を引き戻したブロックの busyTick を書き直す(CPU・GPU。「古い乱数・新しい f」を無くす)
- [ ] gpu_multires_conduction・_subcycle・_near_fold(HW・WARP)が待ちの丸めで毎刻みビット一致(HW は T-0124 しだい)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
