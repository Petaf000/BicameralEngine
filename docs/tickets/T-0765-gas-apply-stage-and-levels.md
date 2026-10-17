# T-0765 気体を当てる段(TxFlowCell)に載せ、係数をレベルの関数に(T-0720 から分けた)

- Status: Todo
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
- [ ] 気体の種類(Slope・面の式・安定の条件・包み)を transport_*.hlsli に。上限・当てるは当てる段の 1 か所
- [ ] 入る側の上限で値が変わる場面を説明して基準を取り直す(D-487 ③)。変わらない場面(静かな大気・弱い流れ)は仮の世界とビット一致のまま
- [ ] レベル 1 の根の世界で、レベル 0 と同じ物理(音の速さ・浮力)になることを試験で

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
