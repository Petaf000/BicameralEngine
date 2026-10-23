# T-1085 成分を持つ節の共役勾配を GPU 版にも足し、支えの既定にする

- Status: Todo(ADR-0455。CPU 版は 2026-10-24 に wt2 で足した・既定は切)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列 1(C-0)の直し
- 設計: docs/design/20-transport-substrate.md §9・22 §3.1 / 決定: ADR-0418・ADR-0455・D-617

## 目的(1〜2 行)
`EquilibriumBlockOptions::lineSearch`(V を前処理とする共役勾配。刻みの中だけ)を gpu_equilibrium_block(T-0970)にも足し、CPU とビット一致させてから、
支え(support_law)の既定を V 4 回 + 共役勾配にする(壁につながる棚が 9/10 に 32 / 69 刻み。今は 1153 / 1711)。

## 完了条件
- [ ] GPU: 細かい段の残差 2 回・内積 3 つ(整数の和。128bit を 2 語で足す)・β と α(Q16。shaders/common/equilibrium_block.hlsli に CPU と同じ関数)
- [ ] gpu_equilibrium_block で lineSearch の CPU とビット一致(支えの場面の全部)
- [ ] 支えの既定を V 4 回 + 共役勾配に: support_law・support_fracture の値の変わり方を記録(釣り合った後の答えは同じはず・着くまでの刻みが変わる)・陰の圧力はビット一致のまま
- [ ] 費用(GPU。RTX 3070 Ti 基準 D-634)を docs/perf.md に

## 作業ログ
