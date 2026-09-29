# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0010 の追加分(整数のルーチンの費用の計測)— 完了

## 状態(3 行以内)
- fixed.hlsli に逆数の掛け算(FxMakeRecip*/FxDivRecip*)を足し、CPU・3070 Ti・WARP でビット一致(自己テスト 21 値、要約 605bc2e41947d188)。
- マイクロベンチ(shaders/bench/fixed_bench.hlsl を演算ごとにコンパイル + tests/gpu_fixed_bench.cpp)で 3070 Ti の費用を測り、DXIL の命令数と合わせて 04 §6 の表に載せた。
- 結果(fmul = 1): int64 の割り算 113・逆数の掛け算 33、128÷64(FxDivShiftS64)1925、exp2 553・log2 1190。書き方の方針の案は 04 §6 の末尾(ユーザー判断待ち)。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release とも警告なし)・`job.py tidy` → 警告なし(19 ファイル)。
- `job.py test` → 15/15(自己テストの要約は 605bc2e41947d188 に変わった)。
- `job.py run -Preset release -Exe gpu_fixed_bench` → 29 演算の Markdown の表がログに出る(約 1 分)。
- `python3 tools/fixed_bench/dxil_count.py out/build/release/shaders/asm/bench` → DXIL の命令数の表(Linux 側で動く)。
- `python3 tools/archmap/archmap.py --check` → OK(リンク 21 個)。CI はまだ回していない(push 後に確認)。

## 壊れている/未確認のもの
- 04 §6 の NVIDIA(SASS)・RDNA3(ISA)の命令数は未計測(BACKLOG、ユーザー判断待ち)。
- GPU のテストは D3D12 のデバッグレイヤー無しで走っている(API の誤用は見えない)。デバッグレイヤーと GBV は T-0003。
- AMD(D-207)での一致・費用は未確認。ベンチは GPU のクロックを固定していない(2 回の実行の差は数 %)。

## このチャットで決めたこと
- bicameral_add_shader に NAME・DEFINES・FLAGS を足した(同じソースを変種ごとにコンパイル)。shaders/bench/ は計測専用で浮動小数点を使ってよい(検査しない)。
- gpu::CreateRootUavSignature に rootConstantCount(b0 のルート定数)を足した。ImmediateQueue::Native() でキューを取れる。
- 計測の exe は ctest に登録しない(合否の無い計測)。

## 次にやること
NEXT.md の先頭(T-0003 GPU デバッグ基盤)。その前に BACKLOG の 2 件(128÷64 の高速化・SASS/RGA)と 04 §6 の方針の案をユーザーと決める。

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
