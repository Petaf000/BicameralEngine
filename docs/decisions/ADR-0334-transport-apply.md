# ADR-0334 面の帳簿と当てる段: 運ぶ計算の 6 つの順を共有 .hlsli の 1 か所に置き、流れの種類は坂・面の式・安定の条件だけを渡す

- Status: Proposed(2026-10-17。T-0714。土台の設計 20 §2.3〜§2.4 を形にした。細部は仮〔ユーザー未確認〕)
- 日付: 2026-10-17
- 決めた人: Claude(T-0714 の作業役)
- 関係: ADR-0326(運ぶ計算の共通の土台)・ADR-0333(セルの見立て)・ADR-0331(世界の帳簿)・ADR-0017 / ADR-0015(違うレベルの面の端数)・T-0108(小刻み)・T-0762(帳簿の書き方の約束)・T-0715(陰の伝導)・T-0716 / T-0719(液体・気体の芯)

## 背景
- 運ぶ計算は種類(伝導・気体・液体・木)ごとに、流す・上限・当てる・欄・帳簿・起こすを別々に書いていた(上限は 8 か所・式 4 種。設計 20 §5)。
- 土台 §2.4: これを 1 つの当てる段にし、種類は 3 つ(坂・面の式・安定の条件)だけを持つ。最初の種類は MR の陽の伝導(今の試験で答え合わせができる)。

## 決定(仮)
1. **当てる段の場所**: `shaders/common/transport_apply.hlsli`。セル 1 つの 6 面を `TxFlowCell<Kind, World>` が 1 流す → 2 上限 → 3 当てる → 4 欄 → 5 帳簿 → 6 起こす の順に回す。
   HLSL 2021 と C++ のテンプレート(種類と世界はメンバー関数を持つ構造体)。回し方(どのセルを、どの順・どのスレッドで)だけが CPU と GPU で違う。
2. **種類の約束**(3 つ): 坂 `Slope(view)` / 面の式 `SideCoefficient`・`CoarseSideCoefficient`・`FaceFlow` / 安定の条件 `SubcycleShift(level)`・`LimitsOutflow()`。
   欄の宣言 `FieldCount()`(0 なら 4 を飛ばす)。最初の種類 `TxConduction`(`transport_conduction.hlsli`): 坂 = 温度、面の式 = min(K) ΔT(HcFaceFlow)、
   安定 = min(G × 4^(k−σ), C/8)・粗い側 × 2^d ÷ 小刻みの回数(`MrSubstepCoarseLimit` に切り出した)・小刻み 4^σ。式は multires_conduction.hlsli の関数を呼ぶだけ(GPU の伝導の段と同じ源)。
3. **世界の約束**: `Neighbor`(流さない面は NONE)・`View`(見立て)・`Level`・`HasFraction`・`Limit`・`Add`(変化の表へ書き足す)・`FollowFields`。
   土台 §3 の「世界に求めるもの 4 つ」と同じ。MR の CPU は `ConductionWorld`(multires_conduction.cpp)。
4. **上限の形**: セルごとに `TxLimit { outScale, inScale }`(Q32。`TxMakeLimit(中身, 出る合計, 器の残り, 入る合計)`・列が複数なら `TxMinLimit`)。
   面の倍率 = min(出ていく側の outScale, 入る側の inScale)(`TxFaceScale`)。流れは大きさを切り捨てで縮める(`TxScaleFlow`。両側が同じ大きさ = 保存)。
   包みの列(物質ごとの量・エネルギー・欄)はどれも同じ倍率で縮める。伝導は C/8 で足りるので使わない(`LimitsOutflow = false`)。
5. **帳簿**: 伝導は断熱の端で外へ出ないので書かない。開いた端を流す種類(段 3)が 4 の後に足す。書き方は T-0762 の約束(列は NestFieldLedgerBase / NestConservedIndex で引く・
   Units と Bits・符号付きは正 → 出・負 → 入・保存の検査は ComputeConservedTotals)。
6. **見立てを読む**: MR の CPU の伝導は熱のキャッシュ(CellThermals)をやめ、刻みごとの見立ての置き場 `CellViews`(MakeCellView。枠ごとに初めて読んだ時に場所を取る)から読む。
   小刻みで変わったブロックだけ作り直す(段の中。段の外の後始末ではない)。陰解法(T-0715 まで)も `CellViews::At`(見立ての thermal)を読む。

## 結果
- 今の MR の CPU の試験(multires・multires_state・multires_wide・multires_activity・multires_quiet・multires_uniform・multires_conduction・multires_subcycle・
  multires_implicit・multires_implicit_tree)が置き換えた形でそのまま通る(ビット一致)。transport_apply_test が上限の形と、種類の面の流れ = 今の式を確かめる。
- 熱のキャッシュの後始末: MR の CPU は前から刻みごとの写し(段の外の後始末 0)。残る 13 か所は仮の世界(probe_sim.cpp 7・GPU の probe_tick 4・probe_gas_tick 1・probe_liquid_tick 1)で、
  仮の世界を土台に載せる・退かせる時に消える。
- 費用は docs/perf.md(CPU)。

## 代案
- 種類を関数の表(関数ポインタ)で渡す: HLSL に無い。テンプレートなら CPU と GPU で同じ源。
- 上限を Gauss-Seidel(順に縮める): 順に依存して CPU と GPU で合わない。Jacobi(先に倍率を数える)にした。
- 見立てを全部持たずに伝導の分(MrThermal)だけ持つ: 速いが、種類が増えるたびに写しが増える(土台 §2.2 に反する)。費用を測って決める(perf.md)。
