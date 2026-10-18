# T-0718 欄の合わせ直し(欄が物質に付く)を当てる段の 1 か所に・位置/運動のエネルギー

- Status: Done(2026-10-18。wt6。ADR-0350)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ③' 運ぶ計算の共通の土台 段 2 ③
- 設計: docs/design/20-transport-substrate.md §2.4 ④ / 決定: ADR-0286・ADR-0350(Proposed)

## 目的
ADR-0286 の「流れの欄は物質に付く」の式を、気体・液体・CPU・GPU で 1 か所にする(設計 20 §2.4 ④ の 4 か所 → 1 か所)。

## 完了条件
- [x] 式を shaders/common/follow_matter.hlsli の FmMomentumAfterMassChange に(GfMomentumAfterMassChange・LqFollowMassChange を消した)。
      呼ぶ所: gas_cells.cpp(仮の世界と MR の気体)・probe_gas_tick.hlsl・probe_liquid.cpp・multires_liquid.cpp・probe_liquid_tick.hlsl
- [x] 当てる段の 4 に決まりと置き場を書いた(transport_apply.hlsli。引数の形は変えない)
- [x] 位置/運動のエネルギー: 種類が今の質量から計算し直し、差を熱(運動)・帳簿の「段の外の質量の変化」(位置)へ。どちらも今の形のまま(種類の持ち物)
- [x] 値はビット単位で同じ。release 警告 0・関係する試験・archmap --check

## 作業ログ
- 2026-10-18(wt6・作業役。int-10 から): 気体と液体の同じ式 2 つを follow_matter.hlsli の 1 つにまとめ、5 か所の呼ぶ側を替えた。
  回し方(前の質量の欄・持ち主・置き場)は世界の側に残した(ADR-0350 の代案。GPU の段が MR の欄を読むようになった時に FollowFields の中身として見直す)。
