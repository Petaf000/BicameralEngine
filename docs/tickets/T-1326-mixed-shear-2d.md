# T-1326 2 次元の支えの違う大きさの面の切り口のずれを 3 次元と同じ分け方に

- Status: Done(2026-11-12 wt3)
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: 試作の列 3(R-B1・R-B2。T-1325 の「分かったこと」から)
- 設計: docs/design/22-support-law.md §3.3 / 決定: D-695(支えは今は 2 次元のまま・3 次元の割れの進み T-1339 の時に置き換える)・ADR-0519・T-1191

## 目的(1〜2 行)
2 次元の MixedForces(sim/support_law.cpp)は、対(④)の伸びの 3 つの節の項のうち自分の項の自分の細かいセルの勾配の分だけを足していた。
対の相手の項が自分の細かいセルを引く分も足し、3 次元の MixedForces(T-1325)と同じ「細かいセルが受ける力の全部」の読みにする(司令塔の仮)。

## 完了条件(チェックできる形で)
- [x] 奥行 1 セルの混ざる場面(Extruded 7 場面 × None / 形 A)で、2 次元の FineFaceForces のずれが 3 次元の奥行 2 つの面の和と面ごとに同じ(最大の力の 0.1 %)・粗いセルの面ごとの和も同じ(support_3d_mixed_test 2)
- [x] 上の段だけ 0.25 m の梁の上の段の塊の x の釣り合い(切る面の軸の和 + 境の切り口のずれの和 = 0)が 2 次元の読みでも 10^-4 以内(support_3d_mixed_test 4)
- [x] 系・答え・ハッシュは変わらない(面の力の読みだけ)。support_law・support_mixed・support_mixed_neighbor_strain・support_3d・support_3d_mixed・support_cost・support_fracture_refine 合格(割れの試験の期待は取り直し不要だった: support_fracture_refine はそのまま合格)
- [x] GPU gpu_equilibrium_block HW/WARP(GPU は面の力を読まないので変わらない見込みの確かめ。HW 15 秒・WARP 133 秒で CPU とビット一致)
- [x] release 警告 0・archmap --check

## メモ・参考
- 対の伸びの項(AddMixedPair): TripleTerm の nodes = {粗いセル C, その項の細かいセル, 対の相手}。勾配 1 = 3 g_軸 + mix g_ずれ・勾配 2 = mix g_ずれ(相手の細かいセルを引く分)。
- 直す前: 細かいセルが +軸の側なら自分の項の勾配 1 だけ、−軸の側なら自分の項の粗いセルの勾配(= −(勾配 1 + 勾配 2))を読んでいた → 対の 2 つの面で読み方が違い、
  どちらの向きでも相手の項の分が抜けていた(根の面で +13.2 / −38.7 × 10^6 mN。3 次元は −12.7 × 2)。
- 直した後: 自分の細かいセルの勾配で、自分の項の勾配 1 + 相手の項の勾配 2 を toFace(細かいセルが +軸の側なら +1)を掛けて足す(ずれの負の項・歪みの差の項と同じ形)。
  対の 2 つの面の和 = 粗いセルが受ける力。エネルギーは自分の項のまま。曲げの回りには入らない(面の点の力。前と同じ)。
- 触ったファイル: engine/src/sim/support_law.cpp(MixedForces)・tests/support_3d_mixed_test.cpp(比べの期待)。GPU のシェーダーは面の力を持たない(equilibrium_block は解くだけ)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-11-12(wt3。ブランチ t-1326-mixed-shear-2d・t-1325-face-cut-3d 463cb02a の上): MixedForces の伸びの項の読みを直し、support_3d_mixed_test の 2 次元との比べにずれ(面ごと・粗いセルの面の和)と 2 次元の釣り合いを足した。ビルドと試験はこの後。
- 2026-11-12(wt3 続き): release 警告 0。CPU 合格 = support_law(626 秒)・support_3d_mixed・support_mixed・support_mixed_neighbor_strain・support_3d・support_cost・support_fracture_refine・float_check 5 本・dxil_alloca_check。
  2 次元と 3 次元の切り口の差(軸・ずれ・縁。面ごと)= 7 場面 × 2 形で 6.6 × 10^-8 〜 4.0 × 10^-7(最大の力に対して)・粗いセルの面ごとのずれの和の差 ≤ 4.0 × 10^-7(直す前は 2〜13 %)。
  上の段の塊の 2 次元の釣り合いの残り 1.10 × 10^-7(None)・7.83 × 10^-8(形 A)。
  GPU gpu_equilibrium_block HW・WARP 合格(ハッシュは変わらない)・archmap --check OK。
