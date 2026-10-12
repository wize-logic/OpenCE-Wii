#!/usr/bin/env python3
"""Checks the vertex programs compiled ahead of time against the interpreter, on the host
(port/wii/tests/nv2a_native_check.c, port/wii/tools/nv2a_codegen.c), then the models' lighting
on GX's lights against the programs on the interpreter (port/wii/tests/gx_lighting_check.c,
port/wii/src/gx_lighting.c: display.gx_lighting).

The programs use WiiLibs' sqrtf (wl/gfx/gfx_sqrt.h, built here): WiiLibs is found in WIILIBS, else ./WiiLibs, else
../WiiLibs.

usage: tools/wii_nv2a_check.py
"""

import subprocess
import sys
from pathlib import Path

from wii_gx_psh_check import wiilibs_dir

ROOT = Path(__file__).resolve().parent.parent
MICROCODE = ROOT / "source" / "rasterizer" / "xbox" / "rasterizer_xbox_vertex_shaders_data.inc"


def main() -> int:
    work = ROOT / "build" / "wii" / "tests"
    work.mkdir(parents=True, exist_ok=True)
    wii = ROOT / "port" / "wii"
    wiilibs = wiilibs_dir()
    includes = [f"-I{wii / 'tests' / 'host'}", f"-I{wii / 'src'}", f"-I{wiilibs}"]
    sqrt = str(wiilibs / "wl" / "gfx" / "gfx_sqrt.c")
    compile_flags = ["cc", "-std=gnu11", "-O2", "-ffp-contract=off", "-g", "-Wall", *includes]
    subprocess.run([*compile_flags, "-DNV2A_CPU_NO_NATIVES", "-o", str(work / "nv2a_codegen"),
                    str(wii / "tools" / "nv2a_codegen.c"), sqrt, "-lm"], check=True)
    subprocess.run([str(work / "nv2a_codegen"), str(MICROCODE), str(work / "nv2a_programs.c")], check=True)
    subprocess.run([*compile_flags, "-o", str(work / "nv2a_native_check"), str(wii / "tests" / "nv2a_native_check.c"),
                    str(wii / "src" / "nv2a_cpu.c"), str(work / "nv2a_programs.c"), sqrt, "-lm"], check=True)
    native = subprocess.run([str(work / "nv2a_native_check"), str(MICROCODE)]).returncode
    subprocess.run([*compile_flags, "-DNV2A_CPU_NO_NATIVES", "-o", str(work / "gx_lighting_check"),
                    str(wii / "tests" / "gx_lighting_check.c"), str(wii / "src" / "gx_lighting.c"),
                    str(wii / "src" / "nv2a_cpu.c"), sqrt, "-lm"], check=True)
    lighting = subprocess.run([str(work / "gx_lighting_check"), str(MICROCODE)]).returncode
    return native or lighting


if __name__ == "__main__":
    sys.exit(main())
