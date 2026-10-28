#!/usr/bin/env python3
"""シミュのシェーダーの DXIL の局所の配列(alloca)の数を数え、上限の表と比べる(T-1233)。

なぜ: シミュの共通のコード(reaction.hlsli など)で、書き換えた構造体の配列を動的な添字で読み書きすると、DXIL に
`alloca`(1 スレッドの局所の配列)が残る。GPU ではレジスタに載らず局所メモリ(L1/L2/DRAM)へ出て遅くなり、HW の Work Graph が
止まった実例もある(CLAUDE.md §4・T-0397)。今は物理・多重解像度の世界などに残っているので 0 は求めず、シェーダーごとの数が
上限の表(limits.txt)を超えたら落とす(増やさない歯止め)。減らしたら --update で表を下げる。

データ: ビルドが bicameral_add_shader で書く asm(<build>/shaders/asm/sim/*.asm。dxc -Fc)を読むだけ。
数え方は release の dxc の出力(debug は -Zi・BICAMERAL_GPU_DEBUG で数が変わるので、Release 以外は数えずに通す)。

使い方:
  dxil_alloca_check.py <asm のフォルダ> --limits limits.txt [--config Release]   検査(超えたら 1)
  dxil_alloca_check.py <asm のフォルダ> --limits limits.txt --update            表を今の数で書き直す
  dxil_alloca_check.py <asm のフォルダ> --report                                 数の多い順に出す
"""

import argparse
import pathlib
import re
import sys

ALLOCA = re.compile(r"=\s*alloca\b")


def CountAllocas(asmDir):
    counts = {}
    for path in sorted(pathlib.Path(asmDir).glob("*.asm")):
        text = path.read_text(encoding="utf-8", errors="replace")
        counts[path.stem] = len(ALLOCA.findall(text))

    return counts


def ReadLimits(path):
    limits = {}
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue

        name, count = line.split()
        limits[name] = int(count)

    return limits


def WriteLimits(path, counts):
    lines = [
        "# シミュのシェーダーごとの DXIL の局所の配列(alloca)の数の上限(T-1233。tools/dxil_alloca/dxil_alloca_check.py)。",
        "# release の dxc の出力で数える。ここに無いシェーダーは 0。減らしたら --update で下げる(上げる時は理由をチケットに書く)。",
    ]
    lines += [f"{name} {count}" for name, count in sorted(counts.items()) if count > 0]
    pathlib.Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("asm_dir")
    parser.add_argument("--limits")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--report", action="store_true")
    args = parser.parse_args()

    counts = CountAllocas(args.asm_dir)
    if not counts:
        print(f"dxil_alloca_check: asm が無い: {args.asm_dir}")
        return 1

    # --- 数を出すだけ ---
    if args.report:
        total = sum(counts.values())
        print(f"{len(counts)} シェーダー・alloca 合計 {total}")
        for name, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
            if count > 0:
                print(f"{count:5d} {name}")
        return 0

    if not args.limits:
        parser.error("--limits が要る")

    if args.update:
        WriteLimits(args.limits, counts)
        print(f"dxil_alloca_check: 上限の表を書き直した({sum(counts.values())} 個)")
        return 0

    if args.config.lower() != "release":
        print(f"dxil_alloca_check: {args.config} は数えない(release の dxc の出力で数える)")
        return 0

    # --- 上限と比べる ---
    limits = ReadLimits(args.limits)
    over = [(name, count, limits.get(name, 0)) for name, count in counts.items() if count > limits.get(name, 0)]
    below = [(name, count, limits[name]) for name, count in counts.items() if name in limits and count < limits[name]]
    for name, count, limit in sorted(over):
        print(f"dxil_alloca_check: 増えた {name}: {count}(上限 {limit})。成分などの配列を動的な添字で読み書きしていないか")
    for name, count, limit in sorted(below):
        print(f"dxil_alloca_check: 減った {name}: {count}(上限 {limit})。--update で表を下げる")

    print(f"dxil_alloca_check: {len(counts)} シェーダー・alloca 合計 {sum(counts.values())}・超えた {len(over)}")
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main())
