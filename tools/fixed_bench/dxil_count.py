"""dxil_count.py — マイクロベンチのシェーダー(shaders/bench/fixed_bench.hlsl)の DXIL の命令数を、演算 1 回あたりで数える(T-0010、04 §6)。

入力はビルドが残す逆アセンブル(<ビルドフォルダ>/shaders/asm/bench/fixed_bench_<演算>.asm)。
ベンチの反復のループ(ラッチに反復回数のルート定数の読み込みがあるループ)の中の命令を数え、
「混ぜるだけ」(base32 / base64)の同じ数を引いて、展開の数(BENCH_UNROLL = 4)で割る。
- 静的: ループの本体に書かれている命令の数(内側のループは 1 回分)。
- 動的: 内側のループを回数だけ数えた見積もり。回数はラッチの比較の定数(`icmp eq i32 %i, 16` など)から取る。
phi は数えない(レジスタの割り当てで消える)。DXIL の 64bit の命令は 1 つと数える(GPU では 32bit の命令が複数になる)。

使い方: python3 tools/fixed_bench/dxil_count.py out/build/release/shaders/asm/bench
"""

import re
import sys
from pathlib import Path

UNROLL = 4
WIDTH_32 = {"base32", "add32", "mul32", "div32", "mulshift32", "recip32", "sqrt32", "sincos"}
FLOAT = {"fadd", "fmul", "fdiv"}
ORDER = ("base32 base64 add32 mul32 div32 add64 mul64 div64 fadd fmul fdiv mulshift32 mulshift64 mulfull128 divs64 "
         "divshift64 div128 recip32 recip64 recips64 makerecip64 sqrt32 sqrt64 exp2 log2 exp ln sincos hash64").split()

LABEL = re.compile(r"^; <label>:(\d+)")
BRANCH_TARGET = re.compile(r"label %(\d+)")
TRIP_COUNT = re.compile(r"icmp (?:eq|ne|ult|slt) i32 %\d+, (\d+)")


def read_blocks(path):
    """@Main の基本ブロックを、書かれている順に [(ラベル, 命令の行の一覧)] で返す。入口のブロックのラベルは '0'"""
    blocks = []
    inside = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("define void @Main"):
            inside = True
            blocks.append(("0", []))
            continue
        if not inside:
            continue
        if line.startswith("}"):
            break
        match = LABEL.match(line)
        if match:
            blocks.append((match.group(1), []))
        elif line.startswith("  ") and not line.startswith("   "):
            blocks[-1][1].append(line.strip())
    return blocks


def count(lines):
    return sum(1 for line in lines if " = phi " not in line)


def successors(blocks, position, index):
    return [position[target] for line in blocks[index][1] if line.startswith("br ")
            for target in BRANCH_TARGET.findall(line) if target in position]


def reaches(blocks, position, start, goal):
    seen, stack = set(), [start]
    while stack:
        index = stack.pop()
        if index == goal:
            return True
        if index in seen:
            continue
        seen.add(index)
        stack.extend(successors(blocks, position, index))
    return False


def find_loops(blocks):
    """後ろ向きの分岐(書かれている順で前のブロックへ)のうち、飛び先からまた戻ってこられるものを 1 つのループとし、
    (先頭, ラッチ) の番号の一覧を返す(ループの出口が前に書かれていることがあるので、戻ってこられるかで見分ける)"""
    position = {label: index for index, (label, _) in enumerate(blocks)}
    loops = []
    for index in range(len(blocks)):
        for target in successors(blocks, position, index):
            if target <= index and reaches(blocks, position, target, index):
                loops.append((target, index))
    return loops


def measure(path):
    blocks = read_blocks(path)
    loops = find_loops(blocks)
    main = [loop for loop in loops if any("cbufferLoadLegacy" in line for line in blocks[loop[1]][1])]
    if len(main) != 1:
        raise SystemExit(f"{path.name}: ベンチのループが見つからない({len(main)} 個)")
    head, latch = main[0]
    static = sum(count(blocks[index][1]) for index in range(head, latch + 1))
    dynamic = static
    inner = []
    for inner_head, inner_latch in loops:
        if (inner_head, inner_latch) == main[0] or not (head <= inner_head and inner_latch <= latch):
            continue
        body = sum(count(blocks[index][1]) for index in range(inner_head, inner_latch + 1))
        trips = [int(value) for line in blocks[inner_latch][1] for value in TRIP_COUNT.findall(line)]
        trip = trips[0] if trips else 1
        dynamic += body * (trip - 1)
        inner.append((body, trip if trips else None))
    return static, dynamic, inner


def main():
    folder = Path(sys.argv[1] if len(sys.argv) > 1 else "out/build/release/shaders/asm/bench")
    results = {name: measure(folder / f"fixed_bench_{name}.asm") for name in ORDER}
    print("| 演算 | DXIL 命令/回(静的) | DXIL 命令/回(動的な見積もり) | 内側のループ(本体の命令 × 回数。展開した 4 つ分) |")
    print("|---|---|---|---|")
    for name in ORDER:
        static, dynamic, inner = results[name]
        base = None if name in FLOAT or name.startswith("base") else results["base32" if name in WIDTH_32 else "base64"]
        if base:
            static -= base[0]
            dynamic -= base[1]
        loops = ", ".join(f"{body}×{trip if trip else '?'}" for body, trip in inner) or "なし"
        print(f"| {name} | {static / UNROLL:.1f} | {dynamic / UNROLL:.1f} | {loops} |")


if __name__ == "__main__":
    main()
