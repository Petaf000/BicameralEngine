# T-0099 物理: 島の方式のコードを消す

- Status: Done(2026-10-03)
- 種類: 工学(片付け。結果は変えない)
- PC: 必須
- 見積もり: チャット 1 回分より小さい
- マイルストーン: M2(docs/plan/ROADMAP.md)
- 設計: docs/design/08-bodies-physics.md §6「結果(T-0094)」「結果(T-0095)」/ ADR-0002「計測(T-0094)」

## 目的(1〜2 行)
島ごとに 1 グループで解く方式(T-0094。既定 Off)は、T-0095 の後に測り直しても今の方式より遅い(山 5.53 ms 対 2.57 ms)ので消す。
コードは git の履歴(f3f1aed まで)に、測った数は ADR-0002・perf.md に残る。

## 完了条件(チェックできる形で)
- [x] shaders/sim/physics_islands.hlsli・SolveIslands の .cso(shaders/CMakeLists.txt)・`GpuPhysicsOptions::islands` とその周り(gpu_physics.cpp・gpu_physics_islands.cpp・FindIslands・ReadIslandLabels)・テスト(`*_islands*`)・gpu_physics_test の島の引数を消す
- [x] 使わない色を述語で飛ばす(skipEmptyColors・u15)は残す。u13・u14 とルート定数の islandMode・islandBodyLimit を消して詰める(HANDOFF の「物理のルート署名」の数も直す)
- [x] CPU の島の印(`PhysicsWorld::IslandLabels`)は、ほかで使っていなければ消す(使っていれば残す理由を書く)
- [x] build(debug / release)・tidy 警告なし・archmap OK(map.yaml の島のノードを消す)・全部の gpu_physics_*・gpu_probe_physics* が CPU とビット一致のまま・1 刻みの時間が変わらない(山 2.57 ms 前後)
- [x] 08 §6・ADR-0002 に「消した(T-0099)」と 1 行

## メモ・参考
- 6×6 のウェーブの解(physics_solve6_wave.hlsli)は SOLVE_LANES・SOLVE_SUBGROUPS(島の方式の -D)に頼っている。島を消したら 64 / 1 に固定して整理する。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-03: physics_islands.hlsli・gpu_physics_islands.cpp・SolveIslands/FindIslands の .cso・`GpuPhysicsOptions::islands` 一式・`ReadIslandLabels`・CPU の `PhysicsWorld::IslandLabels`(テストだけが使っていた)・`*_islands*` のテスト・map.yaml のノードを消した。
  述語は u15 → u13、ルート定数 16 → 14(48 / 64 語)。6×6 の和は 1 グループ = 1 物に固定(SOLVE_LANES・SOLVE_SUBGROUPS・組の引数を消し、AddToSum の「ウェーブが組にまたがる」分岐も不要に)。
  壁の場面は島のテストでしか走っていなかったので gpu_physics_wall(120 刻み、約 56 s)を足した。RecordIterations が tidy の関数の大きさを超えたので RecordSolveColors に分けた。
  確認: build debug/release・tidy 警告なし・archmap OK(92)・gpu_physics_*(7)・gpu_probe_physics*(3)・physics_*/fixed/float_check(14)・debug の gpu_physics_stack が通る。
  時間: 今日の PC は全体に遅く(変更前の HEAD を stash して同じ条件で 山 4.23 ms)、変更後 4.28〜4.31 ms で差は揺れの範囲(perf.md)。
