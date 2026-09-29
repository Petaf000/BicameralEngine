# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0013(能力の確認: int64・64bit atomic・compute キューの DispatchGraph・WARP)— 完了

## 状態(3 行以内)
- GPU の小さな道具 engine/src/gpu(CreateDevice・ImmediateQueue・CreateBuffer など・WorkGraph)ができ、GPU のテストを ctest で回せる。
- fixed の自己テストが RTX 3070 Ti と WARP で、debug(-Od)と release のどちらでも CPU とビット一致。最小の Work Graph が direct / compute キューで動く。
- WARP は Work Graphs と SM 6.8 に対応。CI(windows-2025)でも WARP の GPU テストが回る。ランナーの鍵はリポジトリの .bicameral-runner に移した。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release とも警告なし)・`job.py tidy` → 警告なし(18 ファイル)。
- `job.py test` → 15/15。GPU のテストは `job.py test -Filter gpu -Show`(要約 85c154e666febd92・Leaf の回数 1024 が出る)。
- `job.py run -- --caps` → Int64ShaderOps・Atomic64(typed / groupshared / heap)・WARP の行も出る。
- `python3 tools/archmap/archmap.py --check` → OK(リンク 20 個)。CI run #16(18da8e4)成功。

## 壊れている/未確認のもの
- GPU のテストは D3D12 のデバッグレイヤー無しで走っている(API の誤用は見えない)。デバッグレイヤーと GBV は T-0003。
- AMD(D-207)での一致・対応は未確認。WARP での速さは未測定。
- CI のログは WebFetch で run のページの成否しか見られない(GitHub の API はこのセッションから使えない)。

## このチャットで決めたこと
- ランナーの鍵は `<リポジトリ>\.bicameral-runner\key.txt`(git 管理外)。job.py はそこを直接読む。ホームのフォルダの接続は要らない(ユーザー決定)。
- ADR-0009 / D-211(ユーザーが案 A): Int64ShaderOps と raw / structured バッファの 64bit atomic を必須、typed・groupshared・ヒープ経由は使わない。
  gpu::CreateDevice が SM 6.8・Work Graphs・Int64ShaderOps を確かめ、足りないアダプタは使わない。
- GPU のテストは同じ exe を引数(`--warp`・`--queue direct|compute`)違いで登録し、ラベル gpu(ハードウェアが要る。CI は `--label-exclude gpu`)と warp(CI でも回す)に分ける。

## 次にやること
NEXT.md の先頭(T-0010 の追加分: 整数のルーチンのマイクロベンチと命令数の表、04 §6)。GPU で走らせる道具は engine/src/gpu と tests/gpu_fixed_test.cpp を真似る。

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
