#!/usr/bin/env python3
"""Builds and runs the host check of the Wii's combiner translation (port/wii/tests/gx_psh_check.c).

usage: tools/wii_gx_psh_check.py [programs]
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    output = ROOT / "build" / "wii" / "tests" / "gx_psh_check"
    output.parent.mkdir(parents=True, exist_ok=True)
    tests = ROOT / "port" / "wii" / "tests"
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-function", f"-I{tests / 'host'}",
                    f"-I{ROOT / 'port' / 'wii' / 'src'}", "-o", str(output), str(tests / "gx_psh_check.c"), str(ROOT / "port" / "wii" / "src" / "gx_psh.c"),
                    "-lm"], check=True)
    return subprocess.run([str(output), *sys.argv[1:]]).returncode


if __name__ == "__main__":
    sys.exit(main())
