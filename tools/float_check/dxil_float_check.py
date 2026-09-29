#!/usr/bin/env python3
"""シミュ用シェーダーの DXIL に浮動小数点が無いことを確かめる(docs/design/04-numerics-determinism.md §4・D-205)。

入力は DXC の逆アセンブル(`dxc ... -Fc <asm>`)。ビルドの中で、shaders/sim/ の各シェーダーをコンパイルした直後に呼ばれる
(shaders/CMakeLists.txt の bicameral_add_shader)。浮動小数点の型か命令が 1 つでもあれば、場所を表示して終了コード 1 を返し、
--remove で渡された .cso を消す(壊れた成果物を実行時に読ませないため)。

なぜ逆アセンブルを見るのか: ソースに float と書かなくても、組み込み関数や型変換(asfloat・暗黙の変換)で
浮動小数点の命令は入りうる。GPU が実際に実行するのは DXIL なので、最後の形で確かめる。
"""
import argparse
import pathlib
import re
import sys

# --- 見るもの -------------------------------------------------------------------------------------
# 浮動小数点の型(スカラー・<N x float>・dx.op.*.f32 の宣言の引数や戻り値にも現れる)
FLOAT_TYPES = re.compile(r"\b(half|float|double)\b")
# 浮動小数点の命令(LLVM IR の命令名)。型の検査だけでもほぼ捕まるが、何が悪いかを表示で分かりやすくする
FLOAT_OPS = re.compile(
    r"\b(fadd|fsub|fmul|fdiv|frem|fneg|fcmp|fpext|fptrunc|sitofp|uitofp|fptosi|fptoui)\b")
# dx.op の浮動小数点の多重定義(例: @dx.op.unary.f32)
FLOAT_OVERLOADS = re.compile(r"@dx\.op\.[A-Za-z0-9]+\.f(16|32|64)\b")


def StripComment(line: str) -> str:
    """行末の `; ...`(DXC が付ける説明)を落とす。IR の本文に ; は出てこない。"""
    position = line.find(";")
    return line if position < 0 else line[:position]


def FindViolations(asmText: str) -> list[tuple[int, str, str]]:
    """(行番号, 何が見つかったか, 行) の一覧を返す。"""
    violations = []
    for number, rawLine in enumerate(asmText.splitlines(), start=1):
        stripped = rawLine.lstrip()
        # コメント行(ヘッダの入出力の表など)とメタデータ行(デバッグ情報を含む)は見ない。
        # 浮動小数点を実際に使えば、命令・型の定義・関数の宣言のどれかに必ず現れる
        if stripped.startswith(";") or stripped.startswith("!"):
            continue
        # datalayout の "f32:32" などは型ではない
        if stripped.startswith("target datalayout"):
            continue
        body = StripComment(rawLine)
        for pattern in (FLOAT_OPS, FLOAT_OVERLOADS, FLOAT_TYPES):
            match = pattern.search(body)
            if match:
                violations.append((number, match.group(0), rawLine.strip()))
                break
    return violations


def Main() -> int:
    # Windows ではパイプへの出力が CP932 になり、ninja・ランナー・CI のログ(UTF-8)で文字化けする
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("asm", type=pathlib.Path, help="dxc -Fc の出力")
    parser.add_argument("--source", default="", help="表示用のシェーダーの名前")
    parser.add_argument("--remove", type=pathlib.Path, action="append", default=[],
                        help="違反があったら消すファイル(.cso)")
    parser.add_argument("--expect-reject", action="store_true",
                        help="テスト用: 違反が見つかったら成功、見つからなければ失敗")
    args = parser.parse_args()

    name = args.source or str(args.asm)
    violations = FindViolations(args.asm.read_text(encoding="utf-8", errors="replace"))

    if args.expect_reject:
        if violations:
            print(f"dxil_float_check: {name}: 期待どおり {len(violations)} か所で拒否")
            return 0
        print(f"dxil_float_check: {name}: 浮動小数点を含むはずなのに見つからなかった(検査の抜け)")
        return 1

    if not violations:
        return 0

    print(f"dxil_float_check: {name}: シミュのシェーダーに浮動小数点がある(D-205・04 §4)。{len(violations)} か所:")
    for number, found, line in violations[:20]:
        print(f"  {args.asm}:{number}: [{found}] {line[:160]}")
    if len(violations) > 20:
        print(f"  ...ほか {len(violations) - 20} か所")
    print("  直し方: fixed.hlsli の整数の関数を使う。描画のシェーダーなら shaders/render/ に置く。")
    for path in args.remove:
        path.unlink(missing_ok=True)
    return 1


if __name__ == "__main__":
    sys.exit(Main())
