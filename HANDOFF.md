# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0012 刻みのループ(予算ぶんの単位・状態のハッシュ)— 完了

## 状態(3 行以内)
- `bicameral`(引数なし)で窓が開き、1 刻み = 単位の列(適用 → 拡散 → 重さ × k → ハッシュ)を、毎フレーム予算ぶんだけ compute に投げ、その後ろに描画を投げる(ADR-0011)。
  刻みはフレームをまたぐ。単位の数は `frame::SimScheduler`(TickPacer を置き換え)、刻みごとの状態のハッシュを GPU で取って CPU へ返す。
- 元の T-0012 は分けた。次は T-0086(コマンドキュー・並べたイベント・再生ファイル)。テスト 22/22。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release とも警告なし)・`job.py tidy` → 警告なし(32 ファイル)・`python3 tools/archmap/archmap.py --check` → OK(39)。
- `job.py test` → 22/22。`-Filter gpu_probe_sim -Show` で分け方 5 通りの S(40) = 2523aed9e332cfa6 が CPU と一致(刻みごとのハッシュ列・最後の抽出・イベント 7)。
- `job.py run -Preset release -- --frames 600 --auto-click` → 165 fps・世界 60 刻み/秒。1 秒ごとのログに fps・単位/投入・シミュ GPU・予算・状態 S(t) のハッシュ。
  引数: `--frames n` `--no-vsync` `--latency 2|3` `--target-fps f`(30〜1000、既定 60)`--sim-load n --sim-split k`(重さの試験を k 単位に)`--render-normal` `--auto-click` `--warp`(main.cpp の先頭)。
  例 `--sim-load 5200000 --sim-split 16` → 70.6 fps・世界 42.8(最大の 96%)。`--target-fps 165` → 165 fps・29.5(docs/perf.md)。
- `--caps` はアダプタごとに PCI の ID と画面を出す。

## 壊れている/未確認のもの(ファイル:行 と症状)
- コマンドは「このフレームに入る最初の適用の単位の刻み」に付けて、そのフレームの入力で渡す(frame_loop.cpp の AssignCommandTicks)。本物のコマンドキュー(GPU 側で刻みまで待つ)と
  イベントの並べ替え・再生ファイルは T-0086。
- 1 単位が予算より重いと、そのフレームだけ描画が遅れる(分けない `--sim-load 5200000` で重いフレームは 22 ms)。本物の段は単位を 1〜3 ms に分ける(Work Graphs はレコード数の上限。06 §4.2)。
- 未確認: GPU 側で未来の描画のフェンスを compute に待たせる形 / compute と描画が GPU の中で少しでも重なるか / AMD。
- vsync ありで Present の中に平均 1 ms(BACKLOG。害は無い)。
- 窓の大きさの変更・最小化・DPI の変更はコード上は扱っているが、人の手で試していない(自動の確認は大きさを変えない)。
- DRED の「止まったコマンド」表示は本物のハング(TDR)で未確認(16 §4)。SASS / RDNA3 の命令数は T-0016 / AMD 機。

## このチャットで決めたこと(ADR にしたなら番号)
- T-0012 を分けた: T-0012(刻みのループ・ハッシュ)と T-0086(コマンドキュー・並べたイベント・再生ファイルの骨組み)。ROADMAP・NEXT に記録。
- シミュのリストはフレームの枠(4)ごとに毎フレーム記録する(同じリストを前の実行が終わる前に投げ直せないため。記録済みのリストの使い回しはやめた)。06 §4.1。
- 予算ぶんを 1 本のリストで 1 回投げる形で足りる(06 §4.2 の未確認を測って解消)。
- 状態のハッシュ = Σ Mix64(セルの番号, 値) mod 2^64(順番に依存しない和。GPU は wave の和 + 64bit atomic)。表 256 個、S(t) は t % 256 番目。
- 未処理の刻みの上限は 8(約 133 ms。超えた分は捨てる)。まだ測っていない単位は予算いっぱいと見なす。
- ADR は書いていない(ADR-0011 の実装の詳細。06 §4.1 に書いた)。

## 次にやること
NEXT.md の先頭(T-0086 コマンドキュー・並べたイベント・再生ファイルの骨組み)。

## 注意(次の Claude がハマりそうな所)
- **同じコマンドリストを、キューのフェンスが前の実行を越える前に投げ直さない**(debug layer [553]。debug layer はその実行を捨て、release は黙って走る)。
  だからシミュのリストは ProbeSim のフレームの枠ごとに毎フレーム記録し直す(枠の前のリストが終わるまで、その枠は使わない)。`gpu::Queue::Execute` はフェンスを進めずに投げる。
- `job.py run` で窓のループが固まると、run の timeout(600 秒)まで runner が塞がる。試すときは `job.py run --timeout 60 -Preset ...` にする。
- **GPU のテストを足すとき**: tests/CMakeLists.txt の `bicameral_add_gpu_test(<名前>)` と `bicameral_add_gpu_test_case(<テスト名> <exe> <gpu|warp> <引数>)`。
  exe は bin/ に出て、bin/D3D12(Agility SDK)と bin/shaders をそのまま使う(agility_sdk.cpp を exe ごとに入れている)。
- テストの出力は printf ではなく Log(日本語をそのまま出せる)。終わりに `SingletonFinalizer::Finalize()`。
- **HLSL のノードの属性は clang-format が崩す**ので、work_graph_probe.hlsl のように属性つきの宣言を `// clang-format off/on` で囲む。
- Work Graph の起動順: SetComputeRootSignature → WorkGraph::SetProgram(最初だけ initialize=true)→ SetComputeRoot* → DispatchFromCpu。
- バッファは COMMON で作り、最初の UAV の使用で暗黙に昇格させている。読み戻しは gpu::RecordCopyToReadback(UAV → COPY_SOURCE)。
- archmap は CMake の `function(名前` も定義として見つける(T-0013 で追加)。C++ のメンバー関数は `Class::Method` で書く。
- **シミュのコードに `sqrt`・`pow`・`floor`・`lerp` などの名前の変数や関数を作らない**(ソースの検査が拒否する)。FxSqrt のように接頭辞を付ける。
- 描画だけの共通の HLSL は shaders/common ではなく shaders/render/ に置く(common は浮動小数点禁止)。
- **ベンチの演算を足すとき**: shaders/CMakeLists.txt の BICAMERAL_BENCH_OPERATIONS・fixed_bench.hlsl の BENCH_OP の分岐・
  tests/gpu_fixed_bench.cpp の BENCH_OPERATIONS・tools/fixed_bench/dxil_count.py の ORDER を同じ順で直す(番号 = 並び順)。
  定数の除数や、前の結果に依存しない連鎖はドライバがまとめて消すので、値は実行時のものにして前の結果に依存させる。
- **fixed.hlsli の約束**: 64bit の定数は `FX_U64(上位, 下位)`、関数は `FX_FN`、定数は `FX_CONST`。桁あふれしうる計算は符号なしで。
  自己テストに関数を足したら `FX_SELF_TEST_OUTPUT_COUNT` を増やす(要約 85c154e666febd92 が変わる。gpu_fixed_test も自動で追従する)。
- `job.py tidy` は debug のビルドフォルダの compile_commands.json を使う。先に `job.py build`。
- tools/dxil_float_check/ という空のフォルダが PC に残っている(削除の許可が無く消せなかった。git には入らない)。
- **公開リポジトリ。** 鍵(.bicameral-runner/)・CLAUDE.local.md・PC 固有のパスをコミットしない。ゲームの中身も公開側に入れない(D-006)。
- Linux 側の整形: `python3 -m pip install --user clang-format==23.1.1 pyyaml` → `~/.local/bin/clang-format -i`(セッションごとに入れ直し)。
- `.github/` 以下は device_commit_files では書けない(保護)。device_bash の cp や python なら書ける。
- runner の結果 JSON(runner/logs/*.result.json)を cat しない。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる)。.ps1 は UTF-8(BOM 付き)+ CRLF。
  ただし `job.py raw` に渡す .ps1 は本文が埋め込まれるので BOM を付けない。
- Windows の Python がパイプに書く文字は CP932 になる。ビルドの中で呼ぶ Python は `sys.stdout.reconfigure(encoding="utf-8")` する。
- **fixed.hlsli の割り算を変えるとき**: 先に 64bit の剰余演算を Python で真似て突き合わせると早い(T-0084 はそうした)。fixed_test の `TestDivide128` が _udiv128 と境界まで比べる。
- **デバッグのリングを使うシェーダー**: ルート署名を `gpu::CreateRootSignature(device, {.uavCount = n, .debugRing = true})` で作り、
  `ring.RecordBegin(list)` → `SetComputeRootUnorderedAccessView(layout.DebugRingIndex(), ring.GpuAddress())` → 書く → `ring.RecordReadbackAndReset(list)` → 待つ → `ring.Drain()`。
  fixed.hlsli を使うシミュのシェーダーは Debug で FX_ASSERT がリングを使うので、必ずリングを結ぶ(結ばないと PSO / 実行が壊れる)。
- 書式を足すときは shaders/common/debug_formats.hlsli に `DEBUG_FORMAT(名前, チャンネル, 場所, 書式)` を 1 行。HLSL では `DebugFormat::名前`。
- **HLSL で `line` は予約語**(ジオメトリシェーダーの修飾子)。変数名に使うと「modifiers must appear before type」になる。
- debug layer は、デバイスを作った後に有効にするとデバイスが失われる。`gpu::Device::Create` は必ず最初のデバイスより前に設定する(1 プロセス 1 回の想定)。
- `DebugRing` と `ReadbackRing` の読み戻しは枠(slot)ごと。`Create(device, slotCount)` → `RecordReadbackAndReset(list, slot)` → その枠のリストが終わってから `Drain(max, slot)` / `Read(slot, ...)`。
- **フレームのループ(frame/frame_loop.cpp)**: 抽出の 3 組の約束はファイルの先頭に書いた(抽出 n は、終わっている抽出が n − 2 以上のときだけ)。
  抽出の組の数を変えるときはこの条件も変える(破ると描画とシミュが同じ抽出を同時に使う)。
- **バックバッファを作り直す前・終わる前は `Queue::Flush()`**(最後の Submit の値を待つだけだと、その後ろの Present がまだ走っていて debug layer が CORRUPTION を出す)。
- 仮の刻み(sim/probe_sim・shaders/sim/probe_tick.hlsl・shaders/common/probe_sim.hlsli)の中身(拡散)は T-0005 以降で本物に置き換える前提。
  形(単位の列・単位ごとのタイムスタンプ・刻みの最後のハッシュ・64 バイトのコマンド・枠ごとの入力・イベントのリング)は本物にも使う。
  単位を足すときは probe_sim.hlsli の単位の表・ProbeSim::RecordUnit・UnitsPerTick を揃える。ハッシュの単位は必ず刻みの最後(次の刻みの適用より前の S(t+1) を取る)。
- ハッシュの表の読み戻しは、そのフレームにハッシュの単位があるときだけ(無いのに UAV → COPY_SOURCE を入れると状態が合わない)。ProbeSim::RecordReadbacks。
- HLSL の `[numthreads] void F(...) {}` が続くと clang-format が次の行を字下げして崩す。probe_tick.hlsl は入口の範囲をまとめて `// clang-format off/on` で囲んだ。
- HLSL の 64bit の剰余は避ける(表の番号は下位 32bit を 2 の冪でマスク)。WaveActiveSum は uint64_t で使える(DXC・RTX 3070 Ti・WARP で確認)。
- `job.py run` は窓を開く(ユーザーの画面に出る)。自動の確認は `--frames n --auto-click` で終わらせる。
- clang-tidy: `std::optional` のメンバーは unchecked-optional-access で大量に警告が出る。作れたものだけを受け取る形(ProbeSim・FrameLoopParts)にする。
- HLSL の `[numthreads(...)] void Name(` は archmap が定義として見つけない。map.yaml ではその .hlsl の普通の関数を指す。
