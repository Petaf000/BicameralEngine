# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0086 コマンドキュー・並べたイベント・再生ファイルの骨組み — 完了

## 状態(3 行以内)
- `bicameral`(引数なし)で窓が開き、1 刻み = 単位の列(適用 → 拡散 → 重さ × k → 検査と出力)を毎フレーム予算ぶん compute に投げる(ADR-0011)。
  コマンドは GPU のキュー(環状 1024)で自分の刻みまで待ち、イベントは刻みの終わりに (刻み, 種類, 場所) で並べてリングへ。`--record` / `--replay` で再生ファイル。
- 次は T-0005(クリックから Work Graphs で自動伝播)。テスト 25/25。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release とも警告なし)・`job.py tidy` → 警告なし(35 ファイル)・`python3 tools/archmap/archmap.py --check` → OK(46)。
- `job.py test` → 25/25。`-Filter gpu_probe_sim -Show` で分け方 5 通りの S(40) = ca7bda63a1d7d6b4 が CPU と一致・イベント 8 個が並んで戻る・溢れ(256 + 落とした 44)・
  記録 → 別の分け方で再生して 40 刻み全部一致。`replay_file`(CPU)。`window_replay_record` → `window_replay_play`(窓を 2 回開く。ラベル gpu)。
- `job.py run -Preset release -- --frames 600 --auto-click --record x.bcreplay` → 165 fps・世界 59.7、`-- --replay x.bcreplay --sim-load 5200000 --sim-split 16` →
  68.5 fps・ハッシュ 217 個全部一致(相対パスは bin/ から。docs/perf.md)。
  引数: `--frames n` `--no-vsync` `--latency 2|3` `--target-fps f` `--sim-load n --sim-split k` `--render-normal` `--auto-click` `--record p` `--replay p` `--warp`(main.cpp の先頭)。

## 壊れている/未確認のもの(ファイル:行 と症状)
- 適用の単位は 1 スレッドでキューを順に読む(probe_tick.hlsl の ApplyCommands)。本物のコマンドが増えて重くなったら種類ごとに分ける(06 §3 実装)。
- イベントの一時置き場は 1 刻み 256 個・並べ替えは 1 グループの bitonic sort。本物の反応の段では足りないので、大きくして基数ソートに(06 §3)。
- 再生ファイルのハッシュは全部の刻みを持つ(間引きは読む側だけ対応)。セーブ(差分の書き出し)は未着手(15 §1)。
- 1 単位が予算より重いと、そのフレームだけ描画が遅れる(T-0012 から。本物の段は単位を 1〜3 ms に分ける。06 §4.2)。
- 未確認: GPU 側で未来の描画のフェンスを compute に待たせる形 / compute と描画が GPU の中で少しでも重なるか / AMD。
- vsync ありで Present の中に平均 1 ms(BACKLOG。害は無い)。窓の大きさの変更・最小化・DPI は人の手で試していない。
- DRED の「止まったコマンド」表示は本物のハング(TDR)で未確認(16 §4)。SASS / RDNA3 の命令数は T-0016 / AMD 機。

## このチャットで決めたこと(ADR にしたなら番号)
- コマンドの適用する刻み = まだ記録していない最初の適用の単位の刻み(`ProbeSim::NextApplyTick`)。フレームに適用の単位が無くても GPU のキューで待つ。
- キューに足すのは CPU だけ → 末尾・空きは CPU が控え、足す場所を入力の見出しで渡す(GPU で atomic を使わない)。約束違反は RecordFrame が拒否、GPU は遅れたものを捨ててイベントで知らせる。
- 適用は 1 スレッドで (targetTick, sequence) の順(順番に効くコマンドも決定的)。イベントの溢れでどれが残るかは決めない(数えるだけ。View 用なので)。
- コマンドの形は `sim/command.h` の `Command`(`ProbeCommand` はその別名)。再生ファイルは `engine/src/save/`(新しいライブラリ bicameral_save。CPU だけ)。
- 再生ファイルの形式の版 1(15 §2.1)。記録は最後にハッシュを読み戻した刻みまでに適用されるコマンドだけを書く。再生は 8 刻み先まで先に足す。
- ADR は書いていない(06 §3・15 §2 の実装の詳細)。

## 次にやること
NEXT.md の先頭(T-0005 クリックから Work Graphs で自動伝播)。

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
- **コマンドを足すときの約束**(ProbeSim::RecordFrame が確かめる): (targetTick, sequence) の昇順・targetTick ≥ `NextApplyTick(カーソル)`・数 ≤ `FreeCommandSlots()` かつ 256。
  CPU の控え(ProbeSim の m_commandTail・m_queuedTicks)は「記録したら GPU で実行される」前提。記録したリストを投げなかった場合は控えがずれる(今は失敗 = 終了なので問題なし)。
- probe_sim のルート署名は UAV 9 個(u7 コマンドキュー・u8 刻みのイベントの一時置き場)。イベントのリングの見出しは [0] 書こうとした数 [1] 一時置き場で落とした数。
- `--record` / `--replay` の相対パスは runner の作業フォルダ(bin/)から。ctest の window_replay_* は `out/build/<preset>/tests/window_replay.bcreplay` を使う。
- 窓の ctest(window_replay_*)は画面に窓が 2 回出る(約 6 秒ずつ)。CI はラベル gpu なので走らない。
