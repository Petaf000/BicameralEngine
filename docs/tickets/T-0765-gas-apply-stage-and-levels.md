# T-0765 気体を当てる段(TxFlowCell)に載せ、係数をレベルの関数に(T-0720 から分けた)

- Status: Done(2026-10-18。wt4。ADR-0345。多重レベル〔違うレベルの面〕の気体と GPU の段は後の本)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 2 回分
- マイルストーン: ③' 運ぶ計算の共通の土台 段 3(ROADMAP。多重レベルの気体の前提)
- 設計: docs/design/20-transport-substrate.md §2.3〜§2.4・§3 / 決定: ADR-0326・ADR-0334(Proposed)

## 目的(1〜2 行)
多重解像度の 1 段の気体(T-0720)は、上限(出る側だけ: LimitOutflow・LimitMovedAmounts)と当てる(Transfer)を気体の箱の小刻みの中に持つ。
面の生の包み(成分ごとの量・エネルギー・運動量)を T-0714 の当てる段(TxFlowCell。出る側と入る側の倍率の min)へ渡す形に替える。
係数(0.125 m³ の PRESSURE_PER_AMOUNT_TEMPERATURE_Q32・Δx = 0.5 m の centralFlow・pressureFlow・pressureImpulse・gravityImpulse)をレベル k の関数にし、小刻みを 2^σ に。

## 完了条件(チェックできる形で)
- [x] 上限・当てるは当てる段の 1 か所(transport_apply.hlsli の TxLimit・TxLimitAmount・TxLimitColumn〔足した〕・TxPacket・TxApplyPacket)。
      気体の坂・面の式(圧力 + 重さ・中心の流れ + 圧力の拡散)は前から gas_flow.hlsli(GPU と共有。T-0719)にあり、そのまま使う
      (transport_gas.hlsli へ移して TxFlowCell の種類の形〔Slope・SideCoefficient〕にするのは、気体の面の式が両側の 2 つ外の隣〔MUSCL〕と
      力を当てた後の運動量を読む形なので、多重レベルの気体〔違うレベルの面〕の時に決める。下の「分けたもの」)
- [x] 入る側の上限で値が変わる場面を説明して基準を取り直す(D-487 ③): 気体は圧縮できるので入る側は縮めない(TX_ROOM_UNLIMITED。液体と同じ)。
      出る側の式は前と同じ切り捨て 1 回 → **どの場面も値が変わらない**(仮の世界・GPU の段・箱の試験とビット一致のまま。取り直し無し)
- [x] レベル 1 の根の世界で、レベル 0 と同じ物理(音の速さ・浮力)になることを試験で(multires_gas_test 7)

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-18(wt4・作業役): 気体の箱の小刻み(箱・仮の世界・MR が使う 1 つの CPU の小刻み)の上限と移すを当てる段の関数へ(GasApplyWorld。
  移す塊 = 包み TxPacket・重さの仕事は気体の種類の当て方 AddGravityWork)。係数をセルのレベルの関数に(GasConfig::cellLevel。面の式 × 2^k・層 × 2^-k・
  粘り × 4^k・音速の上限・基準の大気の層の厚さ)、MR の小刻みを × 2^σ(σ = max(0, k))。静止の丸めを当てる段・粘りの後・書き戻しの前へ(RestGasCells)。
  レベル 0 は全部ビット単位で同じ。レベル ±1 の静かな大気は 20 刻み動かない・保存も一定。音の波の先(2 刻み)レベル 0 12750 mm / 1 13125 mm(理論 13333)・
  熱い泡の湯気の重心の上がり(30 刻み)577 / 679 mm(幅は仮: ±0.75 m・±30%)。TxMinScale は片方が 1 なら掛け算をしない(同じ値)。決定は ADR-0345。

## 判断待ち(仮〔ユーザー未確認〕。取り消しやすい)
1. **粗い世界の小刻みの数**(ADR-0345 判断待ち 1): 仮 = 粗いレベルもレベル 0 と同じ小刻み(σ = max(0, k))。作り手から見た違い: 1 m・2 m のセルの世界の
   気体の費用が小刻みの分だけ高い(結果は同じ物理)。案 B = 粗いほど半分。おすすめ: 多重レベルの気体で入れ子の小刻みを決めるまで仮のまま。

## 分けたもの・申し送り
- 気体の坂・面の式を TxFlowCell の種類の形にする・違うレベルの面(粗い側の受け直し 2^σ)は多重レベルの気体の本で(ROADMAP の段 3 の後。新しい番号は付けていない)。
- GPU の気体の段(gas_step.hlsl・probe_gas_tick.hlsl)は今も gas_flow.hlsli の GfLimitFraction・GfLimitMovedAmount(同じ式)。GPU の当てる段へは GPU の本で。
- gas_tree(古い木)の MakeLevelCoefficients は同じ 4 つの面の係数を自前で作る(層の重さ・重さの仕事は割る数で 2^-k)。木は土台に載せ替えて退かせるので触っていない。
