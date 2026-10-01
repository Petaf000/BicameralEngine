# ADR-0013 仮の世界(伝導 + 反応の Work Graph)のテストから WARP を外す

- Status: Accepted
- 日付: 2026-10-01
- 決めた人: ユーザー(Claude が 3 案を出した。T-0089)

## 背景
T-0089 で、仮の世界の伝導と反応を 1 つの Work Graphs のノード(shaders/sim/probe_conduct.hlsl の ConductBlock)にした。
ハードウェアの GPU(RTX 3070 Ti)では CPU リファレンスとビット一致する。WARP では、この Work Graph を作る時点
(`gpu::WorkGraph::Create` の中、状態オブジェクトを作るところ)でプロセスがアクセス違反(0xC0000005)で落ちる。GPU-based validation を切っても同じ。

調べたこと(2026-10-01):
- 同じ反応のコード(reaction.hlsli)は compute シェーダーなら WARP で動く(gpu_reaction_warp)。
- ノードの中身を「読んで写すだけ」にすると動く。1 つの uint64 の比較や、ローカルの配列の動的な添字 1 つでも動く。
- 2 つの RxCell を 8 要素ぶん比べるループ(`ProbeSameCell`。展開してもしなくても)を足しただけで落ちる。反応を抜いて伝導だけでも落ちる。
  特定の書き方ではなく、ノードの関数が少し複雑になると WARP の Work Graphs の JIT が落ちると見ている(未確認。WARP の不具合と推定)。

## 決定
- 設計どおり、伝導と反応は 1 つのノードのまま(D-302「できる限り Work Graphs」)。
- ctest の `gpu_probe_sim_warp`・`gpu_probe_trace_warp` を外す。仮の世界のテストはハードウェアの GPU だけで走らせる。
- 機種による違いは、全部のセルを計算する CPU リファレンスとのビット一致で確かめ、AMD の機械(D-207)でも確かめる。
- 反応の核そのもの(compute)の WARP のテスト(gpu_reaction_warp)は残す。

## 検討した代案と、採らなかった理由
- Work Graph は起こすブロックを決めるだけにし、伝導と反応は compute を間接に起動する: WARP でも動くが、重い所が Work Graphs の外に出る。
- 落ちる原因をさらに細かく探し、書き方で避ける: 時間が読めない。ノードが育つたびに同じ問題が出うる。

## 影響
- tests/CMakeLists.txt から 2 つのテストを外した(理由をコメントに書いた)。窓のアプリを `--warp` で起こすと同じく落ちる。
- WARP の新しい版(Agility SDK の更新)で直ったかは、`job.py run -Exe gpu_probe_sim_test -- --warp` で確かめられる。直ったら戻す。
