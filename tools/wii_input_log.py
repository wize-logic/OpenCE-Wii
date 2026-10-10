#!/usr/bin/env python3
"""Reads the Wii build's input log (debug.input_log = true, HALO_INPUT_LOG=1; lines
"input N: buttons XXXX analog .. .. left X Y right X Y [(menu)]" from xinput_wii.c) and writes each state in
words, so that a run in which every button of a controller was pressed in turn can be checked against the table
in port/wii/README.md ("The controllers"). With --expect, the Xbox controls each press should give are read from
that table's column and compared with the log: the controller is named (classic, remote, gamecube) and the
buttons are the ones the table lists, in its order.

usage: tools/wii_input_log.py LOG [--expect classic|remote|gamecube]
       (LOG is the game's log, or the runner's output; - is the standard input)
"""

import argparse
import re
import sys

LINE = re.compile(r"input (\d+): buttons ([0-9a-f]{4}) analog((?: [0-9a-f]{2}){8}) left (-?\d+) (-?\d+) "
                  r"right (-?\d+) (-?\d+)( \(menu\))?")
BUTTONS = ["up", "down", "left", "right", "Start", "Back", "Lclick", "Rclick"]
ANALOG = ["A", "B", "X", "Y", "Black", "White", "LT", "RT"]

# one physical button at a time in play: the Xbox controls it gives (xinput_wii_map.c; port/wii/README.md)
EXPECT = {
    "classic": {"b": "A", "a": "B", "y": "X", "x": "Y", "ZL": "Black", "ZR": "White", "L": "LT", "R": "RT",
                "up": "up Rclick", "down": "down Lclick", "left": "left", "right": "right", "+": "Start",
                "-": "Back"},
    "remote": {"A": "A", "1": "B", "2": "Rclick", "Z": "LT", "C": "Lclick", "B": "RT", "up": "up Y",
               "down": "down X", "left": "left Black", "right": "right White", "+": "Start", "-": "Back"},
    "gamecube": {"A": "A", "B": "B", "X": "X", "Y": "Y", "Z": "Black", "L": "LT", "R": "RT",
                 "up": "up Rclick", "down": "down Lclick", "left": "left Back", "right": "right White",
                 "Start": "Start"},
}


def decode(buttons: int, analog: list) -> str:
    names = [ANALOG[i] for i in range(8) if analog[i]]
    names += [BUTTONS[i] for i in range(8) if buttons & (1 << i)]
    return " ".join(sorted(names, key=lambda n: (ANALOG + BUTTONS).index(n))) or "-"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--expect", choices=sorted(EXPECT))
    args = parser.parse_args()
    text = sys.stdin if args.log == "-" else open(args.log, errors="replace")
    states = []
    for line in text:
        match = LINE.search(line)
        if not match:
            continue
        port, buttons, analog, lx, ly, rx, ry, menu = match.groups()
        analog = [int(v, 16) for v in analog.split()]
        words = decode(int(buttons, 16), analog)
        extra = []
        if any(v for v in (int(lx), int(ly), int(rx), int(ry))):
            extra.append(f"left {lx} {ly} right {rx} {ry}")
        triggers = [analog[6], analog[7]]
        if any(0 < t < 255 for t in triggers):
            extra.append(f"triggers {triggers[0]} {triggers[1]}")
        states.append((int(port), words, menu is not None))
        print(f"port {port}{' menu' if menu else ''}: {words}{'  ' + '; '.join(extra) if extra else ''}")
    if args.expect:
        seen = {words for _, words, in_menu in states if not in_menu}
        missing = {name: want for name, want in EXPECT[args.expect].items()
                   if not any(sorted(want.split()) == sorted(s.split()) for s in seen)}
        for name, want in missing.items():
            print(f"MISSING {args.expect} {name}: no state of exactly '{want}' in play")
        print(f"{len(EXPECT[args.expect]) - len(missing)} of {len(EXPECT[args.expect])} {args.expect} buttons seen")
        return 1 if missing else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
