# T-0099 物理: 島の方式のコードを消す

- Status: Todo(2026-10-03 ユーザー決定: T-0095 の後、T-0018 の前)
- 種類: 工学(片付け。結果は変えない)
- PC: 必須
- 見積もり: チャット 1 回分より小さい
- マイルストーン: M2(docs/plan/ROADMAP.md)
- 設計: docs/design/08-bodies-physics.md §6「結果(T-0094)」「結果(T-0095)」/ ADR-0002「計測(T-0094)」

## 目的(1〜2 行)
島ごとに 1 グループで解く方式(T-0094。既定 Off)は、T-0095 の後に測り直しても今の方式より遅い(山 5.53 ms 対 2.57 ms)ので消す。
コードは git の履歴(f3f1aed まで)に、測った数は ADR-0002・perf.md に残る。

## 完了条件(チェックできる形で)
- [ ] shaders/sim/physics_islands.hlsli・SolveIslands の .cso(shaders/CMakeLists.txt)・`GpuPhysicsOptions::islands` とその周り(gpu_physics.cpp・gpu_physics_islands.cpp・FindIslands・ReadIslandLabels)・テスト(`*_islands*`)・gpu_physics_test の島の引数を消す
- [ ] 使わない色を述語で飛ばす(skipEmptyColors・u15)は残す。u13・u14 とルート定数の islandMode・islandBodyLimit を消して詰める(HANDOFF の「物理のルート署名」の数も直す)
- [ ] CPU の島の印(`PhysicsWorld::IslandLabels`)は、ほかで使っていなければ消す(使っていれば残す理由を書く)
- [ ] build(debug / release)・tidy 警告なし・archmap OK(map.yaml の島のノードを消す)・全部の gpu_physics_*・gpu_probe_physics* が CPU とビット一致のまま・1 刻みの時間が変わらない(山 2.57 ms 前後)
- [ ] 08 §6・ADR-0002 に「消した(T-0099)」と 1 行

## メモ・参考
- 6×6 のウェーブの解(physics_solve6_wave.hlsli)は SOLVE_LANES・SOLVE_SUBGROUPS(島の方式の -D)に頼っている。島を消したら 64 / 1 に固定して整理する。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
