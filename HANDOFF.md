# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0011(シェーダーのビルドの仕組み・浮動小数点の検査・CI の持ち越し)— 完了

## 状態(3 行以内)
- シェーダーは `bicameral_add_shader(<sim|render>/x.hlsl PROFILE … [ENTRY …])` の 1 行で足せる。成果物は bin/shaders/sim|render/*.cso。
- シミュ(shaders/sim・shaders/common・engine/src/sim)に浮動小数点が入るとビルドが落ちる(DXIL の検査 + ソースの検査。tools/float_check)。
- clang-tidy を `job.py tidy` と CI で強制(警告 0)。CI は Node 20 の action をやめ、Ubuntu を 26.04 に固定。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release とも警告なし)。ログに「浮動小数点の検査: …」と「DXC: sim/fixed_selftest.hlsl (cs_6_8)」が出る。
- `job.py test`(9/9。float_check_* 5 本が「検査が本当に落ちる/誤検出しない」を確かめる)。
- `job.py tidy` → 「run_clang_tidy: 警告なし」(12 ファイル、VS 同梱の clang-tidy 22.1.3)。
- `python3 tools/archmap/archmap.py --check`(Linux 側)→ OK(リンク 15 個。図に「ビルドの中の検査」を足した)。

## 壊れている/未確認のもの
- CI の新しい手順(vcvars64 の環境の受け渡し・pip の clang-tidy 22.1.8・ubuntu-26.04)は push 後の CI の結果で確かめる。落ちていたら最初に直す。
- fixed.hlsli の GPU での結果と速さは未確認のまま(T-0013)。Debug のシェーダーは -Od なので debug / release の両方で比べる。

## このチャットで決めたこと
- C++ 側の浮動小数点の禁止は clang-tidy のカスタム検査ではなく、トークンの検査(Python)にした(04 §4 に理由)。HLSL と C++ を同じ規則で見るため。
- .clang-tidy: random-generator-seed を外す(決まった種が方針)、tests/ では exception-escape を外す、wmain・D3D12SDK* は名前の検査から除く。
  .hlsli では auto・指示付き初期化子の検査を run_clang_tidy.py が外す(HLSL に無い書き方)。
- vcpkg の DXC(1.9.2602.24、lib_6_9 可)だけを使う(ADR-0004 に追記)。

## 次にやること
NEXT.md の先頭(T-0013 能力の確認)。

## 注意(次の Claude がハマりそうな所)
- **シミュのコードに `sqrt`・`pow`・`floor`・`lerp` などの名前の変数や関数を作らない**(ソースの検査が浮動小数点の関数として拒否する)。FxSqrt のように接頭辞を付ける。
- 描画だけの共通の HLSL は shaders/common ではなく shaders/render/ に置く(common は浮動小数点禁止)。
- **fixed.hlsli の約束**: 64bit の定数は `FX_U64(上位, 下位)`、関数は `FX_FN`、定数は `FX_CONST`。桁あふれしうる計算は符号なしで。
  自己テストに関数を足したら `FX_SELF_TEST_OUTPUT_COUNT` を増やす(要約 85c154e666febd92 が変わる)。
- `job.py tidy` は debug のビルドフォルダの compile_commands.json を使う。先に `job.py build`。
- tools/dxil_float_check/ という空のフォルダが PC に残っている(削除の許可が無く消せなかった。git には入らない)。
- **公開リポジトリ。** 鍵(.bicameral-runner/)・CLAUDE.local.md・PC 固有のパスをコミットしない。ゲームの中身も公開側に入れない(D-006)。
- Linux 側の整形: `python3 -m pip install --user clang-format==23.1.1 pyyaml` → `~/.local/bin/clang-format -i`(セッションごとに入れ直し)。
- `.github/` 以下は device_commit_files では書けない(保護)。device_bash の cp なら書ける。
- runner の結果 JSON(runner/logs/*.result.json)を cat しない。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる)。.ps1 は UTF-8(BOM 付き)+ CRLF。
  ただし `job.py raw` に渡す .ps1 は本文が埋め込まれるので BOM を付けない。
- Windows の Python がパイプに書く文字は CP932 になる。ビルドの中で呼ぶ Python は `sys.stdout.reconfigure(encoding="utf-8")` する。
