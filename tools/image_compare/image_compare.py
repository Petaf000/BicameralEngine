#!/usr/bin/env python3
"""画面の画像を基準の画像と比べる(docs/design/16-debug-test.md §5。T-0025)。

データの流れ: ランタイムが `bicameral --screenshot-tick <t> --screenshot <bmp>` で決まった刻みの画面を BMP に書く
→ ここが tests/images/ の基準の PNG と比べ、ずれた画素を赤くした画像(PNG)を書いて、許容を超えたら終了コード 1。

描画は浮動小数点なので、GPU・ドライバが変わると少しずれうる(シミュは整数でビット一致。描画は違う。CLAUDE.md §1-3)。
だから「全部一致」ではなく、次の 2 つの許容で比べる:
  - 色の許容(--channel-tolerance): 画素の R・G・B のどれかの差がこれを超えたら「ずれた画素」(既定 8 / 255)
  - 数の許容(--max-differing-percent): ずれた画素が全体のこの割合を超えたら失敗(既定 0.1 %)
同じ機械・同じドライバなら差は 0 のはず(結果は毎回 0 か、それに近いことを出す)。大きさが違えば比べずに失敗。

外部のライブラリを使わない(Python の標準だけ。ランナーと CI と Linux の作業場のどこでも動くように)。
読めるもの: BMP(24/32 bit・上から下と下から上)・PNG(8 bit の RGB / RGBA・インターレース無し)。書くのは PNG。

使い方:
  image_compare.py compare <実際の画像> <基準の PNG> [--diff <差の PNG>] [--channel-tolerance 8] [--max-differing-percent 0.1]
  image_compare.py update  <実際の画像> <基準の PNG>     基準を作る・置き換える(PNG の RGB で書く)
  image_compare.py selftest                               比べ方そのものの試験(GPU 無し)
"""
import argparse
import pathlib
import struct
import sys
import tempfile
import zlib

DEFAULT_CHANNEL_TOLERANCE = 8
DEFAULT_MAX_DIFFERING_PERCENT = 0.1


# --- 画像(幅・高さ・RGB の bytes。上の行から)-----------------------------------------------------------
class Image:
    def __init__(self, width, height, rgb):
        assert len(rgb) == width * height * 3
        self.width = width
        self.height = height
        self.rgb = bytes(rgb)


def read_bmp(data):
    if data[:2] != b"BM":
        raise ValueError("BMP ではない")

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width, height, _planes, bits, compression = struct.unpack_from("<iiHHI", data, 18)
    if bits not in (24, 32) or compression not in (0, 3):
        raise ValueError(f"読めない BMP(ビット {bits}・圧縮 {compression})")

    top_down = height < 0
    height = abs(height)
    pixel_bytes = bits // 8
    stride = (width * pixel_bytes + 3) & ~3
    rows = []
    for y in range(height):
        source_row = y if top_down else height - 1 - y
        start = pixel_offset + source_row * stride
        row = data[start:start + width * pixel_bytes]
        # BGR(A) → RGB
        blue = row[0::pixel_bytes]
        green = row[1::pixel_bytes]
        red = row[2::pixel_bytes]
        rgb = bytearray(width * 3)
        rgb[0::3] = red
        rgb[1::3] = green
        rgb[2::3] = blue
        rows.append(bytes(rgb))

    return Image(width, height, b"".join(rows))


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _unfilter_row(kind, row, previous, pixel_bytes):
    if kind == 0:
        return bytes(row)
    if kind == 2:  # Up(書くときに使う。速い道)
        return bytes((x + b) & 0xFF for x, b in zip(row, previous))

    out = bytearray(row)
    for i in range(len(out)):
        a = out[i - pixel_bytes] if i >= pixel_bytes else 0
        b = previous[i]
        c = previous[i - pixel_bytes] if i >= pixel_bytes else 0
        if kind == 1:
            out[i] = (out[i] + a) & 0xFF
        elif kind == 3:
            out[i] = (out[i] + ((a + b) >> 1)) & 0xFF
        elif kind == 4:
            out[i] = (out[i] + _paeth(a, b, c)) & 0xFF
        else:
            raise ValueError(f"PNG のフィルタ {kind} は無い")

    return bytes(out)


def read_png(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("PNG ではない")

    position = 8
    header = None
    compressed = []
    while position < len(data):
        length, kind = struct.unpack_from(">I4s", data, position)
        body = data[position + 8:position + 8 + length]
        position += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed.append(body)
        elif kind == b"IEND":
            break

    width, height, depth, color, _compression, _filter, interlace = header
    if depth != 8 or color not in (2, 6) or interlace != 0:
        raise ValueError(f"読めない PNG(深さ {depth}・色 {color}・インターレース {interlace})")

    pixel_bytes = 3 if color == 2 else 4
    stride = width * pixel_bytes
    raw = zlib.decompress(b"".join(compressed))
    previous = bytes(stride)
    rows = []
    for y in range(height):
        start = y * (stride + 1)
        row = _unfilter_row(raw[start], raw[start + 1:start + 1 + stride], previous, pixel_bytes)
        previous = row
        if pixel_bytes == 4:
            rgb = bytearray(width * 3)
            rgb[0::3] = row[0::4]
            rgb[1::3] = row[1::4]
            rgb[2::3] = row[2::4]
            row = bytes(rgb)
        rows.append(row)

    return Image(width, height, b"".join(rows))


def write_png(path, image):
    stride = image.width * 3
    previous = bytes(stride)
    filtered = []
    for y in range(image.height):
        row = image.rgb[y * stride:(y + 1) * stride]
        # Up フィルタ(上の行との差)。描画の画像は縦に滑らかなので、何もしないより小さくなる
        filtered.append(b"\x02" + bytes((x - b) & 0xFF for x, b in zip(row, previous)))
        previous = row

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", image.width, image.height, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(b"".join(filtered), 9)) +
           chunk(b"IEND", b""))
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def read_image(path):
    data = pathlib.Path(path).read_bytes()
    if data[:2] == b"BM":
        return read_bmp(data)

    return read_png(data)


# --- 比べる ------------------------------------------------------------------------------------------------
class Comparison:
    def __init__(self):
        self.size_mismatch = None  # 大きさが違えば説明の文字列
        self.pixel_count = 0
        self.differing = 0  # 色の許容を超えた画素
        self.changed = 0  # 少しでも違う画素(許容の内も)
        self.max_difference = 0
        self.box = None  # ずれた画素を囲む箱 (x0, y0, x1, y1)。含む
        self.diff = None  # 差の画像

    def differing_percent(self):
        return 100.0 * self.differing / self.pixel_count if self.pixel_count else 0.0

    def passed(self, max_differing_percent):
        return self.size_mismatch is None and self.differing_percent() <= max_differing_percent


def compare(actual, baseline, channel_tolerance, make_diff=True):
    result = Comparison()
    if (actual.width, actual.height) != (baseline.width, baseline.height):
        result.size_mismatch = f"大きさが違う: 実際 {actual.width}x{actual.height}・基準 {baseline.width}x{baseline.height}"
        return result

    result.pixel_count = actual.width * actual.height
    if actual.rgb == baseline.rgb:
        if make_diff:
            result.diff = _dim(baseline)
        return result

    # 差の画像: 同じ画素は基準を暗い灰色に、許容の内の違いは黄色、許容を超えた画素は赤(どこがずれたか見える)
    diff = bytearray(_dim(baseline).rgb) if make_diff else None
    a, b = actual.rgb, baseline.rgb
    x0, y0, x1, y1 = actual.width, actual.height, -1, -1
    for index in range(result.pixel_count):
        offset = index * 3
        if a[offset:offset + 3] == b[offset:offset + 3]:
            continue

        difference = max(abs(a[offset] - b[offset]), abs(a[offset + 1] - b[offset + 1]), abs(a[offset + 2] - b[offset + 2]))
        result.changed += 1
        result.max_difference = max(result.max_difference, difference)
        exceeds = difference > channel_tolerance
        if make_diff:
            diff[offset:offset + 3] = b"\xff\x00\x00" if exceeds else b"\xc0\xc0\x00"
        if not exceeds:
            continue

        result.differing += 1
        x, y = index % actual.width, index // actual.width
        x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)

    if result.differing:
        result.box = (x0, y0, x1, y1)
    if make_diff:
        result.diff = Image(actual.width, actual.height, diff)

    return result


def _dim(image):
    # 輝度の 1/3 の灰色(差の画像の背景)
    rgb = image.rgb
    gray = bytes((rgb[i] * 77 + rgb[i + 1] * 150 + rgb[i + 2] * 29) // 768 for i in range(0, len(rgb), 3))
    out = bytearray(len(rgb))
    out[0::3] = gray
    out[1::3] = gray
    out[2::3] = gray
    return Image(image.width, image.height, out)


def describe(result, channel_tolerance, max_differing_percent):
    if result.size_mismatch:
        return result.size_mismatch

    text = (f"ずれた画素 {result.differing} / {result.pixel_count}({result.differing_percent():.4f} %。"
            f"許容 {max_differing_percent} %・色の差 > {channel_tolerance})・少しでも違う画素 {result.changed}・"
            f"色の差の最大 {result.max_difference}")
    if result.box:
        text += f"・ずれた範囲 ({result.box[0]}, {result.box[1]}) - ({result.box[2]}, {result.box[3]})"
    return text


# --- コマンド ------------------------------------------------------------------------------------------------
def command_compare(arguments):
    actual_path = pathlib.Path(arguments.actual)
    baseline_path = pathlib.Path(arguments.baseline)
    if not actual_path.exists():
        print(f"image_compare: 実際の画像が無い: {actual_path}(ランタイムが写せなかった。そのテストのログを見る)")
        return 1

    actual = read_image(actual_path)
    if not baseline_path.exists():
        # 基準が無い: 失敗にして、作り方を出す(勝手に基準を作らない。見て確かめてから入れる)
        candidate = pathlib.Path(arguments.diff).with_name(baseline_path.name) if arguments.diff else None
        if candidate:
            write_png(candidate, actual)
        print(f"image_compare: 基準の画像が無い: {baseline_path}")
        print(f"  見て良ければ作る: python tools/image_compare/image_compare.py update {actual_path} {baseline_path}")
        if candidate:
            print(f"  (今の画面の PNG: {candidate})")
        return 1

    baseline = read_image(baseline_path)
    result = compare(actual, baseline, arguments.channel_tolerance, make_diff=bool(arguments.diff))
    if arguments.diff and result.diff:
        write_png(arguments.diff, result.diff)

    passed = result.passed(arguments.max_differing_percent)
    print(f"image_compare: {'OK' if passed else 'FAILED'}  {baseline_path.name}  "
          f"{describe(result, arguments.channel_tolerance, arguments.max_differing_percent)}")
    if arguments.diff and result.diff:
        print(f"  差の画像: {arguments.diff}(赤 = 許容を超えた・黄 = 許容の内の違い)")
    if not passed:
        print(f"  意図した変化なら基準を置き換える: python tools/image_compare/image_compare.py update {actual_path} {baseline_path}")
    return 0 if passed else 1


def command_update(arguments):
    actual = read_image(arguments.actual)
    write_png(arguments.baseline, actual)
    print(f"image_compare: 基準を書いた: {arguments.baseline}({actual.width}x{actual.height})")
    return 0


# --- 自己試験(GPU 無し。比べ方と読み書きが正しいか)---------------------------------------------------------
def _make_image(width, height, seed):
    rgb = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            offset = (y * width + x) * 3
            rgb[offset:offset + 3] = bytes(((x * 7 + seed) & 0xFF, (y * 5 + seed) & 0xFF, ((x ^ y) * 3) & 0xFF))
    return Image(width, height, rgb)


def _make_bmp(image, top_down, bits):
    pixel_bytes = bits // 8
    stride = (image.width * pixel_bytes + 3) & ~3
    rows = []
    for y in range(image.height):
        source = y if top_down else image.height - 1 - y
        row = bytearray()
        for x in range(image.width):
            r, g, b = image.rgb[(source * image.width + x) * 3:(source * image.width + x) * 3 + 3]
            row += bytes((b, g, r)) + (b"\xff" if pixel_bytes == 4 else b"")
        rows.append(bytes(row) + bytes(stride - len(row)))
    pixels = b"".join(rows)
    info = struct.pack("<IiiHHIIiiII", 40, image.width, -image.height if top_down else image.height, 1, bits, 0,
                       len(pixels), 2835, 2835, 0, 0)
    return b"BM" + struct.pack("<IHHI", 14 + len(info) + len(pixels), 0, 0, 14 + len(info)) + info + pixels


def command_selftest(_arguments):
    failures = []

    def expect(condition, text):
        if not condition:
            failures.append(text)

    image = _make_image(37, 11, 3)  # 幅を 4 の倍数にしない(BMP の行の詰め物を通す)
    for top_down in (True, False):
        for bits in (24, 32):
            expect(read_bmp(_make_bmp(image, top_down, bits)).rgb == image.rgb, f"BMP {bits} bit 上から下 {top_down}")

    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "a.png"
        write_png(path, image)
        expect(read_image(path).rgb == image.rgb, "PNG を書いて読むと同じ")

    # 全部のフィルタを読めるか(Sub・Average・Paeth は書かないので、ここで作る)
    for kind in range(5):
        stride = image.width * 3
        raw = bytearray()
        previous = bytes(stride)
        for y in range(image.height):
            row = image.rgb[y * stride:(y + 1) * stride]
            filtered = bytearray(row)
            for i in range(stride):
                a = row[i - 3] if i >= 3 else 0
                b = previous[i]
                c = previous[i - 3] if i >= 3 else 0
                predictor = [0, a, b, (a + b) >> 1, _paeth(a, b, c)][kind]
                filtered[i] = (row[i] - predictor) & 0xFF
            raw += bytes([kind]) + filtered
            previous = row
        # 組み立てる(CRC はこの読み手が見ないので 0)
        body = zlib.compress(bytes(raw))
        png = (b"\x89PNG\r\n\x1a\n" + struct.pack(">I4s", 13, b"IHDR") +
               struct.pack(">IIBBBBB", image.width, image.height, 8, 2, 0, 0, 0) + b"\0\0\0\0" +
               struct.pack(">I4s", len(body), b"IDAT") + body + b"\0\0\0\0" + struct.pack(">I4s", 0, b"IEND") + b"\0\0\0\0")
        expect(read_png(png).rgb == image.rgb, f"PNG のフィルタ {kind}")

    # 同じ → ずれ 0
    same = compare(image, image, 8)
    expect(same.differing == 0 and same.changed == 0 and same.passed(0.0), "同じ画像は差 0")

    # 許容の内(全部の画素を +8)→ 通る。+9 → 全部ずれる
    def shifted(amount):
        return Image(image.width, image.height, bytes(min(255, v + amount) for v in image.rgb))

    small = compare(shifted(8), image, 8)
    expect(small.differing == 0 and small.passed(0.0), "色の差 8 は許容の内")
    large = compare(shifted(9), image, 8)
    expect(large.differing > 0 and not large.passed(0.1), "色の差 9 は許容を超える")

    # 1 画素だけ大きく違う → 数の許容の内なら通る。箱はその画素
    one = bytearray(image.rgb)
    one[(5 * image.width + 20) * 3] ^= 0xFF
    single = compare(Image(image.width, image.height, one), image, 8)
    expect(single.differing == 1 and single.box == (20, 5, 20, 5), "1 画素の場所")
    expect(single.passed(100.0 / (image.width * image.height)) and not single.passed(0.1), "数の許容")
    expect(single.diff.rgb[(5 * image.width + 20) * 3:(5 * image.width + 20) * 3 + 3] == b"\xff\x00\x00", "差の画像が赤")

    # 大きさが違う → 比べずに失敗
    expect(not compare(_make_image(36, 11, 3), image, 255).passed(100.0), "大きさが違えば失敗")

    for failure in failures:
        print(f"FAILED: {failure}")
    if not failures:
        print("image_compare selftest: OK")
    return 1 if failures else 0


def main():
    # ctest とランナーのログは UTF-8 で読む(Windows の既定の cp932 で書くと文字化けする)
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="画面の画像を基準の画像と比べる(docs/design/16 §5)")
    commands = parser.add_subparsers(dest="command", required=True)

    compare_parser = commands.add_parser("compare")
    compare_parser.add_argument("actual")
    compare_parser.add_argument("baseline")
    compare_parser.add_argument("--diff", help="差の画像(PNG)を書く場所")
    compare_parser.add_argument("--channel-tolerance", type=int, default=DEFAULT_CHANNEL_TOLERANCE)
    compare_parser.add_argument("--max-differing-percent", type=float, default=DEFAULT_MAX_DIFFERING_PERCENT)

    update_parser = commands.add_parser("update")
    update_parser.add_argument("actual")
    update_parser.add_argument("baseline")

    commands.add_parser("selftest")

    arguments = parser.parse_args()
    handler = {"compare": command_compare, "update": command_update, "selftest": command_selftest}[arguments.command]
    return handler(arguments)


if __name__ == "__main__":
    sys.exit(main())
