# T-0011 シェーダーのビルドの仕組み(sim / render の分離)・浮動小数点の検査・CI の持ち越し

- Status: Done
- 種類: 工学
- PC: 必須
- 見積もり: チャット 1 回分
- マイルストーン: M1 (docs/plan/ROADMAP.md)
- 設計: docs/design/04-numerics-determinism.md §4 / 決定: D-205(シミュは整数)・ADR-0004(DXC)・ADR-0005(CI)

## 目的(1〜2 行)
シミュのシェーダーと C++ のシミュのコードに浮動小数点が入ったら、**ビルドが落ちる**ようにする(D-205 を人の注意に頼らず守る)。
あわせて、以後のチケットがシェーダーを 1 行で足せるコンパイルの規則を作り、CI の持ち越し(BACKLOG)を片付ける。

## 完了条件(チェックできる形で)
- [x] `bicameral_add_shader()`(shaders/CMakeLists.txt): 1 行でシェーダーを足せる。`shaders/sim/` と `shaders/render/` で出力先を分ける(bin/shaders/sim/・bin/shaders/render/)。
      cs_6_8(ENTRY あり)と lib_6_8(Work Graphs。ENTRY なし)の両方。Debug は -Od -Zi -Qembed_debug
- [x] DXIL の浮動小数点の検査 `tools/dxil_float_check/`: sim のシェーダーは DXC の逆アセンブル(-Fc)を調べ、浮動小数点の型・命令があればビルドを落とす(.cso も消す)
- [x] ソースの浮動小数点の検査(C++ 側): `shaders/common/`・`shaders/sim/`・`engine/src/sim/` の float/double/half・asfloat・小数のリテラルでビルドを落とす
- [x] 検査が本当に落ちることのテスト(ctest): わざと float を使うシェーダー(型変換・asfloat の抜け道)とソースで「落ちる」、既存の自己テストで「通る」
- [x] CI: ilammy/msvc-dev-cmd(Node 20)をやめて vswhere + vcvars64 に / docs ジョブを ubuntu-26.04 に固定 / clang-tidy を CI で回す
      (PC は `job.py tidy` = VS 同梱の clang-tidy 22.1.3、CI は pip の clang-tidy 22.1.8。既存の 41 件は直すか、理由を書いて設定で外した)
- [x] vcpkg の DXC の版と lib_6_9 の対応を確かめて ADR-0004 に書く(BACKLOG)

## メモ・参考
- 逆アセンブルで検査するもの: 型 `half`・`float`・`double`(ベクトル `<N x float>` や `dx.op.*.f32` の宣言も含む)と、
  命令 fadd fsub fmul fdiv frem fneg fcmp fpext fptrunc sitofp uitofp fptosi fptoui。コメント行(`;`)とメタデータ行(`!`)は見ない。
  `asfloat` は bitcast i32 → float になり、型の検査で捕まる(2026-09-30 に確かめた)。
- C++ 側は 04 §4 では「clang-tidy のカスタム検査」としていたが、clang-tidy のカスタム検査はプラグインのビルドが要り、HLSL も見られない。
  共通のソース(.hlsli)を両方の言語でまとめて見るため、トークンの検査(Python)にした(04 §4 を直す)。

## 作業ログ(チャットごとに 3〜5 行、新しいものを下に)
- 2026-09-30: bicameral_add_shader()・DXIL の検査・ソースの検査・ctest の float_check_* 5 本(9/9 通過、debug / release)。
  わざと sim に asfloat のシェーダーを足すと、両方の検査がビルドを落とし .cso が消えることを確かめた(戻した)。
  clang-tidy を job.py tidy と CI に。HLSL と共通の .hlsli では auto・指示付き初期化子の検査を外す(HLSL に無い書き方)。
  外した・変えた設定: random-generator-seed(決まった種が方針)、tests の exception-escape、wmain・D3D12SDK* の名前、Win32 の文字コード変換。
  vcpkg の DXC は 1.9.2602.24 で lib_6_9 可(ADR-0004)。Debug のシェーダーは -Od -Zi -Qembed_debug。
