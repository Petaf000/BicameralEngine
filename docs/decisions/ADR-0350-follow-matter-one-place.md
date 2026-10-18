# ADR-0350 欄が物質に付く式を当てる段の 1 か所に(follow_matter.hlsli)

- Status: Proposed(2026-10-18。T-0718。工学の整理で値は変わらない。ADR-0286 の決まりそのものは ADR-0286 のまま)
- 日付: 2026-10-18
- 決めた人: Claude(wt6 の作業役)
- 関係: ADR-0286(流れの欄は物質に付く)・ADR-0334(当てる段)・ADR-0349(MR の液体)・設計 20 §2.4 ④

## 背景
- ADR-0286 の「段の外で質量が変わったら、減れば速さを保つ・増えれば運動量を保つ」は、気体(gas_flow.hlsli の GfMomentumAfterMassChange)と
  液体(liquid_flow.hlsli の LqFollowMassChange)で同じ式を 2 つ持ち、CPU(gas_cells.cpp = 仮の世界と MR の気体・probe_liquid.cpp・multires_liquid.cpp)と
  GPU(probe_gas_tick.hlsl・probe_liquid_tick.hlsl)の 5 か所がどちらかを呼んでいた(設計 20 §2.4 ④ の「4 か所 → 1 か所」)。

## 決定
1. 式は shaders/common/follow_matter.hlsli の FmMomentumAfterMassChange(運動量 1 つ・前の質量・今の質量)の 1 か所。0 の向きへ切り捨て
   (前の 2 つと同じ式 = 値はビット単位で同じ)。GfMomentumAfterMassChange・LqFollowMassChange は消した。
   気体のセルは軸ごと、液体の面は面ごとに呼ぶ(欄の持ち主の質量 = 気体はセル・液体は面の両側の和)。
2. 当てる段(transport_apply.hlsli)の 4「欄が物質に付く」に決まりを書き、follow_matter.hlsli を含める(引数の形は変えない。TxFlowCell の
   World::FollowFields はそのまま)。gas_flow.hlsli・liquid_flow.hlsli も含めるので、今の呼ぶ側は含める物を変えずに使える。
3. 帳簿は両方とも世界の帳簿の理由「段の外の質量の変化」(WL_REASON_FOLLOW_MASS。MR の気体 T-0763・液体 T-0769 で既にそう)。
   運動のエネルギー・位置のエネルギーは種類が今の質量から計算し直す(気体 = 前の値を熱へ戻して計算し直す・液体 = 刻みの終わりに計算し直し、
   位置のエネルギーの差は同じ理由で帳簿へ)。これは種類の持ち物(運動のエネルギーの式が種類ごとに違う)なので 1 か所にしない。

## 代案
- 回し方(前の質量の欄を読み、持ち主ごとに式を呼び、差を帳簿へ)まで当てる段の関数にする: 気体はセル・液体は面と持ち主が違い、
  CPU と GPU で欄の置き場も違う(仮の世界の配列・MR の登録の欄・GPU のバッファ)。世界の側の回し方なので今は分けない
  (MR の気体と液体が当てる段に載り、GPU の段が MR の欄を読むようになった時に、World::FollowFields の中身として 1 つにできるか見る)。

## 確かめたこと
- release のビルド警告 0。式が同じなので既存の試験(probe_gas・probe_liquid・multires_gas・multires_liquid・gas_reference・GPU の gpu_probe_liquid・
  gpu_probe_sim ほか)がそのまま通る(下のチケットの作業ログ)。
