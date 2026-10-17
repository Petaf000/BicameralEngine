# T-0770 多重解像度の 1 段の液体を当てる段(T-0714)に載せ替える(T-0717 から分けた)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分(2 時間で区切る)
- マイルストーン: ③' 運ぶ計算の共通の土台 段 2(T-0717 の続き。T-0718 の前か同時)
- 設計: docs/design/20-transport-substrate.md §2.3〜§2.4 / 決定: ADR-0326・ADR-0334(Proposed)

## 目的(1〜2 行)
T-0717 の `sim/multires_liquid.cpp` が仮に持つ「上限(出る側だけ。LqLimitOutflow)・当てる(成分の増減の包み ApplyDelta)・起こす」を、
T-0714 の当てる段(shaders/common/transport_apply.hlsli の TxFlowCell・TxLimit・世界の Add / Neighbor / View)へ載せ替え、液体を「種類」の 1 つにする。

## 完了条件(チェックできる形で)
- [ ] 当てる段の包みを、熱だけ(MrEnergyDelta)から「成分ごとの量 + エネルギー + 運ぶ欄」へ広げる(液体と気体〔段 3〕が同じ形を使う)
- [ ] 液体の種類: 坂(柱の圧力 + 重さ)・面の式(LqOpenFaceForce)・安定の条件(出る合計 ≤ 中身 = LimitsOutflow・速さの上限)と読む欄の宣言
- [ ] 上限が「出る側と入る側の倍率の min」になる所で値が変わるなら、差を説明して基準(multires_liquid_test の仮の世界との比べ)を取り直す(D-487 ③)。
      入る側の器の残り = 液体の空き(溢れの圧力で押し込める分をどう数えるか)を決めてから
- [ ] `LiquidGrid` の面の隣を世界の Neighbor(MrFindFaceNeighbor)に替える(違うレベルの面は T-0717 の範囲外のまま断る)
- [ ] release のビルド 警告 0・multires_liquid・transport_apply・archmap --check

## メモ・参考
- 載せ替える所は T-0717 のチケットの「載せ替えの口」1・4・6。帳簿(口 3)は T-0769、欄が物質に付く(口 2)は T-0718。
- 仮の世界(probe_liquid)は基準として残す(D-513)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
