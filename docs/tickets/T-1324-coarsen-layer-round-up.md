# T-1324 層を使う世界で粗くする時の層の端数を舞う粒にしない(和 ÷ 8 の切り上げを量で抑える)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 2(docs/plan/ROADMAP.md)
- 設計: ADR-0489 の 3 (B)・ADR-0481 の 5 の 4・docs/design/20 §10.5 / 決定: D-687(Q175)/ 前: T-1322(土台の印 1)・T-1323

## 目的(1〜2 行)
粗くする時、子 2³ の層の和 ÷ 8 の切り捨ての余り(成分ごとに 0〜7 µmol)が層から残り(舞う粒)へ移る。Q175 (B)「舞う粒は崩れ・吹き上げの出来事からだけ」に
反するので、層を使う世界(cellLayers.enabled・GPU の MR_LAYER_FLAG_GRAIN)だけ 層 = min(和 ÷ 8 の切り上げ, 粗いセルの量) にする(司令塔の照らし 2026-11-12。新しい問いにしない)。

## 完了条件(チェックできる形で)
- [x] CPU: multires_tree.cpp の CoarsenLayers を層を使う世界だけ切り上げ
- [x] GPU: multires_graph.hlsl の WriteCoarsenedLayers を LayerGrainFloats() の時だけ切り上げ
- [x] 層を使わない世界はビット単位で同じ(gpu_multires_layers の既存の場面・multires_layers の既存の場面)
- [x] multires_layers の細かくする / 粗くするに層を使う世界の組(インライン・溢れ)・gpu_multires_layers の本物の鎖に層を使う世界の場面(CPU と HW/WARP がビット一致)
- [x] release 警告 0・archmap --check

## メモ・参考
- 切り上げても min で量に抑えるので 0 ≤ 層 ≤ 量 は保つ。子が全部層(粒が全部積もった)なら親も全部層(量の整数部)。
- 一様な子 8 つが同じ層の時(畳む・一様のまま粗くする)は和 ÷ 8 が割り切れるので切り上げても同じ(multires_tree.cpp 278 行・multires_tree.hlsl 288 行は触らない)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-12 wt2: チケットを作った。CPU(CoarsenLayers)と GPU(WriteCoarsenedLayers)に切り上げ・試験を足した(t-1324-coarsen-layer-round-up。t-1323 の上)。
- 2026-11-12 wt2: release 警告 0・archmap --check・multires_layers(層を使う世界の細かくする / 粗くする インライン・溢れ = 期待は切り上げを量で抑えた値)・
  dxil_alloca_check・world_layout・tree_refine_placement・gpu_multires_layers HW/WARP(本物の鎖・層を使う世界を足した)・gpu_multires_uniform・
  gpu_tree_refine_placement HW/WARP 合格。層を使わない世界の既存の場面は変わらず(記録したハッシュの取り直しは無し)。Done。
