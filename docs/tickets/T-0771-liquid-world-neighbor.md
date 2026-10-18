# T-0771 液体の面の隣を世界の Neighbor へ(T-0770 から分けた)

- Status: Done(2026-10-18。wt6。ADR-0349 ② ③)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ③' 運ぶ計算の共通の土台 段 2(T-0770 の続き)
- 設計: docs/design/20-transport-substrate.md §2.3〜§3 / 決定: ADR-0334・ADR-0335

## 目的(1〜2 行)
MultiresLiquid の `LiquidGrid`(根のブロックを索引で引いた独自の隣)を、世界の Neighbor(MrFindFaceNeighbor)に替え、液体の面の力・移流・上限を
当てる段の回し方(TxFlowCell の 6 面)とそろえる。違うレベルの面は断ったまま(段 2 の後)。

## 完了条件(チェックできる形で)
- [x] 面の隣を MrFindFaceNeighbor(MR_NEIGHBOR_SAME・世界の端は WeEdgeNeighbor)から。LiquidGrid は刻むブロックの一覧と面ごとの「先」の表だけに
- [x] 面の欄(+ 側の面)を読む向きと、TxFlowCell の 6 面の合わせ方: 面の力は持ち主(− 側のセル)が 1 回、当てる段はセルごとに 6 面を読んで自分の側だけ
      (transport_apply.hlsli の TxFaceFieldIsOwn・TxApplyPacketSide・World::AddEdgePacket)
- [x] multires_liquid_test(仮の世界と毎刻みビット一致・ComputeConservedTotals が一定)・release 警告 0・archmap --check

## メモ・参考
- T-0717 の「載せ替えの口」4(活性のブロックの一覧は T-0723 の後)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-18(wt6・作業役。T-0774・T-0777 と一緒に): LiquidGrid の索引の隣を世界の Neighbor(MrFindFaceNeighbor + WeEdgeNeighbor)の「先」の表に替え、
  当てる段をセルごとの 6 面(自分の側だけ当てる)に。包みは両側が同じ値から作るので値は前とビット単位で同じ(仮の世界との比べがそのまま通った)。
  違うレベルの面は断ったまま(FX_ASSERT)。費用: 池 16³ で MR 7.0 ms/刻み(包みを 2 回作る分を含む)。
