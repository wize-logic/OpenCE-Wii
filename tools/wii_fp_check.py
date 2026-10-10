#!/usr/bin/env python3
"""Checks that the Wii build's own code has no fused multiply-adds (``ninja wii``).

Every port compiles with -ffp-contract=off so that each rounds a product
and a sum as the others do (port/include/halo_math.h), and the lockstep
check compares the Wii's game with x86's bit for bit. Clang has let
multiply-adds through regardless (a function it does not optimise:
source/ai/ai.c's ai_handle_spatial_effect), so the objects clang compiled
are read for fmadd, fmsub, fnmadd and fnmsub. The libraries devkitPPC's gcc
built (newlib, libogc) are not the game's and not checked.

usage: wii_fp_check.py --objdump OBJDUMP --stamp FILE @objects.rsp
"""

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

FUNCTION = re.compile(r"^[0-9a-f]+ <(.+)>:$")
FUSED = re.compile(r"^\s*[0-9a-f]+:\s+(fn?m(?:add|sub)s?\.?)\s")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--stamp", required=True)
    parser.add_argument("objects", nargs="+", help="objects, or @FILE: ninja's response file of them (quoted as a shell's)")
    args = parser.parse_args()
    objects = []
    for argument in args.objects:
        if argument.startswith("@"):
            objects += shlex.split(Path(argument[1:]).read_text())
        else:
            objects.append(argument)
    found = []
    for start in range(0, len(objects), 200):
        batch = objects[start:start + 200]
        listing = subprocess.run([args.objdump, "-d", "--no-show-raw-insn", *batch], capture_output=True, text=True,
                                 check=True).stdout
        unit = function = None
        for line in listing.splitlines():
            if line.endswith(":     file format elf32-powerpc"):
                unit = line.split(":")[0]
                continue
            match = FUNCTION.match(line)
            if match:
                function = match.group(1)
                continue
            match = FUSED.match(line)
            if match and (unit, function) not in found:
                found.append((unit, function))
    for unit, function in found:
        print(f"{unit}: {function} has fused multiply-adds (-ffp-contract=off not honoured)")
    if found:
        return 1
    Path(args.stamp).touch()
    return 0


if __name__ == "__main__":
    sys.exit(main())
