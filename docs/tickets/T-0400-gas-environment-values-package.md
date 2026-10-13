# T-0400 気体の環境の値(音速の基準・基準の温度)をパッケージへ

- Status: Doing
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: D-471 ② 中身の器(docs/plan/ROADMAP.md。T-0371 から分けた)
- 2026-10-13 に新しい指針に合わせて見直した: エンジンは中身を持たない(D-454・D-464)。サンプル「現実」は ⑤ の場面に要る分だけ(D-471 ②)。
  中身は作り手と同じやり方(Luau のパッケージ)で書く(D-486)。ブランクの世界(D-455)では何も読まずに動く。物理定数は D-480・ADR-0171(Accepted)の形に揃える
- 設計: docs/design/13-scripting-game-model.md §2.6・14 §2 / 決定: D-454〜D-456・ADR-0151(追記 1)・ADR-0171・ADR-0176(Proposed)

## 目的(1〜2 行)
sim::GasOptions の音速の基準 340 m/s(D-440)・地面の層の基準の温度などの「環境」の値を、世界のパッケージの環境の値から読む(T-0372 の物理定数と合わせて決める)。

## 完了条件(チェックできる形で)
- [x] どの値を環境(パッケージ)に、どれを器(エンジン)に残すかを決める(→ ADR-0176。Proposed・案 A を仮で。ADR-0151 の 5。T-0372 は済み: 重力は sim::WorldPhysics、R は器〔D-480・ADR-0171〕。同じ形に揃える)
- [x] 気体の試験(gas_reference・gpu_gas)の結果が変わらない(release・HW で通過)
- [x] 今の試験(--world sample・サンプルのパッケージを読む試験)の結果(ハッシュ・画像)が変わらない(作業ログ)

## 洗い出し(2026-10-13。engine/src・shaders・tests を grep)

| 値 | 置き場所(変更前) | 中身か | 分類 | 行き先 |
|---|---|---|---|---|
| 音速の基準 c̃ 340 m/s | gas_reference.h `GasConfig::soundSpeedMmPerS` の既定 | 中身(D-440 の地球の空気の値) | 環境の値 | `WorldPhysics::soundSpeedMillimetersPerSecond`・分類 environment の `sound_speed_mm_per_s`(通した) |
| 音速の基準の上限(ν ≤ 0.6 → 小刻み × 18 m/s) | gas_reference.cpp の assert | 器(安定の条件) | 器 | `GasSoundSpeedLimitMmPerS`(読み込みの範囲の検査にも使う) |
| 地面の層の基準の温度 | 定数は無い。`MakeHydrostaticReference`・`MakeGasTree` の引数(呼び手 = 試験の 293.15 K)から `soundTemperature` を導く | 呼び手の値 | 環境(基準の大気)から導く値 | 基準の大気(成分・温度・地面の圧力)と一緒に T-0373 で environment へ(ADR-0176 の 3) |
| 熱いセルの頭打ちの温度 `soundCapTemperature`(0 = 地面の層の温度) | gas_reference.h | 器(安定の決まり。Q32 の仮の案 A) | 器 | そのまま |
| 小刻みの数 24 | `GasConfig::substeps` | 器 | 器 | そのまま |
| 重力 | `WorldPhysics`(T-0372) | — | 環境の値 | 分類 environment の `gravity_um_per_s2`(通した) |
| 試験の大気(293.15 K・101325 Pa・N2/O2 の割合) | tests/gas_*_test.cpp | 試験の場面 | — | 試験のまま(エンジンの外) |
| サンプルの世界の空気・300 K | sim/probe_sim.cpp | 中身 | 世界の組み立て | T-0398(wt2) |

## 決めたこと(ADR-0176。案 A を仮で)
- 音速の基準は環境の値(ブランクは 0 = 無い。器の既定も 0)。単位・上限・換算は器。基準の温度は新しい値にせず、基準の大気から導く(大気は T-0373)。
- 読み込みは T-0398 の共有の決め事に従う: 反応表と同じ読み込みの分類 `environment`(1 つの鍵 = 1 つの値)。読む所 `script/environment_package.{h,cpp}`、
  型 `data/types/bicameral.d.luau` の `Environment`、サンプルの値 `data/packages/combustion_test/environment.luau`。
  つなぎ目: `ReadReactionTableDefinition` → `ReactionTableDefinition::environment` → `BakeReactionTable` → `BakedReactionTable::environment`
  (wt2 のコミット「パッケージの欄の道具を分け…」を cherry-pick して、印の節に 1 行ずつ)。窓は `reactionTable.environment` で物理の重力を作る
  (`WorldPhysicsOf`〔世界の種類で決めていた〕は無くした)。ブランクの表・再生の作り直しも同じ道を通る。

## 判断待ち(ユーザー)
- **音速の基準を作り手が書くか、エンジンが大気から導くか**(ADR-0176 の案 A / B)。
  - A(仮で実装): 気体を使う世界は環境に `sound_speed_mm_per_s` を書く。書かないと気体の箱が作れない。大気と食い違う値を書いても黙って通り、書いた速さで圧力の波が進む。
  - B: 書かなければ、その世界の地面の大気(成分・温度)から本物の音速を導く(書けば上書き)。大気を変えると自動で追従し、書き間違いが無い。
    代わりに今の試験の気体の結果が少し変わる(340 → 343.7 m/s)、軽い・熱い大気では小刻みを増やす(費用が増える)か誤りにする決まりが要る。
  - おすすめ: 今は A(結果を変えない・取り消しやすい)。気体をサンプルの世界に載せる時(T-0210)に B を足すかを決める(T-0443)。

## 分けたもの
- T-0443(候補。案 B を選んだら): 音速の基準を書かない世界は、地面の基準の大気から本物の音速を導く。
- T-0444: 環境の値のホットリロード(表の差し替えで重力・音速の基準が変わったら、刻みの境界で物理と気体の設定を作り直す)。

## メモ・参考
- 洗い出しの一覧は T-0371 のチケット。合わせた表の版(TableVersion)は全部の分類を含むので、反応表と同じ読み込みに新しい分類を足すと
  表の版が変わる。世界の組み立ては別の読み込み(または版に入れない分類)にするかをここで決める。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-13(wt4・並走): 洗い出し(中身として埋まっていたのは音速の基準 340 m/s だけ。基準の温度は呼び手が渡す基準の大気から導く値)と ADR-0176(Proposed)。
  音速の基準を `WorldPhysics` に足し(ブランク 0・器の既定 0・上限 `GasSoundSpeedLimitMmPerS`)、分類 `environment` を読む `script/environment_package`・
  型 `Environment`・`combustion_test/environment.luau`(地球の値)を足した。T-0398 の共有の決め事に従い、wt2 の欄の道具のコミットを cherry-pick して
  ベイクした表の `environment` に通し、窓は表の環境で物理の重力を作る(`WorldPhysicsOf` を無くした)。試験 environment_package(新)。
  debug: environment_package・reaction_package・luau_type_check・reaction_replay_table・reaction_hot_reload・reaction_table_edit・replay_file・blank_world・
  gas_reference・gas_tree・gpu_gas_tree が通過。release(警告 0): environment_package・gas_reference・gpu_gas(HW)・window_replay_*(6 本)・
  window_lab・window_lab_compare・window_place_*・window_patch_*・window_hot_reload_game_* が通過。image_*(画像が基準と一致)・
  `run -- --world sample --check-physics --frames 240`(積み木の CPU と GPU の物のハッシュの食い違い 0)も通過。Status は Doing のまま(ADR-0176 のユーザーの判断待ち)。
- 2026-10-13 その 2(wt4): main(1ef520b。t-0398・t-0411・t-0426 など)へ載せ直した。cherry-pick の分は main の 073eca1 に任せて落とした。つなぎ目は wt2 の行と両方残し、
  窓は t-0398 の組み立て(MakeLayoutPhysicsScene・HasStackScene(表))の上で、重力を表の environment から作る形にした。release(警告 0)で
  environment_package・reaction_package・luau_type_check・reaction_replay_table・reaction_hot_reload・replay_file・blank_world・world_layout・gas_reference・
  gpu_gas・gpu_gas_flow・window_replay_*・window_place_*・window_hot_reload_game_* と `--world sample --check-physics`(食い違い 0)が通過。archmap --check も通過。
