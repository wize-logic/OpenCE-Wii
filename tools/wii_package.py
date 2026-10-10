#!/usr/bin/env python3
"""Packages the Wii build for the Homebrew Channel (`ninja wii-package`).

The zip holds apps/halo/ as the Homebrew Channel reads it:
- boot.dol, the build
- meta.xml, the name, version and description the channel shows
- icon.png, 128x48, drawn here (no artwork is shipped)

It holds no game data. The maps are the player's own Xbox maps, converted by
tools/wii_convert_maps.py into apps/halo/maps on the card (port/wii/README.md).

usage: tools/wii_package.py --dol build/wii/halo.dol --output build/wii/halo-wii.zip
"""

import argparse
import subprocess
import sys
import time
import zipfile
import zlib
import struct
from pathlib import Path

ICON_WIDTH, ICON_HEIGHT = 128, 48

# 5x7 capitals and digits for the icon's lettering
FONT = {
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "C": ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    "W": ["10001", "10001", "10001", "10101", "10101", "11011", "10001"],
    "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    " ": ["00000"] * 7,
}


def icon_png() -> bytes:
    """a dark blue gradient with 'OPENCE WII' in two sizes, as a PNG"""
    pixels = [[(10, 20 + y, 40 + 2 * y) for x in range(ICON_WIDTH)] for y in range(ICON_HEIGHT)]

    def text(line: str, left: int, top: int, scale: int, colour):
        for index, letter in enumerate(line):
            for row, bits in enumerate(FONT[letter]):
                for column, bit in enumerate(bits):
                    if bit == "1":
                        for dy in range(scale):
                            for dx in range(scale):
                                x = left + (index * 6 + column) * scale + dx
                                y = top + row * scale + dy
                                if 0 <= x < ICON_WIDTH and 0 <= y < ICON_HEIGHT:
                                    pixels[y][x] = colour

    text("OPENCE", (ICON_WIDTH - (6 * 6 - 1) * 3) // 2, 4, 3, (220, 230, 255))
    text("WII", (ICON_WIDTH - (3 * 6 - 1) * 2) // 2, 30, 2, (120, 170, 255))
    raw = b"".join(b"\0" + bytes(channel for pixel in row for channel in pixel) for row in pixels)

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ICON_WIDTH, ICON_HEIGHT, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def version() -> str:
    try:
        described = subprocess.run(["git", "describe", "--always", "--dirty"], capture_output=True, text=True,
                                   check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        described = "unknown"
    return f"bring-up {described}"


def meta_xml() -> str:
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
  <name>OpenCE Wii</name>
  <coder>OpenCE</coder>
  <version>{version()}</version>
  <release_date>{time.strftime("%Y%m%d%H%M%S")}</release_date>
  <short_description>Halo: Combat Evolved (Xbox) for the Wii</short_description>
  <long_description>A port of the Halo: Combat Evolved Xbox decompilation to the Wii. It needs your own Xbox maps, converted with tools/wii_convert_maps.py into apps/halo/maps on the SD card or a USB drive. In bring-up: the main menu runs, levels are not playable yet.</long_description>
  <ahb_access/>
</app>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dol", type=Path, default=Path("build/wii/halo.dol"))
    parser.add_argument("--output", type=Path, default=Path("build/wii/halo-wii.zip"))
    args = parser.parse_args()
    if not args.dol.is_file():
        sys.exit(f"wii_package.py: no {args.dol}; build it with ninja wii")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.write(args.dol, "apps/halo/boot.dol")
        archive.writestr("apps/halo/meta.xml", meta_xml())
        archive.writestr("apps/halo/icon.png", icon_png())
    print(f"{args.output}: boot.dol, meta.xml, icon.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
