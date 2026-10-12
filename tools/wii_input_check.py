#!/usr/bin/env python3
"""Builds and runs the host checks of the Wii's controllers: the mapping (port/wii/tests/wii_input_check.c):
synthetic WiiLibs input states through port/wii/src/xinput_wii_map.c, compared with the Xbox gamepad state the README
("The controllers") says they give, with the Nunchuk's thrust as melee (synthetic acceleration traces through WiiLibs'
input_gesture.c); Halo's configuration of the HOME menu (port/wii/tests/wii_home_check.c, port/wii/src/wii_home_config.c)
run through WiiLibs' state machine (wl/platform/home_menu.c) against fake controllers, a fake game and a clock; the
same for the wrist strap reminder (port/wii/tests/wii_strap_check.c, WiiLibs' strap_screen.c); and the script verbs that drive those
on a run without a person (port/wii/tests/wii_script_check.c, with port/linux/src/test_input.c: home, remote,
extension, battery, event; and the other controllers' player, N:, connect, disconnect and bot N, four controllers
read in order a frame at a time). WiiLibs' input_shape.c, home_menu.c, strap_screen.c and overlay.c come from
WiiLibs' checkout: WIILIBS, else ./WiiLibs (the CI's), else ../WiiLibs.

The check includes a stand-in for the Xbox gamepad's declarations (port/wii/tests/host_input/platform.h); this
script first compares its values with the XDK stand-ins' (port/include/xdk/xdk_xbox.h).

usage: tools/wii_input_check.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def wiilibs_dir() -> Path:
    for candidate in (os.environ.get("WIILIBS"), ROOT / "WiiLibs", ROOT.parent / "WiiLibs"):
        if candidate and (Path(candidate) / "wl" / "platform" / "home_menu.h").is_file():
            return Path(candidate)
    sys.exit("WiiLibs (with wl/platform/home_menu.h) not found: set WIILIBS to its checkout")


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

    wiilibs = wiilibs_dir()
    flags = ["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
             f"-I{tests / 'host_input'}", f"-I{ROOT / 'port' / 'wii' / 'src'}", f"-I{wiilibs}"]
    src = ROOT / "port" / "wii" / "src"
    platform = wiilibs / "wl" / "platform"
    builds = [
        ("wii_input_check", [tests / "wii_input_check.c", src / "xinput_wii_map.c",
                             wiilibs / "wl" / "platform" / "input_shape.c",
                             wiilibs / "wl" / "platform" / "input_gesture.c"]),
        ("wii_home_check", [tests / "wii_home_check.c", src / "wii_home_config.c", platform / "home_menu.c", platform / "strap_screen.c",
                            platform / "overlay.c"]),
        ("wii_strap_check", [tests / "wii_strap_check.c", src / "wii_home_config.c", platform / "strap_screen.c",
                             platform / "home_menu.c", platform / "overlay.c"]),
    ]
    status = 0
    for name, sources in builds:
        output = ROOT / "build" / "wii" / "tests" / name
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([*flags, "-o", str(output), *map(str, sources), "-lm"], check=True)
        status |= subprocess.run([str(output)]).returncode
    status |= script_check(flags, tests)
    return status


def script_check(flags: list, tests: Path) -> int:
    """test_input.c's Wii verbs: a script played a read at a time, and lines that must be refused. test_input.c is
    copied beside the stand-in header first, so that its include of platform.h finds the stand-in's"""
    build = ROOT / "build" / "wii" / "tests" / "script_check"
    build.mkdir(parents=True, exist_ok=True)
    for name in ("test_input.c", "test_input.h"):
        shutil.copyfile(ROOT / "port" / "linux" / "src" / name, build / name)
    output = build / "wii_script_check"
    subprocess.run([*flags, "-DHALO_WII", f"-I{build}", "-Wno-maybe-uninitialized", "-Wno-format-truncation", f"-I{ROOT / 'port' / 'linux' / 'src'}", "-o", str(output),
                    str(tests / "wii_script_check.c"), str(build / "test_input.c"), "-lm"], check=True)
    status = 0
    with tempfile.TemporaryDirectory() as folder:
        script = os.path.join(folder, "script.txt")
        runs = [["good"], ["players"]] + [["bad", line] for line in (
            "remote 4 off", "remote 0 maybe", "remote off", "extension 0 plaid", "extension 9 none", "battery 0 101",
            "battery 0 -1", "battery 0 many", "event restart", "event", "home now",
            "connect 1", "connect 5", "connect", "connect 2 maybe", "disconnect 1", "disconnect 2 real", "disconnect",
            "player 0", "player 5", "player", "player 1 2", "5: press a", "2:", "bot 5 1", "bot 0 1", "bot 2 many",
            "bot 2 -1", "bot 2 3 4")]
        for run in runs:
            status |= subprocess.run([str(output), script, *run]).returncode
    return status


if __name__ == "__main__":
    sys.exit(main())
