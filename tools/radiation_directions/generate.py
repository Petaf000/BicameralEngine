#!/usr/bin/env python3
"""放射の線の向きの組を作る(T-0584・ADR-0262)。出力: shaders/common/radiation_directions.hlsli。

向きの組 = RD_GROUP_COUNT 組 × RD_GROUP_DIRECTIONS 本。1 組は半球(線は両向きなので半球で全部の向き)を等しい立体角に分けた
Fibonacci の点で、組ごとに z と方位をずらす(全部の組を合わせると細かい層別の組)。刻みごとに 1 組を選ぶ(R6 のハッシュ)。
各向き: 整数の向き (Dx, Dy, Dz)(長さ約 1024)・線 1 本の見え方の割合 F(Q16。面 A = セルの面 0.25 m² の単位)・16 m に当たる板の数。
F = h² × |d̂_a| × (2 ÷ 本数)。h = 線の間隔(セル 2 + 2^-16。RD_LINE_SPACING_Q16)、a = 向きの一番大きい軸。
浮動小数点はこの道具だけで使い、出力は整数だけ(シミュのコードに浮動小数点を書かない。04 §4)。
使い方: python3 tools/radiation_directions/generate.py > shaders/common/radiation_directions.hlsli
"""
import math

GROUPS = 16
PER_GROUP = 32
SCALE = 1024
SPACING_Q16 = 2 * 65536 + 1
REACH_CELLS = 32  # 16 m = レベル 0 のセル 32 個(D-443 ③)
GOLDEN = (math.sqrt(5.0) - 1.0) / 2.0


def directions():
    rows = []
    for group in range(GROUPS):
        jitter = (group + 0.5) / GROUPS
        for index in range(PER_GROUP):
            z = 1.0 - (index + jitter) / PER_GROUP
            r = math.sqrt(max(0.0, 1.0 - z * z))
            phi = 2.0 * math.pi * ((index * GOLDEN + group / GROUPS) % 1.0)
            d = (r * math.cos(phi), r * math.sin(phi), z)
            ints = [int(round(c * SCALE)) for c in d]
            norm = math.sqrt(sum(c * c for c in ints))
            axis = max(range(3), key=lambda k: (abs(ints[k]), -k))
            cosine = abs(ints[axis]) / norm
            h = SPACING_Q16 / 65536.0
            view = h * h * cosine * 2.0 / PER_GROUP
            view_q16 = int(view * 65536.0)
            reach = int(REACH_CELLS * cosine)
            rows.append((ints[0], ints[1], ints[2], view_q16, reach))
    return rows


def main():
    rows = directions()
    print("// radiation_directions.hlsli — 放射の線の向きの組(T-0584・ADR-0262)。tools/radiation_directions/generate.py が作る。手で直さない。")
    print("// 1 行 = 1 本: 整数の向き Dx, Dy, Dz・線 1 本の見え方の割合 F(Q16)・16 m に当たる一番大きい軸の板の数。")
    print("// RD_GROUP_COUNT 組 × RD_GROUP_DIRECTIONS 本。1 組で半球の全部の向き(線は両向き)。")
    print("#ifndef BICAMERAL_RADIATION_DIRECTIONS_HLSLI")
    print("#define BICAMERAL_RADIATION_DIRECTIONS_HLSLI")
    print()
    print("RX_NAMESPACE_BEGIN")
    print()
    print(f"FX_CONST uint32_t RD_GROUP_COUNT = {GROUPS};")
    print(f"FX_CONST uint32_t RD_GROUP_DIRECTIONS = {PER_GROUP};")
    print(f"FX_CONST uint32_t RD_DIRECTION_WORDS = 5;")
    print("// 1 行 = 1 本の並びを保つ(整形しない)")
    print("// clang-format off")
    print(f"FX_CONST int32_t RD_DIRECTIONS[{GROUPS * PER_GROUP * 5}] = {{")
    for row in rows:
        print("    " + ", ".join(str(v) for v in row) + ",")
    print("};")
    print("// clang-format on")
    print()
    print("RX_NAMESPACE_END")
    print()
    print("#endif  // BICAMERAL_RADIATION_DIRECTIONS_HLSLI")


if __name__ == "__main__":
    main()
