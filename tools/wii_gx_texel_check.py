#!/usr/bin/env python3
"""Builds and runs the host check of the processor's reading of the Wii's GX textures
(port/wii/tests/gx_texel_check.c: port/wii/src/wii_bitmap_texel.c against WiiLibs' conversions of
the Xbox's textures, wl/codec/xbox_tex.c), with WiiLibs from WIILIBS, else ./WiiLibs (the CI's),
else ../WiiLibs.

usage: tools/wii_gx_texel_check.py
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def wiilibs_dir() -> Path:
    for candidate in (os.environ.get("WIILIBS"), ROOT / "WiiLibs", ROOT.parent / "WiiLibs"):
        if candidate and (Path(candidate) / "wl" / "codec" / "xbox_tex.h").is_file():
            return Path(candidate)
    sys.exit("WiiLibs not found: set WIILIBS to its checkout")


def main() -> int:
    wiilibs = wiilibs_dir()
    output = ROOT / "build" / "wii" / "tests" / "gx_texel_check"
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", f"-I{ROOT / 'port' / 'wii' / 'src'}", f"-I{wiilibs}",
                    "-o", str(output), str(ROOT / "port" / "wii" / "tests" / "gx_texel_check.c"),
                    str(ROOT / "port" / "wii" / "src" / "wii_bitmap_texel.c"),
                    str(wiilibs / "wl" / "codec" / "gx_tex.c"), str(wiilibs / "wl" / "codec" / "xbox_tex.c")],
                   check=True)
    return subprocess.run([str(output)]).returncode


if __name__ == "__main__":
    sys.exit(main())
