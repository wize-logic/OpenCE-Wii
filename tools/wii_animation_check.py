#!/usr/bin/env python3
"""Checks the animations of converted maps against the Xbox's (port/wii/README.md, "Map conversion").

For each map, tools/wii_convert_maps.py's walk finds the animations; then each animation's data is
read the way the game reads it (source/models/model_animations.c), once from the Xbox's map
little-endian and once from the converted map big-endian, and the values are compared element by
element:

- frame info: its floats;
- default data and uncompressed frames: node by node, a compressed_quaternion_8byte's four
  shorts, a translation's three floats, a scale (animation_get_node_orientations);
- compressed data: the header's offsets, each node header, and each node's keyframe frame
  indices, keyframes (a compressed_quaternion_6byte decompressed to its four shorts as
  quaternion_decompress_6byte does, a real_point3d, a real) and default, through the node
  header's first keyframe and count, as animation_get_keyframe_rotation, _translation and
  _scale index them.

Floats are compared by their bits. The decompressed rotations are also checked for being near
unit quaternions, which says the Xbox's side was read as the game reads it.

usage: tools/wii_animation_check.py [--elf build/wii/halo.elf] [--converted DIR] map.map...
(without --converted, the maps are converted in memory)
"""

import argparse
import math
import struct
import sys
from pathlib import Path
from typing import Iterator, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wii_convert_maps import Converter, KEYFRAME_COUNT_BITS  # noqa: E402

COMPRESSED_QUATERNION_COMPONENT_MAXIMUM = 32767


def signed16(value: int) -> int:
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def quaternion_6byte(words) -> Tuple[int, int, int, int]:
    """quaternion_decompress_6byte's four components, before scaling"""
    word0, word1, word2 = words
    i = signed16((word0 >> 12) | (word0 & 0xFFF0))
    j = signed16(((word1 >> 4) & 0x0FF0) | (word0 & 0x000F) | (word0 << 12))
    k = signed16(((((word2 >> 4) & 0x0F00) | (word1 & 0x00F0)) >> 4) | (word1 << 8))
    w = signed16(((word2 >> 8) & 0x000F) | (word2 << 4))
    return i, j, k, w


class Reader:
    def __init__(self, data: bytes, endian: str):
        self.data, self.endian = data, endian

    def get(self, code: str, offset: int):
        return struct.unpack_from(self.endian + code, self.data, offset)


def decode(reader: Reader, animation: dict) -> Iterator[Tuple[str, object]]:
    """the animation's values in the order the game reads them, each (what, value)"""
    node_count, moving = animation["node_count"], animation["moving"]
    if "frame_info" in animation:
        offset, size = animation["frame_info"]
        yield "frame info", reader.get(f"{size // 4}I", offset)

    def nodes(position: int, animated: bool, what: str):
        for node in range(node_count):
            if bool(moving["rotation"] >> node & 1) == animated:
                yield f"{what} rotation", reader.get("4h", position)
                position += 8
            if bool(moving["translation"] >> node & 1) == animated:
                yield f"{what} translation", reader.get("3I", position)
                position += 12
            if bool(moving["scale"] >> node & 1) == animated:
                yield f"{what} scale", reader.get("I", position)
                position += 4

    if "default_data" in animation:
        offset, size = animation["default_data"]
        yield from nodes(offset, False, "default")
    if "data" not in animation:
        return
    offset, size = animation["data"]
    compressed_offset = animation["compressed_offset"]
    compressed = animation["flags"] & 1
    if not compressed or compressed_offset:
        for frame in range(animation["frame_count"]):
            yield from nodes(offset + frame * animation["frame_size"], True, "frame")
    if not compressed:
        return
    block = offset + compressed_offset
    end = offset + size
    header = reader.get("11i", block)
    yield "compressed header", header
    (rotation_indices, default_rotations, rotation_keyframes, translation_headers, translation_indices,
     default_translations, translation_keyframes, scale_headers, scale_indices, default_scales,
     scale_keyframes) = header

    def inside(relative: int, index: int, count: int, width: int) -> bool:
        return 0 <= relative and block + relative + width * (index + count) <= end

    def kind(name: str, headers: int, indices: int, defaults: int, keyframes: int, width: int, code: str):
        adjusted = 0
        for node in range(node_count):
            if inside(defaults, node if name != "scale" else adjusted, 1, width) and \
                    (name != "scale" or moving["scale"] >> node & 1):
                index = node if name != "scale" else adjusted
                value = reader.get(code, block + defaults + width * index)
                yield f"default {name}", quaternion_6byte(value) if name == "rotation" else value
            if not moving[name] >> node & 1:
                continue
            if not inside(headers, adjusted, 1, 4):
                yield f"{name} node header outside", adjusted
                adjusted += 1
                continue
            node_header = reader.get("I", block + headers + 4 * adjusted)[0]
            yield f"{name} node header", node_header
            first, count = node_header >> KEYFRAME_COUNT_BITS, node_header & ((1 << KEYFRAME_COUNT_BITS) - 1)
            if count and inside(indices, first, count, 2) and inside(keyframes, first, count, width):
                yield f"{name} keyframe frame indices", reader.get(f"{count}H", block + indices + 2 * first)
                for keyframe in range(first, first + count):
                    value = reader.get(code, block + keyframes + width * keyframe)
                    yield f"{name} keyframe", quaternion_6byte(value) if name == "rotation" else value
            elif count:
                yield f"{name} keyframes outside", (first, count)
            adjusted += 1

    yield from kind("rotation", 0x2C, rotation_indices, default_rotations, rotation_keyframes, 6, "3H")
    yield from kind("translation", translation_headers, translation_indices, default_translations,
                    translation_keyframes, 12, "3I")
    yield from kind("scale", scale_headers, scale_indices, default_scales, scale_keyframes, 4, "I")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("maps", nargs="+", type=Path)
    parser.add_argument("--elf", type=Path, default=Path("build/wii/halo.elf"))
    parser.add_argument("--converted", type=Path, help="the folder of the converted maps")
    parser.add_argument("--tag-cache-address", type=lambda text: int(text, 0), default=0x90100000)
    args = parser.parse_args()
    converter = Converter(args.elf, args.tag_cache_address)
    failed = False
    for path in args.maps:
        output, _ = converter.convert(path)
        if args.converted:
            output = (args.converted / path.name).read_bytes()
        source = Reader(converter.source, "<")
        converted = Reader(output, ">")
        animations = compressed = values = differing = outside = 0
        far_from_unit = rotations = 0
        examples = []
        for animation in converter.animations.values():
            animations += 1
            compressed += animation["flags"] & 1 and "data" in animation
            little = list(decode(source, animation))
            big = list(decode(converted, animation))
            if len(little) != len(big):
                differing += 1
                examples.append(f"an animation reads {len(little)} values little-endian, {len(big)} big-endian")
                continue
            for (what, a), (_, b) in zip(little, big):
                values += 1
                if "outside" in what:
                    outside += 1
                elif a != b:
                    differing += 1
                    if len(examples) < 5:
                        examples.append(f"{what}: {a} on the Xbox, {b} converted")
                if what.endswith("rotation") or what == "rotation keyframe":
                    rotations += 1
                    norm = math.sqrt(sum(c * c for c in b)) / COMPRESSED_QUATERNION_COMPONENT_MAXIMUM
                    far_from_unit += abs(norm - 1) > 0.05
        print(f"{path.name}: {animations} animations ({compressed} compressed), {values} values read, "
              f"{differing} differ, {outside} outside their data; rotations not near unit: "
              f"{far_from_unit} of {rotations}")
        for example in examples:
            print(f"  {example}")
        failed |= differing > 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
