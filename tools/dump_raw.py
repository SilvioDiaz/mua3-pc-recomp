"""Prints texels of a frame dump's .raw image (TOTK_DUMP_RAW, runtime/vk_dump.c).

    python tools/dump_raw.py <dump dir>/<name>.raw X Y [W H] [--slice Z]

The size and format come from the image's line in images.txt next to the file ("raw <file> <w>x<h>x<d> <format>").
Prints one line per texel: its position and its channel values as the shader reads them (UNORM and SNORM normalised,
floats and integers as stored, depth as 0..1 or float).
"""
import argparse
import math
import struct
import sys
from pathlib import Path


def half(v):
    return struct.unpack('<e', struct.pack('<H', v))[0]


def ufloat(v, mbits):   # 5-bit exponent, mbits mantissa, no sign (R11G11B10)
    e, m = v >> mbits, v & ((1 << mbits) - 1)
    if e == 0:
        return m * 2.0 ** (-14 - mbits)
    if e == 31:
        return math.nan if m else math.inf
    return (m | 1 << mbits) * 2.0 ** (e - 15 - mbits)


# format name (vk_dump.c fmts) -> (bytes per texel, decoder of those bytes)
def channels(code, n, signed=False, norm=0):
    size = {'B': 1, 'H': 2, 'I': 4, 'f': 4, 'e': 2}[code]
    fmt = '<' + (code.lower() if signed and code in 'BHI' else code) * n

    def dec(b):
        v = struct.unpack(fmt, b)
        if norm:
            v = [max(x / norm, -1.0) for x in v] if signed else [x / norm for x in v]
        return list(v)
    return size * n, dec


FORMATS = {}
for name, nc in (('r', 1), ('rg', 2), ('rgba', 4)):
    FORMATS[name + '8'] = channels('B', nc, norm=255)
    FORMATS[name + '8sn'] = channels('B', nc, True, 127)
    FORMATS[name + '8ui'] = channels('B', nc)
    FORMATS[name + '8i'] = channels('B', nc, True)
    FORMATS[name + '16'] = channels('H', nc, norm=65535)
    FORMATS[name + '16sn'] = channels('H', nc, True, 32767)
    FORMATS[name + '16ui'] = channels('H', nc)
    FORMATS[name + '16i'] = channels('H', nc, True)
    FORMATS[name + '16f'] = channels('e', nc)
    FORMATS[name + '32ui'] = channels('I', nc)
    FORMATS[name + '32i'] = channels('I', nc, True)
    FORMATS[name + '32f'] = channels('f', nc)
FORMATS['s8'] = channels('B', 1)
FORMATS['d16'] = FORMATS['d16s8'] = channels('H', 1, norm=65535)
FORMATS['d32f'] = FORMATS['d32fs8'] = channels('f', 1)


def d24(b):
    return [(struct.unpack('<I', b)[0] & 0xffffff) / 16777215]


FORMATS['d24'] = FORMATS['d24s8'] = (4, d24)


def rgb10a2(b, bgr=False):
    u = struct.unpack('<I', b)[0]
    v = [(u & 1023) / 1023, (u >> 10 & 1023) / 1023, (u >> 20 & 1023) / 1023, (u >> 30) / 3]
    return [v[2], v[1], v[0], v[3]] if bgr else v


FORMATS['rgb10a2'] = (4, rgb10a2)
FORMATS['bgr10a2'] = (4, lambda b: rgb10a2(b, True))


def r11g11b10(b):
    u = struct.unpack('<I', b)[0]
    return [ufloat(u & 2047, 6), ufloat(u >> 11 & 2047, 6), ufloat(u >> 22 & 1023, 5)]


FORMATS['r11g11b10f'] = (4, r11g11b10)


def rgb9e5(b):
    u = struct.unpack('<I', b)[0]
    s = 2.0 ** ((u >> 27) - 24)
    return [(u & 511) * s, (u >> 9 & 511) * s, (u >> 18 & 511) * s]


FORMATS['rgb9e5'] = (4, rgb9e5)


def find_entry(raw):
    images = raw.parent / 'images.txt'
    if not images.exists():
        sys.exit(f'{images} is missing: the size and format of {raw.name} are listed there')
    for line in images.read_text(errors='replace').splitlines():
        words = line.split()
        if 'raw' in words:
            i = words.index('raw')
            if i + 3 < len(words) and words[i + 1] == raw.name:
                w, h, d = (int(x) for x in words[i + 2].split('x'))
                return w, h, d, words[i + 3]
    sys.exit(f'{raw.name} has no "raw" entry in {images}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('raw', type=Path)
    ap.add_argument('x', type=int)
    ap.add_argument('y', type=int)
    ap.add_argument('w', type=int, nargs='?', default=1)
    ap.add_argument('h', type=int, nargs='?', default=1)
    ap.add_argument('--slice', type=int, default=0, help='slice of a 3D image')
    a = ap.parse_args()
    w, h, d, fmt = find_entry(a.raw)
    if fmt not in FORMATS:
        sys.exit(f'format {fmt} is not decoded here')
    size, dec = FORMATS[fmt]
    data = a.raw.read_bytes()
    if len(data) < w * h * d * size:
        sys.exit(f'{a.raw.name}: {len(data)} bytes, {w}x{h}x{d} {fmt} needs {w * h * d * size}')
    print(f'{a.raw.name}: {w}x{h}x{d} {fmt}')
    for y in range(a.y, min(a.y + a.h, h)):
        for x in range(a.x, min(a.x + a.w, w)):
            o = ((a.slice * h + y) * w + x) * size
            v = dec(data[o:o + size])
            print(f'{x},{y}: ' + ' '.join(f'{c:.6g}' for c in v))


if __name__ == '__main__':
    main()
