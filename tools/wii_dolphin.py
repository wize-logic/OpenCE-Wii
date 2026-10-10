#!/usr/bin/env python3
"""Runs the Wii build in Dolphin, through WiiLibs' runner (tools/dolphin_run.py in WiiLibs).

The SD card is a folder synced in by Dolphin, build/wii/sd unless --sd says otherwise; the game
looks for its data in apps/halo there (port/wii/README.md). The screenshot goes to
build/wii/dolphin.png. Every other option is the runner's (--until, --fail, --seconds,
--dump-frames, --dolphin).

usage: tools/wii_dolphin.py [--wiilibs DIR] [runner options] [build/wii/halo.dol]
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], add_help=False)
    parser.add_argument("--wiilibs", type=Path, default=Path(os.environ.get("WIILIBS", "../WiiLibs")))
    parser.add_argument("--sd", type=Path, default=Path("build/wii/sd"))
    parser.add_argument("--screenshot", type=Path, default=Path("build/wii/dolphin.png"))
    # (Dolphin's Direct3D 11 backend exits without a word on the game's longer TEV programs)
    parser.add_argument("--video", default="Vulkan")
    # (dual core: Dolphin exits without a word in the menu once the vertices come fast; the cause
    # is open, port/wii/README.md)
    parser.add_argument("--dual-core", action="store_true")
    args, rest = parser.parse_known_args()
    runner = args.wiilibs / "tools" / "dolphin_run.py"
    if not runner.is_file():
        sys.exit(f"wii_dolphin.py: no {runner}; pass --wiilibs")
    # (the program is the one argument ending in .dol or .elf that is no option's value)
    values = {rest[index + 1] for index, argument in enumerate(rest[:-1]) if argument.startswith("--")
              and "=" not in argument}
    if not any(argument.endswith((".dol", ".elf")) and argument not in values for argument in rest):
        rest.append("build/wii/halo.dol")
    (args.sd / "apps" / "halo").mkdir(parents=True, exist_ok=True)
    return subprocess.run([sys.executable, str(runner), "--name", "halo-wii-dolphin", "--sd", str(args.sd),
                           "--screenshot", str(args.screenshot), "--video", args.video,
                           *([] if args.dual_core else ["--single-core"]), *rest]).returncode


if __name__ == "__main__":
    sys.exit(main())
