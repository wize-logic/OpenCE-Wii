#!/usr/bin/env python3
"""Links the Wii build (`ninja wii`) while its platform layer is incomplete.

The game calls far more of the platform layer (the Xbox APIs the port
emulates) than port/wii/src provides yet. This links the executable once; if
the linker reports undefined symbols, it writes a C file defining each one
as a function that stops the game naming it (WiiLibs'
wl_platform_missing_symbol, wl/platform/stop.h), compiles it, and links
again. The list goes to --report, one name a line: what the Wii port still
lacks. A data symbol stubbed this way is a function, which is wrong, and
only safe because the game stops as soon as it calls into any of them. Once
the list is empty this is an ordinary link.

The linking is done by WiiLibs' tools/link_stubs.py (../WiiLibs, or
--wiilibs); this file only finds it.

usage: wii_link.py [--wiilibs ../WiiLibs] --linker CC --compiler CC --cflags FLAGS
                   --ldflags FLAGS --libs FLAGS --stubs FILE.c --report FILE.txt
                   --output FILE.elf @objects.rsp
"""

import argparse
import importlib.util
import sys
from pathlib import Path


def load_linker(wiilibs: Path):
    path = wiilibs / "tools" / "link_stubs.py"
    if not path.is_file():
        sys.exit(f"wii_link.py: no {path}; check out WiiLibs beside this repository or give --wiilibs")
    # (nothing is written into WiiLibs' checkout)
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("wiilibs_link_stubs", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], add_help=False)
    parser.add_argument("--wiilibs", type=Path, default=Path("../WiiLibs"), help="WiiLibs' checkout")
    args, rest = parser.parse_known_args()
    return load_linker(args.wiilibs).main(["--name", "wii_link.py", *rest])


if __name__ == "__main__":
    sys.exit(main())
