# T-0845 本物の音速の投影の 1 刻み(V 2 回固定・外側 0)を GPU で通しに回して測る(設計の賭け 1 の通しの測り)

- Status: Done(測った。採るかは司令塔 → ユーザー)
- 種類: 工学(GPU)
- PC: 必須(ランナーで release のビルドと GPU の実行)
- 見積もり: チャット 1 回分(3 時間)
- 関係: T-0842(CPU の模型)・T-0843(V 2 回固定の解きの GPU・見込み)・T-0531・T-0606・T-0226・docs/design/07 §2.6

## 目的
T-0843 で V 2 回固定の解きだけ GPU で測り、式を作る(系の係数・右辺・多重格子の段の重み)・当てる・運ぶは見込みだった。
投影の 1 刻み(T-0842 の CPU の口: 音速 340・C = V/c²・V 2 固定・外側 0)を GPU で通しに回し、ns/セルと CPU とのビット一致を取る。目安 ≤ 60 ns/セル。

## 完了条件
- [x] 足りない GPU の段(系の係数・右辺・当てる・段の重み・運ぶ)を CPU の模型と整数で同じに足す。既定の道(陽解法の気体)は変えない(口は切ってある既定)
- [x] 箱 32³・64³・128³ で通しの ns/セルと内訳。T-0843 の見込み(64³ 21〜30)と比べる
- [x] 場面(泡・扉・爆発・孔のある壁・細孔の板)で 300 刻み、CPU とハッシュ一致(32³ は 300 刻み毎刻み、64³ は 30 刻み毎刻み、128³ は 2 刻み)
- [x] GpuGas::Create が 128³ で失敗する理由を調べて記録(小さく直した)
- [x] 目安 ≤ 60 ns/セル。外れたら最適化を 2 回まで(入ったが、測ってすぐ効く 2 つを入れた)
- [x] docs/perf.md に記録

## 足したもの(既定は切ってある。既定の道は前のまま)
- `GpuGasProjection`(engine/src/sim/gpu_gas_projection.{h,cpp})と `shaders/sim/gas_projection.hlsl`(gas_step.hlsl を include して面の両側・空き・帳簿の部品をそのまま使う):
  1 刻み = GasDerive → **GasProjectFace**(面: K_f と F*)→ **GasProjectCell**(セル: C_i・境界の K・右辺・段 0 の重み、初めの近似 = 前の解)
  → **GasProjectLevel**(段 1 から 1 段 1 Dispatch: 粗い段の容量・面の係数・重み・子の縮約の重み)→ GpuImplicit::RecordSolve(V 2 回固定)
  → **GasProjectPressure**(x を p̃ へ)→ GasFaceForce・GasApplyForce → **GasProjectFlow**(F = F* + K_f (x_左 − x_右)・抑える前の割合)
  → GasOutflow 〜 GasTransfer(陽解法の段 5〜9 そのまま)。t0 は投影の係数(GasPressureCoefficients)の設定。
- 段の形(節・隣・親子の番号)は CPU の BuildImplicitGrid の系を GpuImplicit に写し、値(容量・対角・重み・右辺)だけを毎刻み GPU が作る。
  気体の箱は全部のセルが同じレベルなので縮約はいつも ÷ 8、粗い段の行は 6 まで(Create・写す時に確かめる)。
- 式の部品を `gas_flow.hlsli` に移して CPU と共有: `GfPressureCapacity`・`GfShareCapacity`・`GfPressureFaceCoefficient`・`GfMulDivSigned`・`GF_PRESSURE_MIN_CAPACITY`。
  CPU 側に `GasPressureCoefficients`・`GasPressurePerSound`・`GasPressureLinkCoefficient` を公開(gas_pressure_system.h。値は前と同じ: 32³ の 300 刻みのハッシュが修正の前後で同じ)。
- `GpuGas::RecordProjectionPhase`(Derive / Force / Carry の段の組を、投影の通し番号と t0 で)・`Uav`・`SrvAddress`、`GpuImplicit::NodesBuffer`・`LinksBuffer`・`ListsBuffer`。
- 試験: `gpu_gas_projection_test --t0845 [--edge n] [--ticks n] [--check-every n] [--measure] [--default-shape]`。
  CPU の模型(StepGasProjection)と GPU を 1 刻みずつ進め、セル(HashGas)と帳簿を比べる。CPU の系の形が前の刻みと違えば、その刻みの前に形を写し直す。
  --measure は刻み 0 の形のまま、1 刻み目の数から V の記録の形を選び(ShapeFrom。--default-shape で既定の形)、暖機 12 刻みの後 16 刻みの部分ごとの最小と通しの最小・中央。

## 128³ で GpuGas::Create が失敗した理由(直した)
- 面の段の Dispatch が 1 次元で、128³ は面 12,582,912 ÷ 64 = 196,608 グループ > 65,535(D3D12 の 1 次元の上限)。Create の最初の検査
  「気体の箱が大きすぎる(面の段の Dispatch の上限)」で返っていた(gpu_gas_test は理由を出さず「計測の準備に失敗」とだけ出していた)。
- 直し: 面の段を 1 行 32,768 グループ(GAS_GPU_FACE_ROW_GROUPS)を超えたら 2 次元で投げ、シェーダーは面の番号 = x + y × 2^21(GasFaceThread)。
  1 行に収まる箱(64³ まで)は Dispatch も番号も前と同じ。セルの段の上限の検査も足した(セル ÷ 64 ≤ 65,535 = 128³ まで)。
- 面の件の番地(32bit)は 128³・成分 3 で 1.41 GB で収まる。256³ はセルの段の Dispatch と番地の両方で作れない(要るなら別チケット)。

## 測り方
- `job.py run -Preset release -Exe gpu_gas_projection_test -- --no-gbv --t0845 --edge 32 --ticks 300 --measure`(64³ は `--edge 64 --ticks 30`、128³ は `--edge 128 --ticks 2`)。
- 陽解法の 128³: `job.py run -Preset release -Exe gpu_gas_test -- --queue compute --profile --size 128 --ticks 2`(面の段の 2 次元の Dispatch の後に初めて作れた)。

## 結果(2026-10-19、RTX 3070 Ti、release、wt3 のランナー。ほかの作業ツリーと並走)
(下の表を参照。perf.md にも同じ)

## 結論
(下に書く)

## 残り(司令塔へ。採るなら別チケット)
- 段の形が変わる刻みを GPU で見つける形(今は試験が CPU の模型の系と比べて写し直す)。段の数は値(自分の重み < 1/2)で決まるので、
  GpuImplicitLevels(T-0134)のような GPU の段の組み立てを気体の箱の形に当てるか、気体の箱は段を最も粗いところまで固定で作るか。
  32³ の孔のある壁で 300 刻みに 1 回変わった(値はビット一致のまま)。
- 世界の気体の段(多重解像度・仮の世界)への組み込みと、外側の反復(T-0530)・版 2 の局所の小刻み(T-0842)の GPU 版は無い。
- T-0842 の 4 の「釣り合いの量のずれ」の根の研究は手つかず。

## 作業ログ
- 2026-10-19(wt3 の作業役): GpuGasProjection と gas_projection.hlsl を足し、32³ × 5 場面 × 300 刻みで初回から毎刻みビット一致。
  最適化 2 回(段の重みの行を子の隣 1 回の走査で集める・V の記録の形を ShapeFrom で)。面の段の 2 次元の Dispatch で 128³ の GpuGas を作れるようにした。
