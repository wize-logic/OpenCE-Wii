#!/usr/bin/env python3
"""Packages the Wii build for the Homebrew Channel (`ninja wii-package`).

The zip holds apps/halo/ as the Homebrew Channel reads it:
- boot.dol, the build
- meta.xml, the name, version and descriptions the channel shows
- icon.png, 128x48, drawn by the packager (no artwork is shipped)

It holds no game data. The maps are the player's own Xbox maps, converted by
tools/wii_convert_maps.py into apps/halo/maps on the card (port/wii/README.md).

The zip is made by WiiLibs' tools/wii_package.py (../WiiLibs, or --wiilibs);
this file gives it the port's name, descriptions (port/wii/package_description.txt)
and the version, which is the repository's `git describe`.

usage: tools/wii_package.py --dol build/wii/halo.dol --output build/wii/halo-wii.zip [--wiilibs ../WiiLibs]
"""

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

DESCRIPTION = Path(__file__).resolve().parent.parent / "port" / "wii" / "package_description.txt"


def version() -> str:
    try:
        described = subprocess.run(["git", "describe", "--always", "--dirty"], capture_output=True, text=True,
                                   check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        described = "unknown"
    return f"bring-up {described}"


def load_packager(wiilibs: Path):
    path = wiilibs / "tools" / "wii_package.py"
    if not path.is_file():
        sys.exit(f"wii_package.py: no {path}; check out WiiLibs beside this repository or give --wiilibs")
    # (nothing is written into WiiLibs' checkout)
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("wiilibs_wii_package", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dol", type=Path, default=Path("build/wii/halo.dol"))
    parser.add_argument("--output", type=Path, default=Path("build/wii/halo-wii.zip"))
    parser.add_argument("--wiilibs", type=Path, default=Path("../WiiLibs"), help="WiiLibs' checkout")
    args = parser.parse_args()
    if not args.dol.is_file():
        sys.exit(f"wii_package.py: no {args.dol}; build it with ninja wii")
    return load_packager(args.wiilibs).main([
        "--dol", str(args.dol),
        "--output", str(args.output),
        "--name", "OpenCE Wii",
        "--app-dir", "halo",
        "--coder", "OpenCE",
        "--version", version(),
        "--short-description", "Halo: Combat Evolved (Xbox) for the Wii",
        "--long-description-file", str(DESCRIPTION),
        "--icon-text", "OPENCE", "WII",
        "--ahb-access",
    ])


if __name__ == "__main__":
    sys.exit(main())
