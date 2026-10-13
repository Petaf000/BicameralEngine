# T-0423 重力を変えた世界の試験

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: D-471 ②(中身の器。docs/plan/ROADMAP.md)
- 設計: docs/decisions/ADR-0171・08・07 §2 / 決定: D-455・D-480

## 目的(1〜2 行)
重力は世界のデータ(D-480)。ブランクの世界の重力 0・月・上限 100 m/s² の世界で、物理と気体が CPU と GPU でビット一致し、
保存則が成り立ち、重力に応じた向きのふるまい(無重力で動かない・落ちる距離と層の圧力の差が重力に比例)をすることを確かめる。

## 完了条件(チェックできる形で)
- [x] 器の既定の重力を 0 にする(PxDefaultParameters・GasConfig)。ブランクの世界は 0、サンプルの世界は標準の値を明示して渡す(sim::WorldPhysics。T-0372 の続き)
- [x] 標準の重力の試験の結果が変わらない(CPU・GPU・画像・窓の再生)
- [x] 物理の CPU: 無重力で浮いた物・積んだ物が動かない(ビット単位)/ 落ちる距離が重力に比例・½gt² に近い / 上限の重力で桁あふれしない(physics_gravity・physics_gravity_checked)
- [x] 物理の GPU と CPU のビット一致: 無重力の積み木・月の山・上限の積み木(gpu_physics_stack_zero_gravity・gpu_physics_pile_moon・gpu_physics_stack_max_gravity)
- [x] 気体の CPU: 層の圧力の差が重力に比例(無重力で 0)/ どの重力でも静止大気が動かない / 閉じた箱の保存則(無重力では重さの仕事の帳簿が 0)(gas_reference)
- [x] 気体の GPU と CPU のビット一致: closed_zero_gravity・closed_moon・closed_max_gravity・static_moon・static_max_gravity・bubble_zero_gravity(無重力で熱い泡が上がらない)(gpu_gas)

## メモ・参考
- 重力の値の置き場所: 単位・上限・換算は shaders/common/world_physics.hlsli、世界の値は engine/src/sim/world_physics.h の WorldPhysics
  (既定 0 = ブランク。StandardWorldPhysics() = 地球。標準パッケージ T-0373 が渡すまでの置き場所)。
- 窓の世界: frame_loop.cpp の WorldPhysicsOf(ブランク → 0・サンプル → 標準)。ブランクの世界には今は物も気体も無いので、見た目は変わらない。
- 気体の木(gas_tree・gpu_gas_tree)は基準の係数を写すだけなので、試験は 1 レベルの箱(gas_reference・gpu_gas)で見る。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-13: 器の既定の重力を 0 にし、世界の値を sim::WorldPhysics にまとめた(T-0372 の続き)。physics_test の gravity・gpu_physics_test の --gravity(3 本)・
  gas_reference の重力の節・gpu_gas の 6 場面を足した。release で全部通過(物理: 月 ÷ 地球の落下 0.16519 = 重力の比・地球の 1 s の落下 4.985 m〔½gt² 4.903 m。陰的な積分の分〕・
  上限の積み木の食い込み 3.2 mm で止まる / 気体: 無重力の閉じた箱の重さの仕事 0・上限で −70 J・どの重力でも静止大気の速さ 0)。
  直したこと: 1 層の圧力の差の比は µmol の丸めで 0.6% ずれたので箱の高さ全部の差で比べた。無重力の泡は「少しでも熱いセルの重心」が音の波で上に寄るので、泡の芯(+5 K 以上)で見た。
