# T-0130 今までの丸めのコードを消す

- Status: Done(2026-10-09)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(2 時間の約束)
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: 02 §3 / 決定: D-429・ADR-0018 / 前のチケット: T-0122

## 目的(1〜2 行)
T-0122 で世界のどこも使わなくなった今までの丸め(D-424 の下限と毎刻みの乱数の丸め)のコードを消し、テストを待ちの丸めにする。

## 完了条件(チェックできる形で)
- [x] MultiresStepOptions::cutoffRounding と CPU・GPU のその道(multires_step.hlsl の Main・StepExpanded・MR_STEP_CUTOFF_ROUNDING)を消す
- [x] RxStepCell・RxEvaluateCell・RxCollectCandidates・RxDesiredExtent・RxScaleExtent・RX_EXTENT_CUTOFF_FRACTION・MrStepCell(Detailed)を消す
- [x] reaction・reaction_contention・gpu_reaction のテストを待ちの丸めに(D-424 の「進まない」を確かめる所は消す)
- [x] release の全部のテストを 2〜3 回に分けて流し、CPU と HW・WARP が毎刻みビット一致

## 決めたこと(Claude。実装の細部。ADR-0018 追記〔T-0130〕)
- 1 セルの反応の試験は 1 セルだけのブロック(RxLoneCell = セル + changedTick)を待ちの丸めで進める(RxAdvanceLoneCell。reaction_table の AdvanceLoneReactionCell)。
  GPU の試験(reaction_cells.hlsl)は区間(50 刻み)の初めに「直前の刻みに変わった」とみなす(刻みは 1 から)。待ちは記憶が無いので偏らない。
- MrSameCell は RxSameCell(reaction.hlsli に移した)を呼ぶ。stepFlags の 8 は空き。
- 計測の暖機(gpu_multires_conduction・gpu_multires_activity)は待ちの丸めで 40 回(400 回は TDR になった。T-0121)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-09: 前の作業役の書きかけ(CPU・GPU の道・reaction.hlsli・試験の大半)を引き継ぎ、残り(MR_STEP_CUTOFF_ROUNDING・multires_activity の比べ・
  gpu_multires_conduction の暖機・map.yaml・02 §3・ADR-0018 追記)を消した。1 セルの反応の試験は 1 セルだけのブロック(RxAdvanceLoneCell)を待ちの丸めで。
  release の全部のテスト 84 本(3 回に分けた)と tidy(警告なし)が通過。T-0122 と T-0129 を合わせた状態で壊れていたものは無かった。作業 約 1 時間 30 分。
