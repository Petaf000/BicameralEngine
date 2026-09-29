#!/usr/bin/env python3
"""シミュのソース(HLSL と C++ の両方)に浮動小数点が無いことを確かめる(docs/design/04-numerics-determinism.md §4・D-205)。

見る場所(ビルドの中で毎回。engine/CMakeLists.txt の bicameral_float_source_check):
  shaders/common/  … GPU と CPU の共通のソース(fixed.hlsli など)。C++ からも読まれるので、DXIL の検査だけでは足りない
  shaders/sim/     … シミュのシェーダー
  engine/src/sim/  … CPU リファレンス・ベイク(C++)
コメントと文字列の中は見ない。違反があれば場所を表示して終了コード 1。

なぜトークンで見るのか: C++ の `std::sqrt(int)` は double を返し、`1.5` は double のリテラル。どちらも型の名前を書かずに
浮動小数点が入る。clang-tidy のカスタム検査はプラグインのビルドが要り、HLSL も読めないので、両方の言語を同じ規則で見る。
"""
import argparse
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".hlsl", ".hlsli", ".h", ".hpp", ".cpp", ".inl"}

# --- 字句(コメント・文字列を飛ばし、数と名前を取り出す)---------------------------------------------
TOKEN = re.compile(
    r"(?P<block>/\*.*?\*/)"
    r"|(?P<line>//[^\n]*)"
    r"|(?P<string>\"(?:\\.|[^\"\\\n])*\")"
    r"|(?P<char>'(?:\\.|[^'\\\n])+')"
    # 数: 16 進(16 進の浮動小数点 0x1.8p3 も)と 10 進。桁区切り ' も許す
    r"|(?P<number>0[xX][0-9A-Fa-f'.]+(?:[pP][+-]?[0-9]+)?[A-Za-z]*|[0-9][0-9']*(?:\.[0-9']*)?(?:[eE][+-]?[0-9]+)?[A-Za-z]*|\.[0-9]+(?:[eE][+-]?[0-9]+)?[A-Za-z]*)"
    r"|(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"|(?P<include>#\s*include\s*<[^>\n]+>)",
    re.DOTALL)

# 浮動小数点の型(HLSL のベクトル・行列 float3・float4x4 も)と、浮動小数点を作る組み込み関数
FLOAT_TYPE_NAME = re.compile(r"^(half|float|double|min16float|min10float)([1-4](x[1-4])?)?$")
FLOAT_FUNCTIONS = {
    "asfloat", "asdouble", "f16tof32", "f32tof16",
    # <cmath> の関数。整数を渡しても double になる(C++)。HLSL でも整数から float へ暗黙に変換される
    "sqrt", "cbrt", "pow", "exp", "exp2", "expm1", "log", "log2", "log10", "log1p",
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh",
    "floor", "ceil", "round", "trunc", "fmod", "modf", "frexp", "ldexp", "hypot", "lerp",
    "fma", "fabs", "rsqrt", "saturate", "smoothstep", "frac", "rcp",
}
FLOAT_HEADERS = re.compile(r"<\s*(cmath|math\.h|cfloat|float\.h|numbers)\s*>")


def IsFloatLiteral(text: str) -> bool:
    if text[:2] in ("0x", "0X"):
        return "." in text or "p" in text or "P" in text
    body = text.rstrip("uUlLzZ")
    return "." in body or "e" in body.lower() or body.lower().endswith(("f", "h"))


def FindViolations(text: str) -> list[tuple[int, str]]:
    """(行番号, 見つかったもの) の一覧。"""
    violations = []
    for match in TOKEN.finditer(text):
        kind = match.lastgroup
        value = match.group(0)
        found = None
        if kind == "number" and IsFloatLiteral(value):
            found = f"小数のリテラル {value}"
        elif kind == "name" and FLOAT_TYPE_NAME.match(value):
            found = f"浮動小数点の型 {value}"
        elif kind == "name" and value in FLOAT_FUNCTIONS:
            found = f"浮動小数点になる関数 {value}"
        elif kind == "include" and FLOAT_HEADERS.search(value):
            found = f"浮動小数点のヘッダ {value}"
        if found:
            violations.append((text.count("\n", 0, match.start()) + 1, found))
    return violations


def CollectFiles(paths: list[pathlib.Path]) -> list[pathlib.Path]:
    files = []
    for path in paths:
        if path.is_file():
            files.append(path)
        elif path.is_dir():
            files += sorted(p for p in path.rglob("*") if p.suffix in SOURCE_SUFFIXES)
    return files


def Main() -> int:
    # Windows ではパイプへの出力が CP932 になり、ninja・ランナー・CI のログ(UTF-8)で文字化けする
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="+", type=pathlib.Path, help="ファイルかフォルダ(無いフォルダは飛ばす)")
    parser.add_argument("--stamp", type=pathlib.Path, help="通ったら更新するファイル(ビルドの依存用)")
    parser.add_argument("--expect-reject", action="store_true",
                        help="テスト用: すべてのファイルで違反が見つかったら成功")
    args = parser.parse_args()

    files = CollectFiles(args.paths)
    total = 0
    missed = []
    for path in files:
        violations = FindViolations(path.read_text(encoding="utf-8", errors="replace"))
        if args.expect_reject:
            if not violations:
                missed.append(path)
            continue
        for line, found in violations:
            print(f"{path}:{line}: {found}")
        total += len(violations)

    if args.expect_reject:
        for path in missed:
            print(f"source_float_check: {path}: 浮動小数点を含むはずなのに見つからなかった(検査の抜け)")
        if not files:
            print("source_float_check: 調べるファイルが無い")
            return 1
        return 1 if missed else 0

    if total:
        print(f"source_float_check: シミュのソースに浮動小数点が {total} か所ある(D-205・04 §4)。"
              "fixed.hlsli の整数の関数を使う。描画のコードなら shaders/render/ か engine/src/render/ に置く。")
        return 1
    if args.stamp:
        args.stamp.parent.mkdir(parents=True, exist_ok=True)
        args.stamp.write_text(f"{len(files)} files OK\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(Main())
