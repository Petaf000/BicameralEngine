# T-1085 成分を持つ節の共役勾配を GPU 版にも足し、支えの既定にする

- Status: Done(2026-10-24・wt2。GPU の費用の ms だけ未測)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列 1(C-0)の直し
- 設計: docs/design/20-transport-substrate.md §9・22 §3.1 / 決定: ADR-0418・ADR-0455・D-617

## 目的(1〜2 行)
`EquilibriumBlockOptions::lineSearch`(V を前処理とする共役勾配。刻みの中だけ)を gpu_equilibrium_block(T-0970)にも足し、CPU とビット一致させてから、
支え(support_law)の既定を V 4 回 + 共役勾配にする(壁につながる棚が 9/10 に 32 / 69 刻み。今は 1153 / 1711)。

## 完了条件
- [x] GPU: ConjugateSave / Residual / Beta / Direction / Push / Alpha / Update(内積は 1 つのグループ 256 スレッドで 128bit の和)・β と α は common/equilibrium_block.hlsli の同じ関数
- [x] gpu_equilibrium_block: k = 1・2・3・6 の箱を V 2 回と V 4 回 + 共役勾配で、支えの全場面(既定 = 共役勾配)を 30 刻み、毎刻み CPU とビット一致(逆順も)
- [x] 支えの既定を V 4 回 + 共役勾配に: support_law は通る(合否の線の内)。support_fracture の A・B・C の折れる荷・場所は前と同じ(106 / 94 / 86 kN/m ほか)。
  **すぐ折れる棚 D(8 m)は 3 通りとも 1 刻み目で割れ始め、4〜7 刻み(0.07〜0.12 秒)で分かれた**(前は 3 本目で 2.1 / 3.7 秒)= 0.5 秒以内に合格
- [x] 陰の圧力・液体は変わらない: gas_projection・probe_liquid・gpu_gas_projection_t0915・gpu_probe_liquid が通る(成分 1 の SolveEquilibrium と GPU の equilibrium.hlsl は触っていない。equilibrium_block.hlsli を読むのは支えだけ)
- [ ] 費用(GPU。RTX 3070 Ti 基準 D-634)を docs/perf.md に: 未測(`run -Exe gpu_equilibrium_block_test -- --no-gbv --measure` に共役勾配の行を足して測る)。CPU は support_fracture が 289 → 709 秒(ほかの試験と重なりあり)

## 作業ログ
- 2026-10-24(wt2): GPU 版に共役勾配の 7 段を足し、数の式を hlsli で CPU と共有した。gpu_equilibrium_block で CPU とビット一致。支えの既定を V 4 回 + 共役勾配に。棚 D が 0.1 秒で折れる。
