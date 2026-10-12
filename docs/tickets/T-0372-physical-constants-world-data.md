# T-0372 物理定数を世界のデータに

- Status: Doing
- 種類: 研究(どこまでをデータにするか・整数の精度と GPU の費用)+ 工学(重力を通す)
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: M2 (docs/plan/ROADMAP.md)
- 設計: docs/design/13-scripting-game-model.md §2.6・04-numerics-determinism.md §2 / 決定: D-454・ADR-0151 の 5・ADR-0171(Proposed)

## 目的(1〜2 行)
エンジンの定数として埋まっている物理定数を洗い出し、「世界ごとに違ってよい値」(重力など)は世界のデータとして CPU と GPU が同じ値を読む形にする。
単位の選び方から決まる換算の定数(気体定数 R など)は器(エンジン)の 1 か所に寄せる。

## 完了条件(チェックできる形で)
- [x] 物理定数の洗い出し(下の表)
- [x] どこまでをデータにするかの案を ADR-0171(Proposed)に書く
- [x] 重力を世界のデータの 1 つの値(µm/s²)にし、物理(PxParameters::gravity)と気体(GasConfig → GasCoefficients → GasGpuParameters)が同じ換算で読む
- [x] 気体定数 R を units.hlsli の 1 か所に寄せる(gas_internal.h・reaction_table.cpp・lab_box.cpp・magic_patch.hlsli)
- [x] 標準の重力で今の結果が変わらない(CPU の試験・GPU の試験のビット一致・画像の試験)
- [ ] 重力を変えた世界の試験(無重力・別の重力で CPU と GPU がビット一致)— T-0423 に分けた
- [ ] 世界のパッケージ(Luau の環境の値)から重力を読む — T-0373(環境ミキサー)/T-0400 と合わせる(下の「T-0400 との関係」)

## 研究の打ち切り条件(研究のときだけ)
- R を世界ごとの値にする案は、「定数がシフト・除数の係数に入っている所が、値を変えても桁あふれ・精度落ちなく式を作り直せるか」を
  1 か所ずつ確かめる。PRESSURE_PER_AMOUNT_TEMPERATURE_Q32(R ÷ V の Q32)の 1 か所で、R を 0.1〜10 倍にしたとき 1 セルの
  物質量の上限(MAX_CELL_AMOUNT)・圧力の桁の約束(04 §2)を作り直す必要が出たら、R は器に残す(案 A)で打ち切る。
  → 今回の結論: 遊びへの効果が無い(R を変えた世界は温度・エネルギーの目盛りを変えた世界と同じ)ので、確かめる前に案 A をおすすめにした。

## 洗い出し(2026-10-12。engine/src・shaders を grep。エディタの表示用の double は除く)

| 定数 | 値 | 置き場所(変更前) | CPU/GPU | ベイクの式 | シェーダーの式 | 分類(案) |
|---|---|---|---|---|---|---|
| 重力 g(物理) | 9.80665 m/s² = 10283018 × 2^-20 | physics_solver.hlsli `PX_GRAVITY` | 両方(共通の hlsli) | なし | 入る(PxInitializeBody の g h²・ウォームスタートの割り算) | 世界のデータ(通した) |
| 重力 g(気体の浮力) | 9806650 µm/s² | gas_internal.h `GRAVITY_MICROMETER_PER_SECOND2` | CPU で係数に → GPU はパラメータ | 入る(gravityImpulse = g × 2^36 ÷ …) | 係数で入る | 世界のデータ(通した) |
| 層の重さの圧力 g ÷ A | 392266(× 10^-4 µPa/mg) | gas_internal.h・gas_cell.hlsli に別々 | 両方 | 入る(静水圧の基準の層) | 入る(HalfHydrostatic) | g から換算(通した) |
| 重さの仕事 g × Δx | 4903325(× 10^-6 µJ/mg) | gas_internal.h・gas_cell.hlsli に別々 | 両方 | なし | 入る(GravityWork。断熱) | g から換算(通した) |
| 気体定数 R(気体の γ) | 8314462618 nJ/(mol·K) | gas_internal.h | CPU(係数 soundScale へ) | 入る | 係数で入る | 器(寄せた) |
| 理想気体 R ÷ V(Q32) | 285682760 | gas_internal.h・gas_cell.hlsli に別々 | 両方 | 入る(静水圧の基準の物質量) | 入る(DeriveRaw の圧力・エネルギー) | 器(R とセルの体積から。static_assert で R と結んだ) |
| 気体定数 R(反応の速さ) | 同上 | reaction_table.cpp | CPU(ベイク) | 入る(アレニウスの表) | なし | 器(寄せた) |
| 気体定数 R(圧力 → 物質量) | 同上 | lab_box.cpp | CPU(実験室の組み立て) | 入る | なし | 器(寄せた) |
| 気体定数 R(転換の評価) | 同上 | magic_patch.hlsli `MP_GAS_CONSTANT_NANOJOULES` | 両方 | — | 入る(Ea ÷ RT) | 器(寄せた。1 行) |
| 気体定数 R(表の表示) | 8.314462618(double) | editor/reaction_table_panel.cpp | CPU(エディタ) | なし | なし | 器(エディタは浮動小数点でよい。そのまま) |
| 音速の基準 c̃ | 340 m/s | gas_reference.h `GasConfig::soundSpeedMmPerS` | CPU で係数に | 入る | 係数で入る | 環境の値 → T-0400 |
| 刻みの長さ・セルの大きさ・単位 | 1/60 s・0.5 m・µmol・mJ・mK | units.hlsli | 両方 | 入る | 入る | 器(ADR-0151 の 1) |

- 熱の放射(シュテファン=ボルツマン)・アボガドロ数・ボルツマン定数は、今のコードには無い(放射は T-0028 で足す時に、この表の決まりで置く)。
- 標準の大気の圧力・周囲の温度は呼ぶ側(probe_sim・実験室)が渡していて、エンジンの定数ではない(環境の値。T-0371・T-0400)。

## 通したもの(重力。2026-10-12)
- 新しい `shaders/common/world_physics.hlsli`(C++/HLSL 共通): 重力の単位(µm/s²・uint32)・上限 100 m/s²・換算
  `PxGravityFromMicrometers` / `GasHydrostaticFromMicrometers` / `GasGravityWorkFromMicrometers`(四捨五入。標準の重力で今までの定数とビット一致)・
  標準の値 `STANDARD_GRAVITY_MICROMETERS_PER_SECOND2`(標準パッケージが渡すまでの、ただ 1 つの置き場所)。
- 物理: `PxParameters::gravity`(64bit の欄。96 → 104 バイト。構造化バッファなのでルート定数は増えない)。`PxInitializeBody(body, rate, gravity)`。
  無重力では適応的なウォームスタートの割り算をしない(0 で割らない)。
- 気体: `GasConfig::gravityMicrometersPerSecond2` → `GasCoefficients::{gravityImpulse, hydrostatic, gravityWork}` → `GasGpuParameters`(176 → 192 バイト。t0 の構造化バッファ)。
  木(gas_tree)は基準の係数を写すので、レベルごとの式はそのまま。
- 精度: 換算の誤差は物理 ±0.5 × 2^-20 m/s²(5e-7 m/s²)、気体 ±0.5 × 10^-4 µPa/mg・±0.5 × 10^-6 µJ/mg。重さの力積の g × 2^36 が 64bit に収まるのは
  約 268 m/s² までなので、上限を 100 m/s² にした(MakeGasCoefficients の assert)。
- GPU の費用: 物理は同じ構造化バッファの 8 バイト、気体は 16 バイト増えるだけ。シェーダーの式は定数 → バッファの読み(同じ所から読んでいる)に変わるだけで、
  割り算の数は変わらない(物理の ÷ rate² は元から実行時の割り算)。

## T-0400(wt2)との関係
- T-0400 は音速の基準 340 m/s・基準の温度を環境の値にする。重力と同じ「世界ごとに違ってよい値」なので、パッケージの環境の値の読み込みは
  1 つの形(世界の環境の値 → GasConfig・PxParameters を作る所)にまとめる。ADR-0171 の決まり(単位は器・値は世界・換算は world_physics.hlsli)に従う。
- 音速の基準は「安定の条件(c̃ と小刻みの数)」の器の上限があるので、world_physics.hlsli に上限と換算を置くか、GasConfig の assert のままにするかは T-0400 で決める。

## 判断待ち(ユーザー)
- **決まった(2026-10-13・D-480)**: 案 A・ブランクの世界の重力は 0・上限 100 m/s² のまま。**続き: ブランクの世界の重力を 0 にする**(今は器の world_physics.hlsli の標準の値がブランクにもかかっている。サンプルの世界は標準の値を明示して渡し、結果を変えない)。
- ADR-0171 の案 A/B/C(R を世界ごとの値にするか)。おすすめ A(R は器)。仮(ユーザー未確認)で A で進めた。
- 重力の上限 100 m/s²(約 10 g)。もっと強い重力の世界が欲しいなら、気体の重さの力積の係数の桁を見直す(128bit で作る。費用は作る時だけ)。

## 分けたもの
- T-0423 重力を変えた世界の試験(無重力・月・別の重力で、物理・気体の CPU と GPU がビット一致。保存則の検査)

## メモ・参考
- 重力の向きは器が決める(物理は -y、気体は -z)。向きを世界のデータにする(横向きの重力など)のは今は考えない(D-454 の範囲外。要れば別のチケット)。
- 司令塔へ: ADR-0151 の 5 に「T-0372 の結論は ADR-0171」と 1 行追記するのはマージ後(wt2 が追記 1 を書いているので衝突を避けた)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-12(wt4・並走): 物理定数を洗い出し(重力 4 か所・R 5 か所〔うちエディタ 1〕・音速の基準・単位)、ADR-0171(Proposed。案 A = 重力は世界のデータ・R は器)を書いた。
  重力を world_physics.hlsli の単位と換算に通し、物理(PxParameters::gravity)と気体(GasConfig → GasCoefficients → GasGpuParameters)が同じ値から読む形にした。
  R は units.hlsli の 1 か所に寄せた(magic_patch.hlsli は定数の 1 行だけ)。標準の重力の換算は static_assert で前の定数と一致を確かめた。
  debug の CPU の試験 19 本(gas_reference・gas_tree・lab_box・physics_*・reaction_*・fixed)が通った。
  release の GPU(HW): gpu_gas・gpu_gas_tree・gpu_physics_wall・gpu_physics_stack_alternative・gpu_probe_physics・gpu_probe_physics_compute の 6 本が通った(CPU とビット一致)。
  続けて gpu_lab_box・gpu_probe_patch(R の 1 行)・image_probe_volume・image_probe_slice(画像が基準と一致)が通った。WARP の組(gpu_gas_warp など)は流していない。
  残り: T-0423(重力を変えた世界の試験)・パッケージの環境の値から重力を読む(T-0373/T-0400)・ADR-0171 のユーザーの判断。Status は Doing のまま(判断待ち)。
