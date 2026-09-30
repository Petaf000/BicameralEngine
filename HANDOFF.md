# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0004 窓とフレームループ — 完了

## 状態(3 行以内)
- `bicameral`(引数なし)で窓が開き、compute キューの仮の刻み(クリックした所が熱くなって広がる 128² の世界)と direct キューの描画が、フェンスだけで並んで回る。
- リストは作るときに 1 度だけ記録して使い回す。GPU → CPU は待たない読み戻し(イベント・デバッグ・タイムスタンプ)。刻みの数はタイムスタンプから決め、重いと世界が遅くなる。
- テスト 22/22(debug / release、ハードウェアと WARP)。設計は 06 §4・§4.1、計測は docs/perf.md。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release とも警告なし)・`job.py tidy` → 警告なし(32 ファイル)・`python3 tools/archmap/archmap.py --check` → OK(39)。
- `job.py test` → 22/22。`-Filter gpu_probe_sim -Show` で「8 ずつ / 1 ずつ / ばらばら」の要約が CPU(cc28719c179eb6ea)と一致。
- `job.py run -Preset release -- --frames 600 --auto-click` → 窓が開き、1 秒ごとに fps・CPU・世界の刻み/秒・GPU 時間のログ、つつきのイベント(クリックから CPU に戻るまで 7〜24 ms)。
  引数: `--frames n` `--no-vsync` `--latency 2|3` `--sim-load n`(重さの試験)`--auto-click` `--warp`(main.cpp の先頭)。人が窓をクリックしても同じ。
- `--caps` はアダプタごとに PCI の ID と画面を出す。

## 壊れている/未確認のもの(ファイル:行 と症状)
- R-LOOP-2(06 研究): 1 刻みの Dispatch が 25 ms 以上だと、描画はシミュを待っていないのに fps がシミュのバッチの速さまで落ちる。原因は未確認。BACKLOG に案(T-0012 で刻みを分けて投げる)。
- vsync ありで Present の中に平均 1 ms(BACKLOG。害は無い)。
- 窓の大きさの変更・最小化・DPI の変更はコード上は扱っているが、人の手で試していない(自動の確認は大きさを変えない)。
- DRED の「止まったコマンド」表示は本物のハング(TDR)で未確認(16 §4)。SASS / RDNA3 の命令数は T-0016 / AMD 機。

## このチャットで決めたこと(ADR にしたなら番号)
- **D-423**(DECISIONS.md): 過去の自作コード(DX12 / ReSTIR DI など)は一切流用しない。描画もほかも着手時点のモダンな手法を調べて一から設計する。T-0004 の流用の項目は削除。
- 描画用の抽出は 3 組(06 §4 を 2 組から改めた)。描画は終わっている最新を読み、シミュのバッチは 2 つまで重ねる。
- 刻みの数はリストの選び方で(刻みの数ごとに記録)。ExecuteIndirect の数での切り替えは遅かった(perf.md)。
- アダプタは窓の画面を持つものを優先(`DeviceOptions::presentMonitor`)。描画のキューは優先度 HIGH。
- ADR は書いていない(06 §4.1 に書いた。方針の変更ではなく実装の詳細。3 組の件は 06 本文を直した)。

## 次にやること
NEXT.md の先頭(T-0012 刻みのループ)。

## 注意(次の Claude がハマりそうな所)
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
- **フレームのループ(frame/frame_loop.cpp)**: 抽出の組・バッチの枠・描画の枠の約束はファイルの先頭に書いた。シミュのバッチを増やす・重ねる数を変えるときは
  「抽出の組の数 = 重ねるバッチの数 + 1」を守る(破ると描画とシミュが同じ抽出を同時に使う)。
- **バックバッファを作り直す前・終わる前は `Queue::Flush()`**(最後の Submit の値を待つだけだと、その後ろの Present がまだ走っていて debug layer が CORRUPTION を出す)。
- 仮の刻み(sim/probe_sim・shaders/sim/probe_tick.hlsl・shaders/common/probe_sim.hlsli)は T-0012 で本物の刻みに置き換える前提の使い捨て。形(64 バイトのコマンド・枠ごとの入力・イベントのリング)は本物にも使う。
- `job.py run` は窓を開く(ユーザーの画面に出る)。自動の確認は `--frames n --auto-click` で終わらせる。
- clang-tidy: `std::optional` のメンバーは unchecked-optional-access で大量に警告が出る。作れたものだけを受け取る形(ProbeSim・FrameLoopParts)にする。
- HLSL の `[numthreads(...)] void Name(` は archmap が定義として見つけない。map.yaml ではその .hlsl の普通の関数を指す。
