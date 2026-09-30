# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0085 R-LOOP-2 重いシミュと描画の並走 — 完了

## 状態(3 行以内)
- `bicameral`(引数なし)で窓が開き、compute の仮の刻み(128² の拡散)と direct の描画がフェンスだけで並んで回る(T-0004)。
- R-LOOP-2 の原因が分かった: この GPU では**描画は compute に先に積まれた仕事が終わるまで始まらない**。CPU が 1 フレームに予算ぶんだけ投げれば描画は戻る。
  方針は ADR-0011(予算 = 目標のフレーム時間 − 描画、目標 fps は設定で既定 60、刻みはフレームをまたいでよい)。測定は docs/perf.md、設計は 06 §4・§4.2。テスト 22/22。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release とも警告なし)・`job.py tidy` → 警告なし(32 ファイル)・`python3 tools/archmap/archmap.py --check` → OK(39)。
- `job.py test` → 22/22。`-Filter gpu_probe_sim -Show` で「8 ずつ / 1 ずつ / ばらばら」の要約が CPU(cc28719c179eb6ea)と一致。
- `job.py run -Preset release -- --frames 600 --auto-click` → 窓が開き、1 秒ごとに fps・CPU・世界の刻み/秒・GPU 時間のログ、つつきのイベント(クリックから CPU に戻るまで 7〜24 ms)。
  引数: `--frames n` `--no-vsync` `--latency 2|3` `--sim-load n`(重さの試験)`--auto-click` `--warp`(main.cpp の先頭)。人が窓をクリックしても同じ。
- `--caps` はアダプタごとに PCI の ID と画面を出す。
- R-LOOP-2 の試験の口(T-0085): `--sim-load n --sim-split k [--split-submit [--pieces-per-frame p]] [--render-normal]`。
  例 `job.py run -Preset release -- --frames 300 --sim-load 5200000 --sim-split 16 --split-submit --pieces-per-frame 4` → 約 164 fps・世界 41 刻み/秒
  (5.2M ≈ 22.5 ms/刻み、1M ≈ 4.8 ms)。分けない `--sim-load 5200000` だと 45 fps。

## 壊れている/未確認のもの(ファイル:行 と症状)
- 今のフレームのループ(frame_loop.cpp)はまだ T-0004 の形(バッチを 2 つまで重ねる)。重いと描画が落ちるのはそのまま。T-0012 で ADR-0011 の形に置き換える。
- 未確認: 予算ぶんを 1 本のリストにまとめて 1 回で投げても同じか / GPU 側で未来の描画のフェンスを compute に待たせる形(誤った実装で固まった)/ compute と描画が GPU の中で少しでも重なるか / AMD。
- vsync ありで Present の中に平均 1 ms(BACKLOG。害は無い)。
- 窓の大きさの変更・最小化・DPI の変更はコード上は扱っているが、人の手で試していない(自動の確認は大きさを変えない)。
- DRED の「止まったコマンド」表示は本物のハング(TDR)で未確認(16 §4)。SASS / RDNA3 の命令数は T-0016 / AMD 機。

## このチャットで決めたこと(ADR にしたなら番号)
- **ADR-0011**(T-0085): シミュは 1 フレームの予算ぶんずつ投げる。予算 = 目標のフレーム時間 − 描画の GPU 時間 − 余裕、目標 fps は設定で既定 60(30 より下げない)。
- T-0012 から R-LOOP-2 を T-0085 として分けた(ROADMAP・BACKLOG に記録)。
- 仮の刻みの重さは Diffuse から別の入口 `Busy`(probe_tick_busy.cso)に移した(分けて投げる試験のため。世界の結果は変わらない)。
以下は T-0004 のもの:
- **D-423**(DECISIONS.md): 過去の自作コード(DX12 / ReSTIR DI など)は一切流用しない。描画もほかも着手時点のモダンな手法を調べて一から設計する。T-0004 の流用の項目は削除。
- 描画用の抽出は 3 組(06 §4 を 2 組から改めた)。描画は終わっている最新を読み、シミュのバッチは 2 つまで重ねる。
- 刻みの数はリストの選び方で(刻みの数ごとに記録)。ExecuteIndirect の数での切り替えは遅かった(perf.md)。
- アダプタは窓の画面を持つものを優先(`DeviceOptions::presentMonitor`)。描画のキューは優先度 HIGH。
- ADR は書いていない(06 §4.1 に書いた。方針の変更ではなく実装の詳細。3 組の件は 06 本文を直した)。

## 次にやること
NEXT.md の先頭(T-0012 刻みのループ。投げ方は ADR-0011)。

## 注意(次の Claude がハマりそうな所)
- **同じコマンドリストを、キューのフェンスが前の実行を越える前に投げ直さない**(debug layer [553]。debug layer はその実行を捨て、release は黙って走る)。
  1 バッチの中で何回も投げるものは、回数分の別のリストを記録する(probe_sim の busyLists)。`gpu::Queue::Execute` はフェンスを進めずに投げる。
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
- **フレームのループ(frame/frame_loop.cpp)**: 抽出の組・バッチの枠・描画の枠の約束はファイルの先頭に書いた。シミュのバッチを増やす・重ねる数を変えるときは
  「抽出の組の数 = 重ねるバッチの数 + 1」を守る(破ると描画とシミュが同じ抽出を同時に使う)。
- **バックバッファを作り直す前・終わる前は `Queue::Flush()`**(最後の Submit の値を待つだけだと、その後ろの Present がまだ走っていて debug layer が CORRUPTION を出す)。
- 仮の刻み(sim/probe_sim・shaders/sim/probe_tick.hlsl・shaders/common/probe_sim.hlsli)は T-0012 で本物の刻みに置き換える前提の使い捨て。形(64 バイトのコマンド・枠ごとの入力・イベントのリング)は本物にも使う。
- `job.py run` は窓を開く(ユーザーの画面に出る)。自動の確認は `--frames n --auto-click` で終わらせる。
- clang-tidy: `std::optional` のメンバーは unchecked-optional-access で大量に警告が出る。作れたものだけを受け取る形(ProbeSim・FrameLoopParts)にする。
- HLSL の `[numthreads(...)] void Name(` は archmap が定義として見つけない。map.yaml ではその .hlsl の普通の関数を指す。
