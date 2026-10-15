# T-0619 格子の y を上にそろえる(物理の対応・描画・筆・試験)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: ROADMAP の T-0210(T-0592 から分けた。T-0590 の判断待ち 1)
- 設計: docs/design/07-transport.md §2.7・08 / 決定: ADR-0256 の 5(仮 = 案 A)

## 目的(1〜2 行)
場面のパッケージの world_cells と気体(probe_gas・GPU の区画)は「格子の y が上」、物理の対応(probe_sim.hlsli の ProbeCellOfPhysicsPoint)と
描画(debug_camera の worldUp = −y・probe_view の物の置き方)は「格子の y が下向き」で食い違っている。案 A(仮・ユーザー未確認)で y を上にそろえる。

## 完了条件(チェックできる形で)
- [x] probe_sim.hlsli: 格子 y = 物理 y / 0.5 m(裏返さない)。ProbeCellOfPhysicsPoint・コメント(物理の地面の上の面が格子の y = 0)
- [x] 描画: debug_camera の worldUp = +y・probe_view の物の置き方(y を裏返さない・回転の符号)・BODY_LIGHT・frame_loop の押す光線(QueuePushes・autoPush)
- [x] 筆(brush_panel・brush_preview)と場面の試験(physics_heat_test の y の範囲など)を合わせる
- [x] 燃える木箱の煙が窓で上へ上がる画像(区画をアプリで載せる道は T-0620)
- [x] release のビルド・gpu_probe_physics・physics_heat・brush_preview・gpu_probe_gas

## メモ・参考
- 作り手から見た違い(案 A): 今書いた場面(world_cells の y が上)がそのまま上下正しく見え、気体の重さ・液体の重さと物理の重力が同じ向きになる。
  案 B(格子の y を下向きにそろえ、world_cells の約束と既存の場面を書き直す)は場面の y を全部ひっくり返すので採らない見込み。
- 物理の場面(積み木)は物理の座標(y が上)で書いてあり、変わらない。変わるのは格子への写し方だけ。

## 作り手(エンジンを使う人)から見た違い
- world_cells に書いた高さが、そのまま窓の上下になる(y を大きく書いた物が上に見える)。今までは物理の積み木だけが正しく、セルの場面は上下逆に見えていた。
- 気体の重さ(浮力)・液体の重さと、物理の重力が同じ向き(−y)。燃える木箱の煙が窓で上へ流れて見える。
- 物理の熱(ぶつかりで失った力学的エネルギー)は、物の下の地面のセル(格子の y = 0 側)へ返る。今までは格子の上の端(y = 63)へ返っていた。
- 物理の場面(積み木)の書き方は変わらない(物理の座標は前から y が上)。変わるのは格子への写し方と、描画・押す光線の向きだけ。
- 窓のカメラの既定の見え方は同じ(右 = +x・上 = +y・奥 = +z。格子の y が上になったので、上下の符号の約束だけが変わった)。

## 座標の変換の置き場所(並走との重なり)
- シミュの側(整数)で物理 → 格子は `ProbeCellOfPhysicsPoint`(shaders/common/probe_sim.hlsli)の 1 か所。CPU の参照(probe_sim.cpp)も同じ関数を使う。
  描画(probe_view.hlsl の物の置き方)と窓の押す光線(frame_loop の QueuePushes)は浮動小数点で、同じ倍率(1 セル = 2^19 単位)を掛けるだけになった(裏返しが無くなったので式が無い)。
- 気体(probe_gas・GPU の区画)・液体(probe_liquid)の区画は前から格子の座標(y が上)で、物理との変換を持たない。T-0598(wt5)・T-0593(wt2)・T-0611(wt4)・T-0646(wt6)と
  ぶつかりうるのは probe_sim.hlsli の物理の節のコメントと ProbeCellOfPhysicsPoint だけ(変えたのは 3 行)。マージで当たったら T-0619 の側(裏返さない)を採る。

## 世界のハッシュが変わる試験
- 物理を入れた世界(--world sample の積み木・physics_heat・gpu_probe_physics・window_replay)は、物理の熱が足されるセルが変わるので、刻みごとのセルのハッシュが前と変わる。
  固定の期待値を持つ試験は無かった(どれも CPU と GPU・記録と再生の一致を見る)。physics_heat_test は熱を見る範囲を格子の底(y 0〜4)へ直した。
  debug_camera_test は「画面の下 = +y」を「画面の下 = −y」に直した。古い再生ファイル(押す・物理の熱を含む)は、物理の熱の入るセルが違うのでハッシュが合わなくなる。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-10-15(作業役 wt3): 案 A(仮・ユーザー未確認)で y を上にそろえた。ProbeCellOfPhysicsPoint を裏返さない・debug_camera の上 = +y(左手系。forward の y の符号も)・
  probe_view の物の置き方と回転(符号の入れ替えを外す)・BODY_LIGHT・押す光線(QueuePushes・--auto-push は格子の (20, 11, 32.5))。筆(brush_panel・brush_preview)は格子の座標だけで動くので変更なし。
  試験: physics_heat の熱を見る範囲を格子の底へ・debug_camera の真ん中の画素を半画素上へ(画面の下 = −y)。release のビルド(警告 0)・physics_heat・debug_camera・brush_preview・
  gpu_probe_physics(・_compute・_gravity_swap)・gpu_probe_gas・window_replay_*(6 本)が通った。窓(--world sample --auto-ignite --auto-push)で積み木が格子の底に立ち、押すと物 6 に当たる(前と同じ物)。
  煙の画像: アプリで区画を載せる道が無い(T-0620)ので gpu_probe_gas_test の断面(z = 32・y が上)で、CO2 が底の木箱から区画の上まで広がるのを見た(気体の段はこのチケットで変えていない。画像は前とバイト一致)。
  気づいたこと(このチケットの外): 同じ画像の温度の側が、区画の中でセル 1 つおきの市松模様(800 K 超と低いセルが交互)。T-0592 の時から同じ。気体の段の奇偶の分離の疑い → 司令塔へ。
