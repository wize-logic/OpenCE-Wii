#!/usr/bin/env python3
"""The Wii port's sampling profiler (port/wii/src/wii_profile.c, WiiLibs' wl/platform/profile.h).

    tools/wii_profile.py setup --note b30 --seconds 20        # writes build/wii/sd/apps/halo/profile.txt
    tools/wii_dolphin.py --seconds 900 --until "[profile] done" > build/wii/profile.log   # a checkpoint run
    tools/wii_profile.py report build/wii/profile.log --top 40 --tree 2   # the tables
    tools/wii_profile.py setup --remove                       # the game profiles nothing without the file

`setup` writes profile.txt, the file that arms the profiler (port/wii/README.md, "Where the
processor's time goes"), into the SD card folder Dolphin syncs in, and says which lines of
config.toml play a level without a person. `report` runs WiiLibs' tools/wl_profile.py over a
log (or a profileN.txt the game wrote) against build/wii/halo.elf; every other option is
wl_profile.py's (--top, --callers, --tree, --hot, --timeline, --thread, --collapsed).
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

KEYS = ("seconds", "after", "hz", "depth", "skip", "repeat")


def profile_text(args: argparse.Namespace) -> str:
    lines = ["# sd:/apps/halo/profile.txt: arms the sampling profiler (tools/wii_profile.py)"]
    for key in KEYS:
        value = getattr(args, key)
        if value is not None:
            lines.append(f"{key} {value}")
    lines.append(f"log {0 if args.no_log else 1}")
    if args.note:
        lines.append(f"note {args.note}")
    return "\n".join(lines) + "\n"


def setup(args: argparse.Namespace) -> int:
    folder = args.sd / "apps" / "halo"
    path = folder / "profile.txt"
    if args.remove:
        path.unlink(missing_ok=True)
        print(f"removed {path}")
        return 0
    folder.mkdir(parents=True, exist_ok=True)
    path.write_text(profile_text(args), encoding="utf-8")
    print(f"wrote {path}")
    print("play a level without a person with these lines in config.toml, [debug] (no lockstep logging: it costs):")
    print('  network_test = "solo:levels\\\\b30\\\\b30"   # a tour: "solo:levels\\\\b30\\\\b30,levels\\\\a10\\\\a10"')
    print('  network_test_tour = 40.0                  # (a tour only) seconds of game time in each level')
    print('  test_input = "bot:1"')
    print("with repeat N in profile.txt for a tour of N levels.")
    return 0


def report(args: argparse.Namespace, rest: list[str]) -> int:
    tool = args.wiilibs / "tools" / "wl_profile.py"
    if not tool.is_file():
        sys.exit(f"wii_profile.py: no {tool}; pass --wiilibs")
    if not args.elf.is_file():
        sys.exit(f"wii_profile.py: no {args.elf}; build it with ninja wii")
    return subprocess.run([sys.executable, str(tool), str(args.elf), *map(str, args.dump), *rest]).returncode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    s = sub.add_parser("setup", help="write (or --remove) profile.txt on the SD card folder")
    s.add_argument("--sd", type=Path, default=Path("build/wii/sd"))
    s.add_argument("--remove", action="store_true")
    for key in KEYS:
        s.add_argument(f"--{key}", type=int)
    s.add_argument("--note")
    s.add_argument("--no-log", action="store_true", help="only the profileN.txt files, not the log")
    r = sub.add_parser("report", help="symbolize a log or profileN.txt with WiiLibs' wl_profile.py")
    r.add_argument("dump", nargs="+", type=Path)
    r.add_argument("--elf", type=Path, default=Path("build/wii/halo.elf"))
    r.add_argument("--wiilibs", type=Path, default=Path(os.environ.get("WIILIBS", "../WiiLibs")))
    args, rest = parser.parse_known_args(argv)
    if args.command == "setup":
        if rest:
            parser.error(f"unknown options {rest}")
        return setup(args)
    return report(args, rest)


if __name__ == "__main__":
    sys.exit(main())
