#!/usr/bin/env python3
"""Builds and runs the host check of the Wii's combiner translation (port/wii/tests/gx_psh_check.c), with
WiiLibs' TEV programs (wl/platform/tev.c) from WiiLibs' checkout: WIILIBS, else ./WiiLibs (the CI's), else
../WiiLibs.

usage: tools/wii_gx_psh_check.py [programs]
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def wiilibs_dir() -> Path:
    for candidate in (os.environ.get("WIILIBS"), ROOT / "WiiLibs", ROOT.parent / "WiiLibs"):
        if candidate and (Path(candidate) / "wl" / "platform" / "tev.h").is_file():
            return Path(candidate)
    sys.exit("WiiLibs not found: set WIILIBS to its checkout")


def main() -> int:
    output = ROOT / "build" / "wii" / "tests" / "gx_psh_check"
    output.parent.mkdir(parents=True, exist_ok=True)
    tests = ROOT / "port" / "wii" / "tests"
    wiilibs = wiilibs_dir()
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-function", f"-I{tests / 'host'}",
                    f"-I{ROOT / 'port' / 'wii' / 'src'}", f"-I{wiilibs}", "-o", str(output), str(tests / "gx_psh_check.c"),
                    str(ROOT / "port" / "wii" / "src" / "gx_psh.c"), str(wiilibs / "wl" / "platform" / "tev.c"),
                    str(wiilibs / "wl" / "platform" / "host" / "tev_host.c"), str(wiilibs / "wl" / "base" / "hash.c"),
                    "-lm"], check=True)
    return subprocess.run([str(output), *sys.argv[1:]]).returncode


if __name__ == "__main__":
    sys.exit(main())
