#!/usr/bin/env python3
"""Checks the Wii's sqrtf (port/wii/src/wii_math.c) against the host's, bit for bit, on the host
(port/wii/tests/wii_math_check.c).

usage: tools/wii_math_check.py [--all]   (--all: every positive float, not every 199th)
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    work = ROOT / "build" / "wii" / "tests"
    work.mkdir(parents=True, exist_ok=True)
    wii = ROOT / "port" / "wii"
    subprocess.run(["cc", "-std=gnu11", "-O2", "-ffp-contract=off", "-g", "-Wall", "-o", str(work / "wii_math_check"),
                    str(wii / "tests" / "wii_math_check.c"), "-lm"], check=True)
    return subprocess.run([str(work / "wii_math_check"), *sys.argv[1:]]).returncode


if __name__ == "__main__":
    sys.exit(main())
