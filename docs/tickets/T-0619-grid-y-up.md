# T-0619 格子の y を上にそろえる(物理の対応・描画・筆・試験)

- Status: Todo
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP の T-0210(T-0592 から分けた。T-0590 の判断待ち 1)
- 設計: docs/design/07-transport.md §2.7・08 / 決定: ADR-0256 の 5(仮 = 案 A)

## 目的(1〜2 行)
場面のパッケージの world_cells と気体(probe_gas・GPU の区画)は「格子の y が上」、物理の対応(probe_sim.hlsli の ProbeCellOfPhysicsPoint)と
描画(debug_camera の worldUp = −y・probe_view の物の置き方)は「格子の y が下向き」で食い違っている。案 A(仮・ユーザー未確認)で y を上にそろえる。

## 完了条件(チェックできる形で)
- [ ] probe_sim.hlsli: 格子 y = 物理 y / 0.5 m(裏返さない)。ProbeCellOfPhysicsPoint・コメント(物理の地面の上の面が格子の y = 0)
- [ ] 描画: debug_camera の worldUp = +y・probe_view の物の置き方(y を裏返さない・回転の符号)・BODY_LIGHT・frame_loop の押す光線(QueuePushes・autoPush)
- [ ] 筆(brush_panel・brush_preview)と場面の試験(physics_heat_test の y の範囲など)を合わせる
- [ ] 燃える木箱の煙が窓で上へ上がる画像(区画をアプリで載せる道は T-0620)
- [ ] release のビルド・gpu_probe_physics・physics_heat・brush_preview・gpu_probe_gas

## メモ・参考
- 作り手から見た違い(案 A): 今書いた場面(world_cells の y が上)がそのまま上下正しく見え、気体の重さ・液体の重さと物理の重力が同じ向きになる。
  案 B(格子の y を下向きにそろえ、world_cells の約束と既存の場面を書き直す)は場面の y を全部ひっくり返すので採らない見込み。
- 物理の場面(積み木)は物理の座標(y が上)で書いてあり、変わらない。変わるのは格子への写し方だけ。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
