#!/usr/bin/env python3
"""Builds and runs the host check of the Wii's combiner translation (port/wii/tests/gx_psh_check.c), with
WiiLibs' TEV programs (wl/platform/tev.c) from WiiLibs' checkout: WIILIBS, else ./WiiLibs (the CI's), else
../WiiLibs; then the check of the translated programs' cache (port/wii/tests/gx_psh_cache_check.c).

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


def build(name: str, sources: list) -> Path:
    output = ROOT / "build" / "wii" / "tests" / name
    output.parent.mkdir(parents=True, exist_ok=True)
    tests = ROOT / "port" / "wii" / "tests"
    wiilibs = wiilibs_dir()
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-function", f"-I{tests / 'host'}",
                    f"-I{ROOT / 'port' / 'wii' / 'src'}", f"-I{wiilibs}", "-o", str(output), *map(str, sources),
                    str(ROOT / "port" / "wii" / "src" / "gx_psh.c"), str(wiilibs / "wl" / "platform" / "tev.c"),
                    str(wiilibs / "wl" / "platform" / "host" / "tev_host.c"), str(wiilibs / "wl" / "base" / "hash.c"),
                    "-lm"], check=True)
    return output


def main() -> int:
    tests = ROOT / "port" / "wii" / "tests"
    check = build("gx_psh_check", [tests / "gx_psh_check.c"])
    # (the translated programs' cache, port/wii/src/gx_psh_cache.c, against fresh translations)
    cache_check = build("gx_psh_cache_check", [tests / "gx_psh_cache_check.c",
                                               ROOT / "port" / "wii" / "src" / "gx_psh_cache.c"])
    status = subprocess.run([str(check), *sys.argv[1:]]).returncode
    return subprocess.run([str(cache_check)]).returncode or status


if __name__ == "__main__":
    sys.exit(main())
