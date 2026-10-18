#!/usr/bin/env python3
"""Generate src/app.ico — a small landscape glyph for Explorer / "Open with".

No imaging library is used: the icon is rasterised here with 4x supersampling and
written as a multi-size ICO (BMP entries up to 64 px, PNG entries above that).
"""

import struct
import zlib
from pathlib import Path

SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 4  # supersampling factor
BG = (0x14, 0x16, 0x1A)
SKY_TOP = (0x4F, 0x8E, 0xF7)
SKY_BOTTOM = (0x1B, 0x4F, 0xA8)
SUN = (0xFF, 0xF2, 0xC4)
MOUNTAIN_BACK = (0xBF, 0xDB, 0xFF)
MOUNTAIN_FRONT = (0xFF, 0xFF, 0xFF)
GROUND = (0x13, 0x36, 0x6E)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def inside_round_rect(u, v, x0, y0, x1, y1, r):
    if u < x0 or u > x1 or v < y0 or v > y1:
        return False
    cx = min(max(u, x0 + r), x1 - r)
    cy = min(max(v, y0 + r), y1 - r)
    if (u - cx) ** 2 + (v - cy) ** 2 > r * r:
        return False
    return True


def triangle(px, py, a, b, c):
    def sign(p1, p2, p3):
        return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])

    d1 = sign((px, py), a, b)
    d2 = sign((px, py), b, c)
    d3 = sign((px, py), c, a)
    has_neg = d1 < 0 or d2 < 0 or d3 < 0
    has_pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (has_neg and has_pos)


def shade(u, v):
    """Returns RGBA in 0..1 for the sample at (u, v)."""
    if inside_round_rect(u, v, 0.0, 0.0, 1.0, 1.0, 0.20):
        if inside_round_rect(u, v, 0.13, 0.13, 0.87, 0.87, 0.07):
            r, g, b = mix(SKY_TOP, SKY_BOTTOM, min(1.0, max(0.0, (v - 0.13) / 0.74)))
            if (u - 0.66) ** 2 + (v - 0.31) ** 2 <= 0.075 ** 2:
                r, g, b = SUN
            if triangle(u, v, (0.05, 0.87), (0.42, 0.40), (0.74, 0.87)):
                r, g, b = MOUNTAIN_BACK
            if triangle(u, v, (0.30, 0.87), (0.62, 0.52), (0.95, 0.87)):
                r, g, b = MOUNTAIN_FRONT
            if v >= 0.80:
                r, g, b = mix((r, g, b), GROUND, 0.85)
            return (r / 255.0, g / 255.0, b / 255.0, 1.0)
        return (BG[0] / 255.0, BG[1] / 255.0, BG[2] / 255.0, 1.0)
    return (0.0, 0.0, 0.0, 0.0)


def render(size):
    out = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    u = (x + (sx + 0.5) / SS) / size
                    v = (y + (sy + 0.5) / SS) / size
                    r, g, b, a = shade(u, v)
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
            n = SS * SS
            alpha = acc[3] / n
            if alpha > 0:
                rgb = [acc[i] / acc[3] * 255.0 for i in range(3)]
            else:
                rgb = [0.0, 0.0, 0.0]
            offset = (y * size + x) * 4
            out[offset + 0] = max(0, min(255, int(round(rgb[0]))))
            out[offset + 1] = max(0, min(255, int(round(rgb[1]))))
            out[offset + 2] = max(0, min(255, int(round(rgb[2]))))
            out[offset + 3] = max(0, min(255, int(round(alpha * 255))))
    return bytes(out)


def bmp_entry(rgba, size):
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = []
    for y in range(size - 1, -1, -1):
        row = bytearray()
        for x in range(size):
            i = (y * size + x) * 4
            r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    xor = b"".join(rows)
    mask_stride = ((size + 31) // 32) * 4
    and_mask = b"\x00" * (mask_stride * size)
    return header + xor + and_mask


def png_entry(rgba, size):
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        raw += rgba[y * size * 4 : (y + 1) * size * 4]

    def chunk(tag, data):
        payload = tag + data
        return struct.pack(">I", len(data)) + payload + struct.pack(">I", zlib.crc32(payload) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def main():
    images = []
    for size in SIZES:
        rgba = render(size)
        data = png_entry(rgba, size) if size >= 128 else bmp_entry(rgba, size)
        images.append((size, data))

    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack("<BBBBHHII", size if size < 256 else 0, size if size < 256 else 0,
                           0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data

    target = Path(__file__).resolve().parent.parent / "src" / "app.ico"
    target.write_bytes(bytes(out))
    print(f"wrote {target} ({len(out)} bytes, sizes: {[s for s, _ in images]})")


if __name__ == "__main__":
    main()
