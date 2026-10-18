# T-0773 世界の端の GPU(伝導の段)と陰解法の端

- Status: In Progress(2026-10-18。wt3。T-0766 と同じチャット)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分(T-0766 と一緒)
- マイルストーン: ROADMAP「③' 運ぶ計算の共通の土台」段 3(T-0721 から分けた)
- 設計: 20 §1.1・§2.4 / 決定: D-514・D-518(上の軸は y)・ADR-0337・ADR-0338・ADR-0343(Proposed)

## 目的(1〜2 行)
GPU の MR の伝導の段でも開いた世界の端が外の柱と流れ、帳簿の「世界の端」に載る(CPU の T-0721・T-0772 とビット一致)。陰解法の端。

## 完了条件(チェックできる形で)
- [x] 伝導の段の世界が world_edge.hlsli の WeEdgeNeighbor を呼ぶ(面の隣が世界の外なら)・外の柱の見立て(World::Outside)・帳簿は AddWorldLedgerUnits(AddWorldLedgerCarrying)
- [x] 世界の箱・端の種類・外の柱を GPU へ(表の語 t2。RecordUpload が nest.edges と違えば作り直す)
- [x] gpu_multires_conduction に「端の鎖」(6 面 open・高さで温度の違う外の柱。活性 Compute / Work Graph・全部、小刻みも)
- [ ] 試験が通る(GPU の端の鎖が毎刻み CPU とビット一致・帳簿と種も)
- [ ] 陰解法の端: CPU は wt5 の T-0790(共有の .hlsli)、GPU はそれに合わせて T-0784 に分けた

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
