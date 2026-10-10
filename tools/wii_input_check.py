#!/usr/bin/env python3
"""Builds and runs the host check of the Wii's controller mapping (port/wii/tests/wii_input_check.c): synthetic
WiiLibs input states through port/wii/src/xinput_wii_map.c, compared with the Xbox gamepad state the README
("The controllers") says they give. WiiLibs' input_shape.c comes from WiiLibs' checkout: WIILIBS, else ./WiiLibs
(the CI's), else ../WiiLibs.

The check includes a stand-in for the Xbox gamepad's declarations (port/wii/tests/host_input/platform.h); this
script first compares its values with the XDK stand-ins' (port/include/xdk/xdk_xbox.h).

usage: tools/wii_input_check.py
"""

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def wiilibs_dir() -> Path:
    for candidate in (os.environ.get("WIILIBS"), ROOT / "WiiLibs", ROOT.parent / "WiiLibs"):
        if candidate and (Path(candidate) / "wl" / "platform" / "input_shape.h").is_file():
            return Path(candidate)
    sys.exit("WiiLibs (with wl/platform/input_shape.h) not found: set WIILIBS to its checkout")


def gamepad_defines(path: Path) -> dict:
    pattern = re.compile(r"^#define\s+(XINPUT_GAMEPAD_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", re.M)
    return {name: int(value, 0) for name, value in pattern.findall(path.read_text())}


def main() -> int:
    tests = ROOT / "port" / "wii" / "tests"
    standin = gamepad_defines(tests / "host_input" / "platform.h")
    xdk = gamepad_defines(ROOT / "port" / "include" / "xdk" / "xdk_xbox.h")
    if len(standin) != 16:
        sys.exit(f"the stand-in header defines {len(standin)} XINPUT_GAMEPAD names, not 16")
    for name, value in standin.items():
        if xdk.get(name) != value:
            sys.exit(f"{name}: the stand-in has {value}, the XDK header {xdk.get(name)}")

    output = ROOT / "build" / "wii" / "tests" / "wii_input_check"
    output.parent.mkdir(parents=True, exist_ok=True)
    wiilibs = wiilibs_dir()
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    f"-I{tests / 'host_input'}", f"-I{ROOT / 'port' / 'wii' / 'src'}", f"-I{wiilibs}",
                    "-o", str(output), str(tests / "wii_input_check.c"),
                    str(ROOT / "port" / "wii" / "src" / "xinput_wii_map.c"),
                    str(wiilibs / "wl" / "platform" / "input_shape.c"), "-lm"], check=True)
    return subprocess.run([str(output)]).returncode


if __name__ == "__main__":
    sys.exit(main())
