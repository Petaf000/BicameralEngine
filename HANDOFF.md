# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0084 128÷64 の割り算の高速化 — 完了

## 状態(3 行以内)
- ユーザーが 04 §6 の書き方の方針を採用(ADR-0010・D-322)。SASS は T-0016 で、RDNA3 は AMD 機で測る。128÷64 の高速化を M1 に入れ(T-0084)、完了した。
- `FxDivU128By64` を逆数 + Newton 法(Möller & Granlund 2011)に替えた(新しい `FxReciprocalNormalizedU64`)。商と余りは厳密で、自己テストの要約 605bc2e41947d188 は変わらず。
- 費用(fmul = 1): 128÷64 1925 → 247、FxDivShiftS64 1829 → 247、FxMakeRecipU64 1494 → 301。04 §6・perf.md を更新。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release とも警告なし)・`job.py tidy` → 警告なし(19 ファイル)。
- `job.py test` → 15/15(`-Filter fixed -Show` で GPU・WARP の要約 605bc2e41947d188 が CPU と一致)。
- `job.py run -Preset release -Exe gpu_fixed_bench` → 29 演算の表(約 1 分)。`python3 tools/fixed_bench/dxil_count.py out/build/release/shaders/asm/bench` → DXIL の命令数。
- `python3 tools/archmap/archmap.py --check` → OK(リンク 21 個)。

## 壊れている/未確認のもの
- 04 §6 の NVIDIA(SASS)は T-0016、RDNA3(ISA)は AMD 機(D-207)で。AMD での一致・費用は未確認。
- GPU のテストは D3D12 のデバッグレイヤー無しで走っている。デバッグレイヤーと GBV は T-0003(次)。
- ベンチは GPU のクロックを固定していない(2 回の実行の差は数 %)。

## このチャットで決めたこと
- ADR-0010 / D-322(整数の演算の書き方の方針)。決定 2(128÷64 と Q 形式の割り算をセルごとの毎刻みで使わない)は、速くなった後もそのまま。
- ベンチの div128・divshift64 は除数を毎回変える(同じ除数だと、逆数を作る所がループの外へ出されて 75 になる)。

## 次にやること
NEXT.md の先頭(T-0003 GPU デバッグ基盤)。

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
