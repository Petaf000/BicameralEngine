# ADR-0406 釣り合う家族の共通部品の GPU 版の形(段 0 は法則の段の同じスレッドで・粗い段から後は部品の段)

- Status: Accepted の所(部品と法則の段の分け方・作業用の語の並び・ビット一致の確かめ方)/ Proposed の所(閉じた領域の GPU 版の作り。T-0939)
- 日付: 2026-10-21
- 決めた人: Claude(wt2 の作業役。T-0915)。設計の芯は ADR-0397(部品の口)・D-541・ADR-0373 §2(ユーザー決定)
- 関係: ADR-0397(部品の口・CPU)・T-0845(陰の圧力の GPU)・T-0531(一般の右辺の V サイクル)・ADR-0019(熱の陰解法の多重格子)
- 設計書: docs/design/20-transport-substrate.md §9・§13.1

## 決定
1. **段 0 は法則の段が同じスレッドで仕上げる**: 法則の段(陰の圧力なら GasProjectCell)は節 1 つにつき 1 スレッドで、自分だけの係数
   (C_i + Σ 値を固定した面の係数)と隣との面の係数(6 まで)を作り、`EqGpuFinishNode`(対角・自分の重み・隣の重み・面の係数を部品の作業用の語へ)と
   `EqGpuSetRhs`(右辺 = Σ EqSourceTerm と温めた始まり)を呼ぶ(shaders/sim/equilibrium_core.hlsli)。系を一度バッファに書いてから部品が読む形にしない
   (節・面・源の一覧を VRAM に持つと、1 セルあたり源 2 本 × 24 B・面 3 本 × 32 B が増え、T-0918 の詰めと逆になる)。
   CPU の部品(EquilibriumSystem を作って SolveEquilibrium)とは、式の関数(common/equilibrium.hlsli・implicit_conduction.hlsli)と和の順が同じなのでビット一致。
2. **粗い段から後は部品の段**: 粗い段の重み(EquilibriumLevel。段 1 から 1 段 1 Dispatch)→ V サイクル(GpuImplicit::RecordSolve の fixedCycles。
   決まった回数)→ 残差の計器(EquilibriumMeter。任意)。解いた値は作業場の値 A で、法則の当てる段が読む(面の値 F = g (x_a − x_b) は法則の当てる段が作る。
   陰の圧力は GasProjectFlow)。
3. **作業用の 128bit の語の並びは common/equilibrium.hlsli の EqGpu*Word**(容量・対角・面の係数・残差の計器)。CPU と GPU のどちらからも番地を同じ関数で出す。
4. **ビット一致の確かめ方**: CPU の部品の上の道(GasProjectionOptions::equilibrium)と毎刻み、箱・帳簿・残差の計器を比べる(gpu_gas_projection_test --t0915)。
   前の GPU の道(T-0845)とは 32³ × 5 場面 × 300 刻みの最後のハッシュで比べる(前の道は消したので、2026-10-21 に前の道で取った値を試験に書いた)。
5. 今の GPU 版は**節がどれも同じレベルの系**(気体の箱。縮約はいつも親のセル・粗い段の行は 6 まで。`GpuEquilibrium::Supports`)と**閉じない解き**だけ。
6. (Proposed)**閉じた領域の GPU 版は T-0939 に分ける**: 種 → 長さの内の節の印 → 未知数の詰め(キーで並べてからプレフィックス和。R5)→ 外への面を固定した面に。
   段の形が刻みごとに変わるので、GPU で段を作る形(GpuImplicitLevels の形を部品の系に当てる)と一緒に作るのが要る。CPU の段の形を毎刻み写す形では、
   閉じた領域の得(解く節が少ない)が写しの費用で消える(未確認。T-0939 で測る)。

## 理由
- 部品の口(ADR-0397)は「法則は式の作りだけを書く」。GPU で式の作りを書く場所は法則の段なので、同じスレッドで部品の関数を呼べば、
  系の一覧を VRAM に持たずに済み、法則ごとの段の数も増えない(陰の圧力は前と同じ段の数から GasProjectLevel が部品へ移っただけ)。
- 支え・電位・磁場・案 B の液体も同じ形(自分の段で C_i と面の係数を作り EqGpuFinishNode を呼ぶ)で入る。

## 影響
- GpuGasProjection は GpuEquilibrium の上(gas_projection.hlsl の GasProjectLevel は部品の EquilibriumLevel へ移った)。値は前の GPU の道と同じ(T-0915)。
- 陰の圧力の CPU の既定は部品の道(GasProjectionOptions::equilibrium = true)。前の道(Project の fixedCycles・外側 1 の枝)は消した。
