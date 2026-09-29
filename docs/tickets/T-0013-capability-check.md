# T-0013 能力の確認(int64・64bit atomic・compute キューの DispatchGraph・WARP)

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1〜2 回分
- マイルストーン: M1 (docs/plan/ROADMAP.md)
- 設計: docs/design/04-numerics-determinism.md §未確認・06-simulation-loop.md §未確認・16-debug-test.md §4 / 決定: D-412

## 目的(1〜2 行)
M1 以降の前提になる GPU の機能を、エンジン自身の出力で確定させる。
あわせて fixed.hlsli が GPU で CPU とビット一致することを確かめる(以後の GPU カーネルのテストの型にする)。

## 完了条件(チェックできる形で)
- [x] `--caps` に Int64ShaderOps・64bit atomic(typed / raw / groupshared)を表示し、この PC の値を docs/perf.md に記録
- [x] 最低機の条件(int64 と 64bit atomic を必須にするか)を 04 §未確認 から決定に移す(ユーザーの判断が要れば ADR を Proposed で)
- [x] fixed の自己テスト(bin/shaders/sim/fixed_selftest.cso)を GPU で走らせ、CPU の要約(85c154e666febd92)と一致することをテストにする。debug と release の両方
- [x] compute キュー(D3D12_COMMAND_LIST_TYPE_COMPUTE)で DispatchGraph が通るか確かめ、06 §未確認 を結果で置き換える
- [x] WARP で SM 6.8 と Work Graphs が使えるかを確かめ、16 §4 を結果で置き換える(使えれば CI の GPU テストをチケット化)
- [x] HANDOFF / NEXT 更新・コミット

## メモ・参考
- WARP の Work Graphs 対応・compute キューでの DispatchGraph は、仕様とドライバの両方で **未確認**。推測で書かず、実行結果で埋める。
- Debug のシェーダーは -Od。debug と release の両方でビット一致を見る(04 §未確認)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-09-30: 鍵の置き場所をリポジトリの .bicameral-runner に移した(847c270。別件だが同じチャット)。
  engine/src/gpu(デバイス・1 回投げて待つキュー・バッファ・Work Graph)を作り、gpu_fixed_test・gpu_work_graph_test を ctest に登録(ラベル gpu / warp)。
- 結果: RTX 3070 Ti と WARP の両方で、fixed の自己テストが debug(-Od)・release とも CPU とビット一致(85c154e666febd92)。
  最小の Work Graph が direct / compute キューの両方で正しく走る。WARP は FL 12_1・SM 6.8・WG 1.0。CI(windows-2025)でも warp のテストが通った(run #16)。
- `--caps` に Int64ShaderOps・64bit atomic・WARP を表示。最低機の条件は ADR-0009 / D-211(ユーザーが案 A)。CreateDevice が起動時に確かめる。
- archmap が CMake の function() を定義として見つけるようにした(呼び出し行を指していた)。
