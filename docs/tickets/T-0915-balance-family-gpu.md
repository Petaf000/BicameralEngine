# T-0915 C-0 の 2: 釣り合う家族の共通部品の GPU 版(Compute)と CPU とのビット一致

- Status: Todo
- 種類: 工学(GPU)
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列 1(C-0。T-0853 から分けた)
- 設計: docs/design/20-transport-substrate.md §2.7・§13.1 / ADR-0397

## 目的
T-0853 の CPU の部品(engine/src/sim/equilibrium・shaders/common/equilibrium.hlsli)を GPU の Compute で回し、陰の圧力の GPU 版(GpuGasProjection。T-0845)を部品の上へ移す。

## 完了条件
- [ ] shaders/sim/equilibrium_*.hlsl(Compute)が equilibrium.hlsli の同じ関数を呼ぶ: 対角・源の項 ÷ D・固定した面の項・面の値・残差の計器。V サイクルは GpuImplicit の段を使い回す
- [ ] GpuGasProjection の GasProjectCell(右辺)を部品の源の項・固定した面の形へ。T-0845 の 5 場面 × 300 刻み(32³)で、GPU が部品の上の CPU(gas_projection の equilibrium = true)と毎刻みビット一致
- [ ] 閉じた領域の GPU 版(種 → 長さの内の節の印 → 未知数の詰め〔キーで並べてからプレフィックス和。R5〕→ 外への面を固定した面に)。長さが世界を覆う時は閉じない解とビット一致・小さい長さで外の節がビット一致
- [ ] 保存(V 1・2・4)・2 回の実行と積み方を変えた実行でハッシュ一致
- [ ] 通ったら gas_projection の equilibrium を既定にし、前の道(Project の fixedCycles の枝)を消す
- [ ] 1 セルの費用を docs/perf.md へ(T-0845 の 32³ 32〜34・64³ 20〜23 ns/セルと比べる)

## メモ
- 段の形は今と同じく CPU の BuildImplicitGrid を写す(GPU で段を作るのは T-0918 の作業場の詰めと一緒に考える)。
- 司令塔(2026-10-21・M-1): V の回数は世界のデータで選べる形(GPU の記録の形 ShapeFrom も回数ごと)。

## 作業ログ
