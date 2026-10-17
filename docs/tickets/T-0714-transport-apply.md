# T-0714 面の帳簿 + 当てる段 + 陽の伝導を載せる(運ぶ計算の共通の土台の段 1 ④。MR の CPU)

- Status: Done(2026-10-17。GPU の段は T-0766・見立ての費用は T-0767・陰の伝導は T-0715)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP「③' 運ぶ計算の共通の土台」段 1
- 設計: 20 §2.3〜§2.5 / 決定: ADR-0326・ADR-0334(Proposed)

## 目的(1〜2 行)
流す → 上限 → 当てる → 欄 → 帳簿 → 起こすを共有 .hlsli の 1 か所(当てる段)にし、流れの種類は坂・面の式・安定の条件だけを渡す形にする。
最初の種類として MR の陽の伝導を載せ、見立て(MakeCellView)から係数を読み、今の MR の試験とビット一致にする。

## 完了条件(チェックできる形で)
- [x] `shaders/common/transport_apply.hlsli`: `TxFlowCell<Kind, World>`(6 つの順)・`TxFaceFlow`・上限(`TxLimit`・`TxSideScale`・`TxMakeLimit`・`TxMinLimit`・`TxFaceScale`・`TxScaleFlow`)・`TxWakes`
- [x] `shaders/common/transport_conduction.hlsli`: 種類 `TxConduction`(式は multires_conduction.hlsli の関数を呼ぶだけ。粗い側の係数を `MrSubstepCoarseLimit` に切り出した)
- [x] MR の CPU の伝導(multires_conduction.cpp)を当てる段に置き換え(`ConductionWorld`)。熱のキャッシュ CellThermals → 見立ての置き場 CellViews(陰解法も見立ての thermal を読む)
- [x] 今の MR の CPU の試験 10 本(multires・multires_state・multires_wide・multires_activity・multires_quiet・multires_uniform・multires_conduction・multires_subcycle・multires_implicit・multires_implicit_tree)と cell_view が通る(ビット一致)
- [x] `transport_apply_test`(上限の形・種類の面の流れ = 今の式 10 万面)・`transport_apply_check.hlsl`(HLSL でコンパイルできるか。実行しない)
- [x] release のビルド 警告 0・archmap(map.yaml に transportapply)・perf.md

## 当てる段の型(wt4 T-0719・wt6 T-0716 が呼ぶ。早めに決めた形)
- **種類(Kind)の約束**: `int64_t Slope(CvCellView)` / `uint64_t SideCoefficient(CvCellView, int32_t level, uint32_t shift)` /
  `uint64_t CoarseSideCoefficient(CvCellView coarse, int32_t fineLevel, uint32_t gap, uint32_t fineShift, uint32_t coarseShift)` /
  `int64_t FaceFlow(int64_t slopeA, uint64_t coefA, int64_t slopeB, uint64_t coefB)`(A → B が正)/ `uint32_t SubcycleShift(int32_t level)` /
  `bool LimitsOutflow()` / `uint32_t FieldCount()`。メンバー関数を持つ構造体(HLSL では const の実体で呼ばない)。
- **世界(World)の約束**: `Neighbor(slot, MrBlock, index, face)`(流さない面は MR_NEIGHBOR_NONE)・`View(slot, index)`・`Level(slot)`・`HasFraction(slot)`・
  `Limit(slot, index) → TxLimit`・`Add(slot, index, MrEnergyDelta)`・`FollowFields(slot, index)`。
- **上限**: セルごとに `TxLimit { outScale, inScale }`(Q32。1 = `TX_SCALE_ONE`)。列ごとに `TxMakeLimit(中身, 出る合計, 器の残り, 入る合計)` → `TxMinLimit` で最小。
  面は `TxFaceScale(出ていく側, 入る側)`、流れ(包みの各列)は `TxScaleFlow(flow, scale)`(大きさを切り捨て・符号を保つ = 両側で同じ)。
  出る合計・入る合計は面の生の流れから先に数える(Jacobi。その回の全部の面を 1 回流してから当てる)。
- **包み**: 今の当てる段はエネルギーの列 1 つ(MrEnergyDelta。違うレベルは MrSplitCrossFlow で端数)。物質の列・欄の列を運ぶ種類(気体・液体)は、
  同じ形で列ごとに `TxScaleFlow` し、`Add` を列の数だけ持つ世界にする(段 2・3 で TxFlowCell に包みの型を足す。T-0716・T-0719 と合わせる)。
- **帳簿**: 伝導は書かない。書く種類は T-0762 の約束(列は NestFieldLedgerBase / NestConservedIndex・Units と Bits・正 → 出・負 → 入・保存の検査は ComputeConservedTotals)。

## メモ・参考
- **熱のキャッシュの後始末の数え**: MR の CPU の伝導は前から刻みごとの写し(CellThermals。段の外の後始末は 0)なので、この本で消えた段の外の後始末は 0。
  段の中の作り直し(小刻みで変わったブロックの Invalidate)は 1 か所のまま(見立ては変わったセルだけ作り直す。土台 §2.5)。
  残るのは仮の世界の ProbeMakeCache 13 か所(probe_sim.cpp 7〔初めの作成 3 を除く〕・probe_tick.hlsl 4・probe_gas_tick.hlsl 1・probe_liquid_tick.hlsl 1)。仮の世界を土台に載せる・退かせる時に消える。
- **費用(CPU release。docs/perf.md)**: ctest の時間(同じランナー・ほかの作業ツリーと並走なので揺れる)は multires_conduction 17.3 → 20.6 s・multires_subcycle 44.3 → 66.2 s・
  multires_implicit_tree 21.3 → 43.3 s・ほかはほぼ同じ。見立て(545 ns/セル)が MrCellThermal より重いのと、面ごとに見立て(約 200 バイト)を値で写すため(未確認。測って直すのは T-0767)。
  鎖の場面(伝導・活性)は 1 刻み 9.5 ms。
- 置き換えの前提: 見立ての thermal は MrCellThermal とビット一致(T-0713 の試験)。伝導の基準の圧力は 0(読まない)。溢れの世界は LoadWideNestCell の形で見立てを作る。
- cell_view.cpp を bicameral_sim から bicameral_multires へ移した(MR の CPU が相の表を作るため)。StepBlocks・StepConduction は相の表(span)を受け取る。

## 分けたもの
- **T-0766**(工学・GPU): GPU の MR の伝導の段(shaders/sim/multires_conduct.hlsli)を当てる段(TxFlowCell + 64bit の atomic の世界)に載せ、transport_apply_check.hlsl を消す。GPU の見立ての相の表(16 バイトの配列)も。
- **T-0767**(工学・CPU): 見立ての費用。面ごとの写しをやめる(参照で読む世界)か、種類が読む欄だけ作る形を測って選ぶ(multires_implicit_tree が約 2 倍)。
- T-0715(陰の伝導)は ROADMAP の行のまま(陰解法は CellViews::At で見立ての thermal を読むようになった)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-17(wt5 の作業役): transport_apply.hlsli・transport_conduction.hlsli を足し、MR の CPU の伝導を当てる段に置き換えた(CellThermals → CellViews)。
  release 警告 0・CPU の MR の試験 10 本 + cell_view + transport_apply が通った(ビット一致)。ADR-0334(Proposed)。GPU は T-0766、費用は T-0767 に分けた。
