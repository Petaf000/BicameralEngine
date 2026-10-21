# T-0915 C-0 の 2: 釣り合う家族の共通部品の GPU 版(Compute)と CPU とのビット一致

- Status: Done(閉じた領域の GPU 版は T-0939 に分けた)
- 種類: 工学(GPU)
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: 試作の列 1(C-0。T-0853 から分けた)
- 設計: docs/design/20-transport-substrate.md §2.7・§13.1 / ADR-0397 / この作りの決定: ADR-0406

## 目的
T-0853 の CPU の部品(engine/src/sim/equilibrium・shaders/common/equilibrium.hlsli)を GPU の Compute で回し、陰の圧力の GPU 版(GpuGasProjection。T-0845)を部品の上へ移す。

## 完了条件
- [x] shaders/sim/equilibrium.hlsl・equilibrium_core.hlsli(Compute)が equilibrium.hlsli の同じ関数を呼ぶ: 対角・源の項 ÷ D(EqSourceTerm)・固定した面の項・残差の計器(EqResidualMagnitude)。V サイクルは GpuImplicit の段を使い回す。
      面の値 F は法則の当てる段が作る(陰の圧力は GasProjectFlow。CPU も faceValues = false で同じ)
- [x] GpuGasProjection の GasProjectCell(右辺)を部品の源の項・固定した面の形へ。T-0845 の 5 場面 × 300 刻み(32³)で、GPU が部品の上の CPU(gas_projection の equilibrium = true)と毎刻みビット一致(箱・帳簿・残差の計器)
- [ ] 閉じた領域の GPU 版 → **T-0939 に分けた**(ADR-0406 の 6。段の形が刻みごとに変わるので GPU で段を作る形と一緒に)
- [x] 保存(V 1・2・4)・2 回の実行と積み方を変えた実行(1 本のリストに 8 刻み・段ごとの Dispatch 2 段まで)でハッシュ一致
- [x] 通ったら gas_projection の equilibrium を既定にし、前の道(Project の fixedCycles の枝)を消す
- [x] 1 セルの費用を docs/perf.md へ(T-0845 の 32³ 32〜34・64³ 20〜23 ns/セルと比べる)

## メモ
- 段の形は今と同じく CPU の BuildImplicitGrid を写す(GPU で段を作るのは T-0918 の作業場の詰めと一緒に考える)。
- 司令塔(2026-10-21・M-1): V の回数は世界のデータで選べる形(GPU の記録の形 ShapeFrom も回数ごと)。

## 結果(2026-10-21、release、wt2 のランナー。`job.py run -Preset release -Exe gpu_gas_projection_test -- --no-gbv --queue compute --t0915 --ticks 300`)
| 場面(32³・340 m/s・V 2・外側 0) | CPU の部品と | 前の GPU の道(T-0845)と | 保存 | 段・写し直し | 残差の計器の最大(mg。GPU = CPU) | ハッシュ |
|---|---|---|---|---|---|---|
| 泡 | 300 刻み毎刻みビット一致 | 一致 | 0 | 6・0 | 0 | a4b6928b6f05e940 |
| 扉 | 同 | 一致 | 0 | 6・0 | 91 | 884686f097eafbb2 |
| 爆発 | 同 | 一致 | 0 | 6・0 | 29 | 04811f98601a371d |
| 孔のある壁 | 同 | 一致 | 0 | 6・1 | 6,967,454 | d53f653207a2cfb6 |
| 細孔の板 | 同 | 一致 | 0 | 6・0 | 208 | 4b9e31f213adcd63 |
- 前の GPU の道のハッシュは、消す前に同じ日に前の道で 5 場面 × 300 刻みを回して取った(T-0853 の表と同じ値)。試験に書いてある(T0845_HASHES_32)。
- 保存: 16³ の爆発・孔のある壁 × V 1・2・4 × 60 刻み、毎刻み CPU とビット一致・保存 0。
- 決定性: 16³ × 5 場面 × 120 刻みを 2 回 + 積み方を変えて(1 本のリストに 8 刻み・V の記録の段ごとの Dispatch 2 段まで。リスト 15 本)、ハッシュが同じ。
- ctest: `gpu_gas_projection_t0915`(上と同じ。上限 1800 秒)を足した。
- 時間と VRAM(docs/perf.md。`--t0845 --edge n --ticks 2 --measure`。RTX 3070 Ti): 通し 32³ 32.1〜34.0・64³ 19.8〜23.0・128³ 18.2〜21.9 ns/セル = T-0845 の前の道と同じ
  (段の重みが部品へ移っただけで段の数は同じ)。VRAM は気体の箱 + 部品で 2.28〜2.34 KB/セル、うち部品 1.42〜1.48 KB/セル(T-0918 へ)。

## 作ったもの
- `GpuEquilibrium`(engine/src/sim/gpu_equilibrium.{h,cpp}): 段の形を写す・粗い段(EquilibriumLevel)・V(GpuImplicit::RecordSolve の fixedCycles)・残差の計器(EquilibriumMeter)・VRAM。
- `shaders/sim/equilibrium_core.hlsli`: 法則の段が同じスレッドで呼ぶ `EqGpuFinishNode`(対角・重み・面の係数)・`EqGpuSetRhs`・`EqGpuValue`。
- `shaders/common/equilibrium.hlsli`: 作業用の語の並び(EqGpu*Word。CPU と GPU で同じ関数)。
- gas_projection.hlsl: GasProjectCell は式の作り(C_i・境の K・面の K・E_i と −Σ F*)だけになり、部品の関数で仕上げる。GasProjectLevel は部品へ移した(消した)。
- gas_projection: `GasProjectionOptions::equilibrium` を既定 true。前の道の fixedCycles・外側 1 の枝を消した(外側の反復の道 = equilibrium = false か fixedCycles でない時は前のまま)。
  部品の道でも `cost.unconverged` を前の道と同じ数え方で数える(probe_gas の報告が変わらないように)。
- `GpuImplicit::DeviceBytes`・`GpuGasProjection::DeviceBytes`(T-0918 の測り)。

## 仮(ユーザー未確認)の決め
- 段 0 は法則の段が同じスレッドで仕上げる(系の一覧を VRAM に持たない)。ADR-0406 の 1。
- 部品の GPU 版は今は同じレベルの節・閉じない解きだけ(`GpuEquilibrium::Supports`)。違うレベルの節(木のセル)・閉じた領域は T-0939。

## 分けたもの
- **T-0939**: 閉じた領域の GPU 版(種 → 長さの内の印 → 未知数の詰め → 外への面を固定した面に)と、違うレベルの節(木のセル)の GPU 版。GPU で段を作る形と一緒に。
- 作業場の詰め(上限 2 倍・作業用の語)は T-0918 のまま(下の測りを足した)。

## 作業ログ
- 2026-10-21(wt2 の作業役): t-0853-c0 から t-0915-c0-gpu。前の GPU の道で 32³ × 5 場面 × 300 刻みのハッシュを取ってから、部品の GPU 版を書き、陰の圧力の GPU を部品の上へ。
  初回で 5 場面 × 300 刻み毎刻み CPU の部品とビット一致・前の GPU の道のハッシュと一致・保存・決定性。既定を部品の道へ替え、前の道の fixedCycles の枝を消した。
