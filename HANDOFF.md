# HANDOFF.md — 前のチャットからの引き継ぎ

最終更新: 2026-09-30 / チケット: T-0010(整数の数学ライブラリ)— 完了

## 状態(3 行以内)
- M1「原理の確認」の最初のチケット T-0010 が完了。シミュの整数の数学を HLSL と C++ の共通のソース(shaders/common/fixed.hlsli)で書いた。
- CPU のテスト(fixed_test)で 128bit の組み込み関数とビット一致・誤差を実測。GPU で同じ列を計算して比べるのは T-0013。
- 図(docs/architecture/map.yaml)を新しい設計(06 の 1 刻みの段・02 のベイク)に合わせて直した。

## 動いているもの(確認方法つき)
- `job.py build`(debug / release とも警告なし)→ DXC が shaders/sim/fixed_selftest.hlsl を cs_6_8 で通す(bin/shaders/fixed_selftest.cso)。
- `job.py test`(4/4)。`job.py test -Filter fixed -Show` で誤差の実測と自己テストの要約(65536 件、85c154e666febd92)が出る。
- `python3 tools/archmap/archmap.py --check`(Linux 側)→ OK(リンク 12 個)。
- `job.py run -- --caps` は前回のまま(コードは変えていない)。

## 壊れている/未確認のもの
- fixed.hlsli の GPU での実行結果と速さは未確認(T-0013 で自己テストのシェーダーを走らせ、要約 85c154e666febd92 と比べる)。
- シェーダーのビルドは自己テスト 1 本だけの仮の仕組み(shaders/CMakeLists.txt)。一般化と DXIL の float 検査は T-0011。

## このチャットで決めたこと
- 丸めは積も割り算も 0 方向の切り捨てで統一。角度は 1 周 = 2^32(04 §2 に行を足した)。log・exp は Q32。
- exp2・log2 はビットごとの方法(定数表はソースに書く)。精度は 04 の目標を満たした。速さが足りなければ多項式へ(04「研究」)。

## 次にやること
NEXT.md の先頭(T-0011 シェーダーのビルドの仕組みと float の検査)。

## 注意(次の Claude がハマりそうな所)
- **fixed.hlsli の約束**: 64bit の定数は `FX_U64(上位, 下位)`、関数は `FX_FN`(C++ では constexpr)、定数は `FX_CONST`。桁あふれしうる計算は符号なしで。
  clang-format は `FX_NAMESPACE_BEGIN` を名前空間と知らないので、中身は字下げされない(それで良い)。
- 自己テストに関数を足したら `FX_SELF_TEST_OUTPUT_COUNT` を増やす。要約の値が変わるので、T-0013 の比べる値も合わせる。
- **公開リポジトリ。** 鍵(.bicameral-runner/)・CLAUDE.local.md・PC 固有のパスをコミットしない。ゲームの中身も公開側に入れない(D-006)。
- Linux 側の整形: `python3 -m pip install --user clang-format==23.1.1` → `~/.local/bin/clang-format -i`(セッションごとに入れ直し)。archmap の検査には pyyaml も入れる。
- `.github/` 以下は device_commit_files では書けない(保護)。runner/staging/ に書いてから device_bash で cp する。
- runner の結果 JSON(runner/logs/*.result.json)を cat しない。job.py の要約か .log を grep する。
- 外部コマンドを呼ぶ .ps1 で `$ErrorActionPreference='Stop'` にしない(PS 5.1 は stderr の警告で止まる)。.ps1 は UTF-8(BOM 付き)+ CRLF。
