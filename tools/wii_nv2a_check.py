#!/usr/bin/env python3
"""Checks the vertex programs compiled ahead of time against the interpreter, on the host
(port/wii/tests/nv2a_native_check.c, port/wii/tools/nv2a_codegen.c).

usage: tools/wii_nv2a_check.py
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MICROCODE = ROOT / "source" / "rasterizer" / "xbox" / "rasterizer_xbox_vertex_shaders_data.inc"


def main() -> int:
    work = ROOT / "build" / "wii" / "tests"
    work.mkdir(parents=True, exist_ok=True)
    wii = ROOT / "port" / "wii"
    includes = [f"-I{wii / 'tests' / 'host'}", f"-I{wii / 'src'}"]
    compile_flags = ["cc", "-std=gnu11", "-O2", "-ffp-contract=off", "-g", "-Wall", *includes]
    subprocess.run([*compile_flags, "-DNV2A_CPU_NO_NATIVES", "-o", str(work / "nv2a_codegen"),
                    str(wii / "tools" / "nv2a_codegen.c"), "-lm"], check=True)
    subprocess.run([str(work / "nv2a_codegen"), str(MICROCODE), str(work / "nv2a_programs.c")], check=True)
    subprocess.run([*compile_flags, "-o", str(work / "nv2a_native_check"), str(wii / "tests" / "nv2a_native_check.c"),
                    str(wii / "src" / "nv2a_cpu.c"), str(work / "nv2a_programs.c"), "-lm"], check=True)
    return subprocess.run([str(work / "nv2a_native_check"), str(MICROCODE)]).returncode


if __name__ == "__main__":
    sys.exit(main())
