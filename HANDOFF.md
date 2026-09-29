# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0003 GPU デバッグ基盤 — 完了

## 状態(3 行以内)
- シェーダー(compute と Work Graphs のノード)から書ける printf / assert のリングを作った(`DEBUG_PRINT` / `DEBUG_ASSERT`、書式は CPU 側の一覧)。FX_ASSERT もつないだ。
- `gpu::Device` が debug プリセットで debug layer・GBV・DRED を有効にし、debug layer の報告をログへ流して数える。GPU のテストはエラー 0 件も確かめる。
- テスト 19/19(debug / release、ハードウェアと WARP)。設計は 16 §1.1。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release とも警告なし)・`job.py tidy` → 警告なし(22 ファイル)。
- `job.py test` → 19/19。`-Filter gpu_debug -Show` でリングの行(`GPU print debug_ring_probe/Main:18 ...`)と DRED の記録が見える。
- `job.py run -Preset release -Exe gpu_fixed_bench` → 29 演算の表(bench のシェーダーはリング無し)。
- `python3 tools/archmap/archmap.py --check` → OK(リンク 25 個)。

## 壊れている/未確認のもの
- DRED の「止まったコマンド」表示は本物のハング(TDR)で未確認(RemoveDevice では終わったリストの記録が残らない。16 §4)。
- 04 §6 の NVIDIA(SASS)は T-0016、RDNA3(ISA)は AMD 機(D-207)で。
- ベンチは GPU のクロックを固定していない(2 回の実行の差は数 %)。

## このチャットで決めたこと
- リングのバインドは u0 space1 のルートの UAV で固定(space0 はシェーダーが自由に使う)。容量 4096 件 / フレーム。有効なのは Debug のシミュのシェーダーだけ。
- 書式の文字列は GPU に置かない(番号 + 整数の引数)。場所は書式の一覧に、刻み・セル・レコードの ID は引数で(T-0008 もこの形)。
- ADR にはしていない(16 §1.1 に書いた。方針の変更ではなく実装の詳細)。

## 次にやること
NEXT.md の先頭(T-0004 窓とフレームループ)。

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
- `DebugRing` の読み戻しのバッファは 1 つ。フレームを重ねる(T-0004)ときはフレームごとに持たせる。
