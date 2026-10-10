#!/usr/bin/env python3
"""Compares two lockstep logs: the same session played by two builds (port/wii/README.md, "The
lockstep check").

Each build logs, with debug.fixed_ticks and debug.lockstep_log = N, a line every N ticks:

    lockstep: tick T objects C hash H random R

the hash of every object's position, velocity, facing, vitality and parent, and the game's random
seed (port/linux/game/network_test.c). This finds the first tick at which the two logs differ. With
debug.lockstep_detail = T, each build also lists every object's values at tick T: given that tick,
the objects that differ are printed, their values as floats.

usage: tools/lockstep_compare.py a.log b.log
"""

import re
import struct
import sys
from typing import Dict, List, Tuple

DIGEST = re.compile(r"lockstep: tick (\d+) objects (\d+) hash ([0-9a-f]+) random ([0-9a-f]+)")
OBJECT = re.compile(r"lockstep object (\d+): ((?:[0-9a-f]{8} ?){14})")
NODES = re.compile(r"lockstep nodes (\d+): (\d+) ([0-9a-f]+)")
FIELDS = ["index", "definition", "position x", "position y", "position z", "velocity x", "velocity y",
          "velocity z", "forward x", "forward y", "forward z", "body", "shield", "parent"]


def digests(path: str) -> Dict[int, Tuple[str, ...]]:
    """each tick's digest, the first time the tick is logged (a revert to a saved game logs its
    ticks again)"""
    found: Dict[int, Tuple[str, ...]] = {}
    for line in open(path, errors="replace"):
        match = DIGEST.search(line)
        if match and int(match.group(1)) not in found:
            found[int(match.group(1))] = match.groups()[1:]
    return found


def objects(path: str) -> Tuple[Dict[int, List[str]], Dict[int, Tuple[str, str]]]:
    """the first detail's objects and their nodes' hashes"""
    values: Dict[int, List[str]] = {}
    nodes: Dict[int, Tuple[str, str]] = {}
    for line in open(path, errors="replace"):
        match = OBJECT.search(line)
        if match:
            index = int(match.group(1))
            if index in values:
                break
            values[index] = match.group(2).split()
        match = NODES.search(line)
        if match and int(match.group(1)) not in nodes:
            nodes[int(match.group(1))] = (match.group(2), match.group(3))
    return values, nodes


def as_float(word: str) -> float:
    return struct.unpack(">f", bytes.fromhex(word))[0]


def main() -> int:
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[-1])
    a, b = digests(sys.argv[1]), digests(sys.argv[2])
    common = sorted(set(a) & set(b))
    print(f"{len(a)} and {len(b)} digests, {len(common)} ticks in common"
          + (f", up to tick {common[-1]}" if common else ""))
    first = next((tick for tick in common if a[tick] != b[tick]), None)
    if first is None:
        print("every tick in common agrees")
    else:
        earlier = [tick for tick in common if tick < first]
        print(f"first difference at tick {first}: {a[first]} and {b[first]}"
              + (f" (the last agreement at tick {earlier[-1]})" if earlier else ""))
    (values_a, nodes_a), (values_b, nodes_b) = objects(sys.argv[1]), objects(sys.argv[2])
    if values_a and values_b:
        differing = [index for index in sorted(set(values_a) & set(values_b))
                     if values_a[index] != values_b[index] or nodes_a.get(index) != nodes_b.get(index)]
        print(f"detail: {len(differing)} of {len(set(values_a) & set(values_b))} objects differ")
        for index in differing[:20]:
            fields = [(FIELDS[field], *(as_float(v[field]) if 2 <= field <= 12 else v[field]
                                        for v in (values_a[index], values_b[index])))
                      for field in range(14) if values_a[index][field] != values_b[index][field]]
            print(f"  object {int(values_a[index][0], 16)} (definition {values_a[index][1]}): {fields}"
                  + (f" nodes {nodes_a.get(index)} and {nodes_b.get(index)}"
                     if nodes_a.get(index) != nodes_b.get(index) else ""))
    return 0 if first is None else 1


if __name__ == "__main__":
    sys.exit(main())
