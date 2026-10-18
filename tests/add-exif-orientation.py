#!/usr/bin/env python3
"""Insert a minimal EXIF APP1 block carrying only the orientation tag.

Pure byte manipulation: no imaging library is involved. The base JPEG comes from
tests/make-test-image.ps1; this script produces orientation1.jpg ... orientation8.jpg
so the viewer's EXIF handling can be verified against known quadrant colors.
"""
import struct
import sys
from pathlib import Path

ORIENTATION_TAG = 0x0112


def app1_for_orientation(orientation: int) -> bytes:
    tiff = b"II"                      # little endian
    tiff += struct.pack("<H", 42)     # magic
    tiff += struct.pack("<I", 8)      # offset of IFD0
    tiff += struct.pack("<H", 1)      # one directory entry
    tiff += struct.pack("<H", ORIENTATION_TAG)
    tiff += struct.pack("<H", 3)      # type SHORT
    tiff += struct.pack("<I", 1)      # count
    tiff += struct.pack("<H", orientation) + b"\x00\x00"  # value, padded
    tiff += struct.pack("<I", 0)      # no next IFD

    payload = b"Exif\x00\x00" + tiff
    return b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload


def main() -> int:
    source = Path(sys.argv[1] if len(sys.argv) > 1 else "tests/data/quad.jpg")
    data = source.read_bytes()
    if not data.startswith(b"\xff\xd8"):
        print(f"{source} is not a JPEG", file=sys.stderr)
        return 1

    for orientation in range(1, 9):
        out = source.with_name(f"orientation{orientation}.jpg")
        out.write_bytes(data[:2] + app1_for_orientation(orientation) + data[2:])
        print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
