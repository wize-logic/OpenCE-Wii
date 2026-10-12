#!/usr/bin/env python3
"""Checks that the Wii build's own code has no fused multiply-adds (``ninja wii``).

Every port compiles with -ffp-contract=off so that each rounds a product
and a sum as the others do (port/include/halo_math.h), and the lockstep
check compares the Wii's game with x86's bit for bit. Clang has let
multiply-adds through regardless (a function it does not optimise:
source/ai/ai.c's ai_handle_spatial_effect), so the objects clang compiled
are read for fused multiply-adds (fmadd, fmsub, fnmadd, fnmsub, the single
and paired-single forms). The libraries devkitPPC's gcc built (newlib,
libogc) are not the game's and not checked.

The check is WiiLibs' tools/fma_check.py (../WiiLibs, or --wiilibs); this
file runs it and says what a failure means here.

usage: wii_fp_check.py --objdump OBJDUMP --stamp FILE @objects.rsp [--wiilibs ../WiiLibs]
"""

import argparse
import importlib.util
import sys
from pathlib import Path


def load_checker(wiilibs: Path):
    path = wiilibs / "tools" / "fma_check.py"
    if not path.is_file():
        sys.exit(f"wii_fp_check.py: no {path}; check out WiiLibs beside this repository or give --wiilibs")
    # (nothing is written into WiiLibs' checkout)
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("wiilibs_fma_check", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # (its dataclasses look their module up here)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], add_help=False)
    parser.add_argument("--wiilibs", type=Path, default=Path("../WiiLibs"), help="WiiLibs' checkout")
    args, rest = parser.parse_known_args()
    status = load_checker(args.wiilibs).main(["--quiet", *rest])
    if status == 1:
        print("wii_fp_check.py: -ffp-contract=off was not honoured in the functions above", file=sys.stderr)
    return status


if __name__ == "__main__":
    sys.exit(main())
