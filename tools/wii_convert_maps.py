#!/usr/bin/env python3
"""Converts the Xbox's maps for the Wii (port/wii/README.md, "Map conversion").

An Xbox map is a little-endian memory image: its tags are read to 0x803A6000 and hold absolute
pointers there, and each structure bsp is read to a fixed address after them. The Wii runs the
same code big-endian with its tag cache elsewhere (HALO_PORT_TAG_CACHE_BASE_ADDRESS), so every
value of the tags is byteswapped and every pointer into the tag cache moved, by the layouts and
the tag schema tools/wii_tag_layout.py reads from build/wii/halo.elf:

- the tags' header, their instance table and names;
- each tag's root and, down the schema, every block element, by its structure's layout;
- the data blocks whose contents are plain bytes (left), UTF-16 text (16-bit swaps) and so on,
  as DATA_CONTENTS says, field by field;
- each structure bsp's header and tags, where it loads;
- the vertex and index buffers' Direct3D headers.

Still to do, and reported for each map (the converted map is not playable until they are): the
vertex and index data, the animation frame data, the scripts' syntax nodes, the recorded
animations' event streams, and the bitmaps' pixels (to GX formats).

The output is uncompressed, its header big-endian with the Wii's version
(CACHE_FILE_VERSION_WII).

usage: tools/wii_convert_maps.py [--elf build/wii/halo.elf] [--output DIR] map.map...
"""

import argparse
import struct
import sys
import time
import zlib
from pathlib import Path
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wii_tag_layout import Definition, Executable, Schema, fourcc  # noqa: E402

XBOX_TAG_CACHE_ADDRESS = 0x803A6000
TAG_CACHE_SIZE = 0x1600000
HEADER_SIZE = 0x800
CACHE_FILE_VERSION_XBOX = 5
# the Wii's maps: the Xbox's version with 'WI' above it (port/linux/game/cache_file_formats.h)
CACHE_FILE_VERSION_WII = 0x57490005
SCENARIO_GROUP_TAG = 0x73636E72  # 'scnr'
STRUCTURE_BSP_GROUP_TAG = 0x73627370  # 'sbsp'
SCENARIO_STRUCTURE_BSPS_OFFSET = 0x5A4
BUFFER_SIZE = 12

# What each data block holds, by the definition and field that name it: "bytes" (left as they
# are), "utf16" (16-bit swaps), or "deferred" (not converted yet: reported).
DATA_CONTENTS: Dict[Tuple[str, str], str] = {
    ("font", "pixels"): "bytes",
    ("hs_source_file", "source"): "bytes",
    ("hud_message_text", "text_data"): "utf16",
    ("scenario", "hs_string_constants"): "bytes",
    ("scenario", "hs_syntax_data"): "deferred",
    ("sound_permutation", "mouth_data"): "bytes",
    ("sound_permutation", "subtitle_data"): "bytes",
    ("string_list_entry", "string"): "bytes",
    ("unicode_string_list_entry", "string"): "utf16",
    # (cluster and sound cluster data are kept in the tags but read by nothing)
    ("structure_bsp", "cluster_data"): "bytes",
    ("structure_bsp", "sound_cluster_data"): "bytes",
    ("animation", "data"): "deferred",
    ("animation", "default_data"): "deferred",
    ("animation", "frame_info"): "deferred",
    ("recorded_animation", "event_stream"): "deferred",
}


class Region:
    """bytes the game reads to one address: the tags, or a structure bsp"""

    def __init__(self, name: str, address: int, file_offset: int, size: int):
        self.name = name
        self.address = address
        self.file_offset = file_offset
        self.size = size
        self.covered = bytearray(size)

    def contains(self, address: int, size: int = 1) -> bool:
        return self.address <= address and address + size <= self.address + self.size

    def offset(self, address: int) -> int:
        return address - self.address + self.file_offset


class Plan:
    """a definition's byteswap: its scalars unpacked little-endian and packed big-endian (bytes
    as they are), and which of them are pointers"""

    def __init__(self, definition: Definition):
        formats = []
        self.pointers: List[int] = []
        position = 0
        for leaf in sorted(definition.leaves, key=lambda leaf: leaf.offset):
            if leaf.offset > position:
                formats.append(f"{leaf.offset - position}s")
            if leaf.kind == "bytes":
                formats.append(f"{leaf.size}s")
            else:
                if leaf.kind == "pointer":
                    self.pointers.append(len(formats))
                formats.append({2: "H", 4: "I", 8: "Q"}[leaf.size] if leaf.kind != "float" else
                               {4: "I", 8: "Q"}[leaf.size])
            position = leaf.offset + leaf.size
        if definition.size > position:
            formats.append(f"{definition.size - position}s")
        self.little = struct.Struct("<" + "".join(formats))
        self.big = struct.Struct(">" + "".join(formats))
        assert self.little.size == definition.size, (definition.name, self.little.size, definition.size)


class Converter:
    def __init__(self, elf: Path, tag_cache_address: int):
        started = time.monotonic()
        self.executable = Executable(elf)
        self.schema = Schema(self.executable)
        problems = self.schema.lay_out()
        if problems:
            raise SystemExit("the tag layouts have problems (tools/wii_tag_layout.py):\n  " + "\n  ".join(problems))
        self.plans = {id(definition): Plan(definition) for definition in self.schema.definitions.values()}
        self.delta = tag_cache_address - XBOX_TAG_CACHE_ADDRESS
        self.tag_data = self.executable.member_offsets("struct tag_data")
        self.header_plan = self._plan_of_type("struct tag_validate_header")
        self.instance_plan = self._plan_of_type("struct tag_validate_instance")
        self.bsp_header_plan = self._plan_of_type("struct tag_validate_structure_bsp_header")
        self.bsp_reference_plan = None
        print(f"layouts: {len(self.schema.groups)} groups, {len(self.schema.definitions)} definitions "
              f"({time.monotonic() - started:.0f} s)")

    def _plan_of_type(self, type_name: str) -> Plan:
        die = self.executable.types[type_name]
        definition = Definition(type_name, self.executable.size_of(die), type_name)
        definition.leaves = self.executable.struct_layout(type_name)
        return Plan(definition)

    # ---------- one map

    def convert(self, path: Path) -> Tuple[bytes, List[str]]:
        raw = path.read_bytes()
        (signature, version, file_length, _, tag_data_offset, tag_data_size) = struct.unpack_from("<6I", raw, 0)
        if signature != 0x68656164 or version != CACHE_FILE_VERSION_XBOX:
            raise ValueError(f"{path.name}: not an Xbox map (version {version})")
        image = raw if len(raw) == file_length else raw[:HEADER_SIZE] + zlib.decompress(raw[HEADER_SIZE:])
        if len(image) != file_length:
            raise ValueError(f"{path.name}: inflates to {len(image)} bytes, not {file_length}")
        self.source = image
        self.output = bytearray(image)
        self.notes: List[str] = []
        self.deferred: Dict[str, int] = {}

        # the header: its words big-endian, its version the Wii's
        self.output[0:24] = struct.pack(">6I", signature, CACHE_FILE_VERSION_WII, file_length, 0,
                                        tag_data_offset, tag_data_size)
        footer = struct.unpack_from("<I", image, HEADER_SIZE - 4)[0]
        self.output[HEADER_SIZE - 4:HEADER_SIZE] = struct.pack(">I", footer)

        tags = Region("tags", XBOX_TAG_CACHE_ADDRESS, tag_data_offset, tag_data_size)
        header = self.apply(self.header_plan, tags, XBOX_TAG_CACHE_ADDRESS)
        instances, scenario_index, _, tag_count, vertex_count, vertex_buffers, index_count, index_buffers, _ = header
        self.buffers(tags, vertex_buffers, vertex_count, index_buffers, index_count)

        missing_groups: Dict[str, int] = {}
        scenario = None
        for index in range(tag_count):
            instance = self.apply(self.instance_plan, tags, instances + index * self.instance_plan.little.size)
            group_tag, _, _, tag_index, name, base_address = instance[:6]
            self.mark_string(tags, name)
            group = self.schema.groups.get(group_tag)
            if group_tag == SCENARIO_GROUP_TAG:
                scenario = base_address
            if group_tag == STRUCTURE_BSP_GROUP_TAG:
                continue  # (in its own region: below)
            if group is None or group.definition is None:
                missing_groups[fourcc(group_tag)] = missing_groups.get(fourcc(group_tag), 0) + 1
                continue
            if not tags.contains(base_address, group.definition.size):
                self.notes.append(f"tag {index} ({fourcc(group_tag)}) is outside the tags")
                continue
            self.walk(group.definition, tags, base_address, 1, swap=True)
        if missing_groups:
            self.notes.append("tags of groups the schema has no definition for (left as they are): " +
                              ", ".join(f"{group} x{count}" for group, count in sorted(missing_groups.items())))

        # the structure bsps, each where it loads
        bsp_group = self.schema.groups.get(STRUCTURE_BSP_GROUP_TAG)
        if scenario is not None:
            count, address = struct.unpack_from("<iI", image, tags.offset(scenario + SCENARIO_STRUCTURE_BSPS_OFFSET))
            for index in range(count):
                reference = tags.offset(address + index * 0x20)
                file_offset, file_size, base_address = struct.unpack_from("<iiI", image, reference)
                bsp = Region(f"bsp {index}", base_address, file_offset, file_size)
                header = self.apply(self.bsp_header_plan, bsp, base_address)
                root, vertex_count, vertex_buffers, index_count, index_buffers, _ = header
                self.buffers(bsp, vertex_buffers, vertex_count, index_buffers, index_count)
                if bsp_group and bsp_group.definition and bsp.contains(root, bsp_group.definition.size):
                    self.walk(bsp_group.definition, bsp, root, 1, swap=True)
                else:
                    self.notes.append(f"structure bsp {index}'s root is outside it")
                self.report_coverage(bsp)
        self.report_coverage(tags)
        for what, count in sorted(self.deferred.items()):
            self.notes.append(f"not converted yet: {what} ({count} bytes)")
        return bytes(self.output), self.notes

    # ---------- the walk

    def apply(self, plan: Plan, region: Region, address: int) -> tuple:
        """one structure swapped (its pointers into the tag cache moved): its little-endian values"""
        offset = region.offset(address)
        values = plan.little.unpack_from(self.source, offset)
        out = list(values)
        for index in plan.pointers:
            pointer = out[index]
            if XBOX_TAG_CACHE_ADDRESS <= pointer < XBOX_TAG_CACHE_ADDRESS + TAG_CACHE_SIZE:
                out[index] = pointer + self.delta
        plan.big.pack_into(self.output, offset, *out)
        start = address - region.address
        region.covered[start:start + plan.little.size] = b"\x01" * plan.little.size
        return values

    def walk(self, definition: Definition, region: Region, address: int, count: int, swap: bool):
        plan = self.plans[id(definition)]
        for element in range(count):
            base = address + element * definition.size
            if swap:
                self.apply(plan, region, base)
            for field in definition.fields:
                if field.type == "block" and field.definition is not None:
                    for item in range(field.count):
                        at = region.offset(base + field.offset + item * field.size)
                        block_count, block_address = struct.unpack_from("<iI", self.source, at)
                        if block_count <= 0:
                            continue
                        if not region.contains(block_address, block_count * field.definition.size):
                            self.notes.append(f"{definition.name}.{field.name}: a block outside {region.name}")
                            continue
                        self.walk(field.definition, region, block_address, block_count, swap=True)
                elif field.type == "struct" and field.definition is not None:
                    # (its scalars are its parent's, swapped already: its blocks and data only)
                    self.walk(field.definition, region, base + field.offset, field.count, swap=False)
                elif field.type == "data":
                    self.data(definition, field, region, base + field.offset)

    def data(self, definition: Definition, field, region: Region, address: int):
        at = region.offset(address)
        size = struct.unpack_from("<i", self.source, at + self.tag_data["size"])[0]
        data_address = struct.unpack_from("<I", self.source, at + self.tag_data["address"])[0]
        if size <= 0:
            return
        if not region.contains(data_address, size):
            self.notes.append(f"{definition.name}.{field.name}: data outside {region.name}")
            return
        contents = DATA_CONTENTS.get((definition.name, field.name))
        start = data_address - region.address
        if contents is None:
            self.notes.append(f"{definition.name}.{field.name}: data of unknown contents (left as they are)")
            contents = "deferred"
        if contents == "utf16":
            offset = region.offset(data_address)
            words = struct.unpack_from(f"<{size // 2}H", self.source, offset)
            struct.pack_into(f">{size // 2}H", self.output, offset, *words)
        elif contents == "deferred":
            what = f"{definition.name}.{field.name}"
            self.deferred[what] = self.deferred.get(what, 0) + size
        region.covered[start:start + size] = b"\x01" * size

    def buffers(self, region: Region, vertex_buffers: int, vertex_count: int, index_buffers: int, index_count: int):
        """the Direct3D headers of the vertex and index buffers: Common, Data (moved), Lock"""
        for address, count in ((vertex_buffers, vertex_count), (index_buffers, index_count)):
            if count <= 0 or not region.contains(address, count * BUFFER_SIZE):
                continue
            offset = region.offset(address)
            words = list(struct.unpack_from(f"<{count * 3}I", self.source, offset))
            for index in range(1, len(words), 3):
                if XBOX_TAG_CACHE_ADDRESS <= words[index] < XBOX_TAG_CACHE_ADDRESS + TAG_CACHE_SIZE:
                    words[index] += self.delta
            struct.pack_into(f">{count * 3}I", self.output, offset, *words)
            start = address - region.address
            region.covered[start:start + count * BUFFER_SIZE] = b"\x01" * (count * BUFFER_SIZE)
        if vertex_count > 0 or index_count > 0:
            self.deferred["vertex and index data"] = self.deferred.get("vertex and index data", 0)

    def mark_string(self, region: Region, address: int):
        if not region.contains(address):
            return
        offset = region.offset(address)
        end = self.source.index(b"\0", offset)
        start = address - region.address
        region.covered[start:start + end - offset + 1] = b"\x01" * (end - offset + 1)

    def report_coverage(self, region: Region):
        uncovered = region.size - sum(region.covered)
        self.notes.append(f"{region.name}: {region.size} bytes, {uncovered} not reached by the walk "
                          f"({100.0 * uncovered / max(region.size, 1):.1f}%: vertex and index data, padding)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("maps", nargs="+", type=Path)
    parser.add_argument("--elf", type=Path, default=Path("build/wii/halo.elf"))
    parser.add_argument("--output", type=Path, default=Path("build/wii/sd/apps/halo/maps"))
    parser.add_argument("--tag-cache-address", type=lambda text: int(text, 0), default=0x90100000,
                        help="where the Wii reads the tags (HALO_PORT_TAG_CACHE_BASE_ADDRESS)")
    args = parser.parse_args()
    converter = Converter(args.elf, args.tag_cache_address)
    args.output.mkdir(parents=True, exist_ok=True)
    for path in args.maps:
        started = time.monotonic()
        converted, notes = converter.convert(path)
        (args.output / path.name).write_bytes(converted)
        print(f"{path.name}: {len(converted)} bytes ({time.monotonic() - started:.0f} s)")
        for note in notes:
            print(f"  {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
