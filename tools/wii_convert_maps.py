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
- the vertex and index buffers' Direct3D headers, and their vertices and indices;
- the scripts' syntax nodes, the animations' frames and the recorded animations' event streams;
- the bitmaps' pixels, in GX's formats (tools/wii_gx_textures.py).

What the walk does not reach is reported for each map.

The output is uncompressed, which the Wii reads where it is (the Xbox inflated its maps to a cache
partition; source/cache/cache_files_windows.c, HALO_WII), its header big-endian with the Wii's
version (CACHE_FILE_VERSION_WII, port/linux/game/cache_file_formats.h).

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
import wii_gx_textures  # noqa: E402

XBOX_TAG_CACHE_ADDRESS = 0x803A6000
TAG_CACHE_SIZE = 0x1600000
HEADER_SIZE = 0x800
CACHE_FILE_VERSION_XBOX = 5
# the Wii's maps: the Xbox's version with 'WI' above it (port/linux/game/cache_file_formats.h)
CACHE_FILE_VERSION_WII = 0x57490005
SCENARIO_GROUP_TAG = 0x73636E72  # 'scnr'
STRUCTURE_BSP_GROUP_TAG = 0x73627370  # 'sbsp'
BITMAP_GROUP_TAG = 0x6269746D  # 'bitm'
BITMAP_TYPE_2D = 0
BITMAP_TYPE_CUBE = 2
BITMAP_GROUP_TYPE_INTERFACE = 4
# struct bitmap_data's flags (source/bitmaps/bitmaps.c)
BITMAP_POWER_OF_TWO_BIT, BITMAP_COMPRESSED_BIT, BITMAP_LINEAR_BIT, BITMAP_V16U16_BIT = 0, 1, 4, 5
SCENARIO_STRUCTURE_BSPS_OFFSET = 0x5A4
BUFFER_SIZE = 12

# What each data block holds, by the definition and field that name it: "bytes" (left as they
# are), "utf16" (16-bit swaps), "longs" (32-bit swaps), "hs_syntax" (the scripts' syntax nodes:
# hs_syntax below), or "deferred" (not converted yet: reported).
DATA_CONTENTS: Dict[Tuple[str, str], str] = {
    ("font", "pixels"): "bytes",
    ("hs_source_file", "source"): "bytes",
    ("hud_message_text", "text_data"): "utf16",
    ("scenario", "hs_string_constants"): "bytes",
    ("scenario", "hs_syntax_data"): "hs_syntax",
    ("sound_permutation", "mouth_data"): "bytes",
    ("sound_permutation", "subtitle_data"): "bytes",
    ("string_list_entry", "string"): "bytes",
    ("unicode_string_list_entry", "string"): "utf16",
    # (each cluster's potentially visible set: bit vectors the game reads a long at a time,
    # structure_bsp_get_cluster_pvs; the sound clusters' is read a byte at a time)
    ("structure_bsp", "cluster_data"): "longs",
    ("structure_bsp", "sound_cluster_data"): "bytes",
    ("animation", "data"): "animation",
    ("animation", "default_data"): "animation",
    ("animation", "frame_info"): "animation",
    ("recorded_animation", "event_stream"): "recorded_animation",
    ("structure_material", "compressed_vertex_data"): "bsp_vertices",
    ("structure_material", "uncompressed_vertex_data"): "bsp_vertices",
}


# The vertex types the maps' geometry is in (source/rasterizer/rasterizer_geometry.h), each a
# vertex's values in order as the game's vertex declarations read them
# (rasterizer_xbox_vertex_shaders_initialize.c): f a float, p a packed normal (11:11:10 bits in a
# 32-bit word, which a 32-bit swap keeps), h a short, b a byte.
VERTEX_FORMATS: Dict[int, str] = {
    0: "fff" "fff" "fff" "fff" "ff",          # environment, uncompressed: 56 bytes
    1: "fff" "ppp" "ff",                       # environment, compressed: 32
    2: "fff" "ff",                             # lightmap, uncompressed: 20
    3: "p" "hh",                               # lightmap, compressed: 8
    4: "fff" "fff" "fff" "fff" "ff" "hh" "ff",  # model, uncompressed: 68
    5: "fff" "ppp" "hh" "bb" "h",              # model, compressed: 32
}
VERTEX_STRUCT_CODES = {"f": "I", "p": "I", "h": "H", "b": "B"}
TRIANGLE_BUFFER_STRIP = 1

# A recorded animation's stream (source/cutscene): its unit control's fields by version
# (recorded_animation_initialize.c), each (width, count): width 1 is bytes, which keep their order
UNIT_CONTROL_FIELDS = [
    [(1, 1), (1, 1), (2, 1), (2, 1), (2, 1), (4, 2), (4, 3), (4, 3), (4, 3)],
    [(4, 1)],
    [(2, 1)],
    [(2, 1)],
]
# each event's data, by type: the version 1 streams' after their 4-byte header
# (recorded_animation_playback_v1.c), the current ones' after their header byte and time delta
# (recorded_animation_playback.c)
EVENTS_V1 = {0: [], 1: [], 2: [(1, 2)], 3: [(1, 2)], 4: [(2, 1)], 5: [(2, 1)], 6: [(4, 2)], 7: [], 8: [],
             **{event: [(4, 3)] for event in range(9, 16)}, **{event: [(4, 2)] for event in range(16, 23)}}
EVENTS = {0: [], 1: [], 2: [(1, 1)], 3: [(1, 1)], 4: [(2, 1)], 5: [(2, 1)], 6: [(4, 2)],
          **{event: [(1, 2)] for event in range(7, 15)}, **{event: [(2, 2)] for event in range(15, 23)}}
RECORDED_ANIMATION_END = 1

# A node header of an animation's compressed data: its first keyframe above, its keyframe count in
# the low 12 bits (COMPRESSED_ANIMATION_NODE_HEADER_KEYFRAME_COUNT_BITS, model_animations.c)
KEYFRAME_COUNT_BITS = 12


def keyframe_count(data: bytes, offset: int, end: int, endian: str, node_count: int) -> int:
    """how many keyframes the node headers at offset reach: the largest first keyframe plus count"""
    total = 0
    for node in range(node_count):
        if offset + 4 * node + 4 > end:
            break
        header = struct.unpack_from(endian + "I", data, offset + 4 * node)[0]
        count = header & ((1 << KEYFRAME_COUNT_BITS) - 1)
        if count:
            total = max(total, (header >> KEYFRAME_COUNT_BITS) + count)
    return total


def compressed_animation_sections(data: bytes, block: int, size: int, endian: str, node_count: int,
                                  rotating: int, translating: int, scaling: int) -> List[Tuple[str, int, int, int]]:
    """the sections of an animation's compressed data at block, as model_animations.c reads them
    (struct compressed_animation_header; animation_get_keyframe_rotation, _translation, _scale and
    animation_get_node_orientations): each (name, offset from block, element width, element
    count). The header's eleven offsets are followed by one node header (a long) per rotating
    node; a rotation is a compressed_quaternion_6byte, three words; a keyframe's frame index a
    word; a translation a real_point3d; a scale a real. The default rotations and translations
    are one per node (by node index), the default scales one per scaling node (by its adjusted
    index), the keyframes as many as the node headers reach"""
    end = block + size
    (rotation_indices, default_rotations, rotation_keyframes, translation_headers, translation_indices,
     default_translations, translation_keyframes, scale_headers, scale_indices, default_scales,
     scale_keyframes) = struct.unpack_from(endian + "11i", data, block)

    def reach(offset: int, count: int) -> int:
        return keyframe_count(data, block + offset, end, endian, count) if 0 <= offset else 0

    rotations = reach(0x2C, rotating)
    translations = reach(translation_headers, translating)
    scales = reach(scale_headers, scaling)
    return [
        ("header", 0, 4, 11),
        ("rotation node headers", 0x2C, 4, rotating),
        ("rotation keyframe frame indices", rotation_indices, 2, rotations),
        ("default rotations", default_rotations, 2, 3 * node_count),
        ("rotation keyframes", rotation_keyframes, 2, 3 * rotations),
        ("translation node headers", translation_headers, 4, translating),
        ("translation keyframe frame indices", translation_indices, 2, translations),
        ("default translations", default_translations, 4, 3 * node_count),
        ("translation keyframes", translation_keyframes, 4, 3 * translations),
        ("scale node headers", scale_headers, 4, scaling),
        ("scale keyframe frame indices", scale_indices, 2, scales),
        ("default scales", default_scales, 4, scaling),
        ("scale keyframes", scale_keyframes, 4, scales),
    ]


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
        self.definition = definition
        # (the placeholders and gaps, left as they are, by name and offset: the report of those
        # holding data, and tools/wii_placeholder_check.py's of whether the game reads them)
        self.byte_runs: List[Tuple[int, str, int, int]] = []
        position = 0
        for leaf in sorted(definition.leaves, key=lambda leaf: leaf.offset):
            if leaf.offset > position:
                self.byte_runs.append((len(formats), f"{definition.name}+{position:#x}", position,
                                       leaf.offset - position))
                formats.append(f"{leaf.offset - position}s")
            if leaf.kind == "bytes":
                if leaf.size >= 2 and leaf.name.split(".")[-1].startswith(Schema.PLACEHOLDER_PREFIXES):
                    self.byte_runs.append((len(formats), f"{definition.name}.{leaf.name.split('.', 1)[-1]}",
                                           leaf.offset, leaf.size))
                formats.append(f"{leaf.size}s")
            else:
                if leaf.kind == "pointer":
                    self.pointers.append(len(formats))
                formats.append({2: "H", 4: "I", 8: "Q"}[leaf.size] if leaf.kind != "float" else
                               {4: "I", 8: "Q"}[leaf.size])
            position = leaf.offset + leaf.size
        if definition.size > position:
            self.byte_runs.append((len(formats), f"{definition.name}+{position:#x}", position,
                                   definition.size - position))
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
        # (the schema's placeholders the game reads values in: wii_tag_layout.py's fill_placeholders)
        filled, _ = self.schema.fill_placeholders()
        for line in filled:
            print(f"layouts: filled {line}")
        self.plans = {id(definition): Plan(definition) for definition in self.schema.definitions.values()}
        self.vertex_structs = {vertex_type: (struct.Struct("<" + "".join(VERTEX_STRUCT_CODES[c] for c in format)),
                                             struct.Struct(">" + "".join(VERTEX_STRUCT_CODES[c] for c in format)))
                               for vertex_type, format in VERTEX_FORMATS.items()}
        # each definition's vertex and triangle buffers (struct vertex_buffer, struct
        # triangle_buffer): where in an element, and which
        self.buffer_fields = {}
        for definition in self.schema.definitions.values():
            names = {leaf.name.split(".", 1)[1] if "." in leaf.name else leaf.name: leaf.offset
                     for leaf in definition.leaves}
            found = []
            for name, offset in names.items():
                if name.endswith(".hardware_format"):
                    prefix = name[:-len(".hardware_format")]
                    if f"{prefix}.type" in names and f"{prefix}.count" in names:
                        kind = "vertex" if f"{prefix}.offset" in names else "triangle"
                        found.append((kind, names[f"{prefix}.type"]))
            if found:
                self.buffer_fields[id(definition)] = found
        self.delta = tag_cache_address - XBOX_TAG_CACHE_ADDRESS
        self.tag_data = self.executable.member_offsets("struct tag_data")
        self.header_plan = self._plan_of_type("struct tag_validate_header")
        self.instance_plan = self._plan_of_type("struct tag_validate_instance")
        self.bsp_header_plan = self._plan_of_type("struct tag_validate_structure_bsp_header")
        self.data_array_plan = self._plan_of_type("struct data_array")
        self.bitmap_group = self.executable.member_offsets("struct bitmap_group")
        self.bitmap_data = self.executable.member_offsets("struct bitmap_data")
        self.bitmap_data_size = self.executable.size_of(self.executable.types["struct bitmap_data"])
        # the scripts' value sizes (source/hs/hs.c), as the executable holds them
        sizes = self.executable.symbols["hs_type_sizes"]
        count = self.executable.symbol_sizes["hs_type_sizes"] // 2
        self.hs_type_sizes = list(struct.unpack(f">{count}h", self.executable.read(sizes, 2 * count)))
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
        self.vertex_jobs: Dict[int, Tuple[int, int]] = {}
        self.triangle_jobs: Dict[int, Tuple[int, int]] = {}
        self.buffer_regions: List[Region] = []
        self.bitmaps: Dict[str, int] = {}
        self.unswapped: Dict[str, int] = {}
        # (the bytes of each of those that hold data in some element, as an integer whose bytes
        # are the run's, by definition and offset: tools/wii_placeholder_check.py)
        self.unswapped_bytes: Dict[Tuple[int, int], int] = {}
        # each animation's data (its file offset, size and what the game reads it by), for
        # tools/wii_animation_check.py
        self.animations: Dict[int, dict] = {}

        # the header (source/cache/cache_files_windows.c's struct cache_file_header, the one
        # that names every field the game reads): its values big-endian, its version the Wii's;
        # the name, the build and the reserved bytes as they are
        struct.pack_into(">III", self.output, 0, signature, CACHE_FILE_VERSION_WII, file_length)
        struct.pack_into(">II", self.output, 0x10, tag_data_offset, tag_data_size)
        scenario_type, pad, checksum = struct.unpack_from("<hhI", image, 0x60)
        struct.pack_into(">hhI", self.output, 0x60, scenario_type, pad, checksum)
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
            if group_tag == BITMAP_GROUP_TAG:
                self.bitmap_pixels(tags, base_address)
        if missing_groups:
            self.notes.append("tags of groups the game reads nothing of (left as they are): " +
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
                self.buffer_data(bsp)
                self.report_coverage(bsp)
        self.buffer_data(tags)
        self.report_coverage(tags)
        self.notes.append("bitmaps in GX's formats: " + ", ".join(
            f"{kind} {count}" for kind, count in sorted(self.bitmaps.items())))
        for what, count in sorted(self.deferred.items()):
            self.notes.append(f"not converted yet: {what} ({count} bytes)")
        if self.unswapped:
            # (placeholders and gaps not zero: a field the game reads would be left little-endian;
            # tools/wii_placeholder_check.py looks for the game's reads of each)
            self.notes.append("placeholders left as they are that hold data (tools/wii_placeholder_check.py "
                              "checks the game reads none): " + ", ".join(
                f"{name} x{count}" for name, count in sorted(self.unswapped.items(), key=lambda item: -item[1])))
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
        for index, name, run, _ in plan.byte_runs:
            if values[index].strip(b"\0"):
                self.unswapped[name] = self.unswapped.get(name, 0) + 1
                key = (id(plan.definition), run)
                self.unswapped_bytes[key] = self.unswapped_bytes.get(key, 0) | int.from_bytes(values[index], "big")
        start = address - region.address
        region.covered[start:start + plan.little.size] = b"\x01" * plan.little.size
        return values

    def walk(self, definition: Definition, region: Region, address: int, count: int, swap: bool):
        plan = self.plans[id(definition)]
        for element in range(count):
            base = address + element * definition.size
            if swap:
                self.apply(plan, region, base)
            for kind, offset in self.buffer_fields.get(id(definition), ()):
                at = region.offset(base + offset)
                buffer_type, count = struct.unpack_from("<hxxi", self.source, at)
                hardware = struct.unpack_from("<I", self.source, at + (16 if kind == "vertex" else 12))[0]
                if count > 0 and hardware:
                    jobs = self.vertex_jobs if kind == "vertex" else self.triangle_jobs
                    jobs[hardware] = (buffer_type, count)
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
                    self.data(definition, field, region, base + field.offset, base)

    def data(self, definition: Definition, field, region: Region, address: int, element: int):
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
        elif contents == "longs":
            self.swap_words(region.offset(data_address), size - size % 4, 4)
        elif contents == "hs_syntax":
            self.hs_syntax(region, data_address, size)
        elif contents == "recorded_animation":
            self.recorded_animation(definition, region, element, data_address, size)
        elif contents == "animation":
            self.animation(definition, field, region, element, data_address, size)
        elif contents == "bsp_vertices":
            self.bsp_vertices(definition, field, region, element, data_address, size)
        elif contents == "deferred":
            what = f"{definition.name}.{field.name}"
            self.deferred[what] = self.deferred.get(what, 0) + size
        region.covered[start:start + size] = b"\x01" * size

    def hs_syntax(self, region: Region, address: int, size: int):
        """the scripts' syntax nodes: a data array's header, then its nodes. A node's value is a
        32-bit cell holding a boolean, short, long or real by the node's type, which the game reads
        both as the whole cell and as its narrow member (port/linux/include/halo_cells.h): it is
        written as the 32-bit number the Xbox's cell held, a constant's widened from its type's
        size (hs_type_sizes), a call's or global's (an expression or global index) as it is"""
        header = self.apply(self.data_array_plan, region, address)
        offset = region.offset(address) + self.data_array_plan.little.size
        end = region.offset(address) + size
        node = struct.Struct("<hhhhiiI")
        node_big = struct.Struct(">hhhhiiI")
        while offset + node.size <= end:
            datum, index, node_type, flags, next_node, source, data = node.unpack_from(self.source, offset)
            primitive, global_value = flags & 1, flags & 4
            if primitive and not global_value and 0 <= node_type < len(self.hs_type_sizes):
                width = self.hs_type_sizes[node_type]
                if width == 1:
                    data = data & 0xFF
                elif width == 2:
                    data = (data & 0xFFFF) - 0x10000 if data & 0x8000 else data & 0xFFFF
                    data &= 0xFFFFFFFF
            node_big.pack_into(self.output, offset, datum, index, node_type, flags, next_node, source, data)
            offset += node.size

    def animation(self, definition: Definition, field, region: Region, element: int, address: int, size: int):
        """an animation's frames (source/models/model_animations.c): its frame info all floats
        (dx dy, dx dy dyaw or dx dy dz dyaw); its default data and uncompressed frames, node by
        node, a rotation four shorts (compressed_quaternion_8byte), a translation three floats, a
        scale a float (frames for the nodes the flags say move, the default for the others); and
        its compressed data, by the sections compressed_animation_sections names"""
        leaves = {leaf.name.split(".", 1)[1]: leaf.offset for leaf in definition.leaves}

        def value(name, code="<h"):
            return struct.unpack_from(code, self.source, region.offset(element + leaves[name]))[0]

        offset = region.offset(address)
        node_count, frame_count, frame_size = value("node_count"), value("frame_count"), value("frame_size")
        flags = value("flags", "<H")
        moving = {kind: (value(f"nodes_with_{kind}_flags[0]", "<I") | value(f"nodes_with_{kind}_flags[1]", "<I") << 32)
                  for kind in ("rotation", "translation", "scale")}
        record = self.animations.setdefault(region.offset(element), {
            "node_count": node_count, "frame_count": frame_count, "frame_size": frame_size, "flags": flags,
            "moving": moving, "compressed_offset": value("compressed_data_offset", "<i")})
        record[field.name] = (offset, size)
        if field.name == "frame_info":
            self.swap_words(offset, size - size % 4, 4)
            return
        widths = {"rotation": (2, 4), "translation": (4, 3), "scale": (4, 1)}

        def nodes(position: int, animated: bool) -> int:
            for node in range(node_count):
                for kind in ("rotation", "translation", "scale"):
                    if bool(moving[kind] >> node & 1) == animated:
                        width, count = widths[kind]
                        self.swap_words(position, width * count, width)
                        position += width * count
            return position

        if field.name == "default_data":
            end = nodes(offset, False)
            if end - offset != size:
                self.notes.append(f"an animation's default data is {size} bytes, not {end - offset}")
            return
        compressed_offset = record["compressed_offset"]
        compressed = flags & 1
        uncompressed = frame_count * frame_size if (not compressed or compressed_offset) else 0
        position = offset
        for frame in range(frame_count if uncompressed else 0):
            end = nodes(position, True)
            if end - position != frame_size:
                self.notes.append(f"an animation's frame is {frame_size} bytes, not {end - position}")
                break
            position = end
        if not compressed:
            return
        block, block_size = offset + compressed_offset, size - compressed_offset
        if not 0 <= compressed_offset <= size - 0x2C:
            self.notes.append(f"an animation's compressed data is outside it ({compressed_offset} of {size} bytes)")
            return
        counts = [bin(moving[kind] & ((1 << node_count) - 1)).count("1") for kind in ("rotation", "translation", "scale")]
        covered = bytearray(block_size)
        for name, start, width, count in compressed_animation_sections(self.source, block, block_size, "<",
                                                                       node_count, *counts):
            if count == 0:
                continue
            if not 0 <= start <= start + width * count <= block_size:
                self.notes.append(f"an animation's compressed {name} are outside its data")
                continue
            if any(covered[start:start + width * count]):
                self.notes.append(f"an animation's compressed {name} overlap another section")
            covered[start:start + width * count] = b"\x01" * (width * count)
            self.swap_words(block + start, width * count, width)
        left = block_size - sum(covered)
        if left:
            self.notes.append(f"an animation's compressed data has {left} of {block_size} bytes in no section")

    def recorded_animation(self, definition: Definition, region: Region, element: int, address: int, size: int):
        """a recorded animation's event stream: its unit control (the fields of each version up to
        its own), the current format's animation state (three yaw and pitch shorts), then its
        events up to the end event. A current event's header is a byte of two bitfields, time delta
        (2 bits) then type (6), which MSVC packs from the low bit and a big-endian compiler from
        the high one: it is rewritten so; a time delta of a word follows it, unaligned"""
        leaves = {leaf.name.split(".", 1)[1]: leaf.offset for leaf in definition.leaves}
        version = self.source[region.offset(element + leaves["version"])]
        control_version = self.source[region.offset(element + leaves["unit_control_data_version"])]
        start = region.offset(address)
        end = start + size
        position = start

        def swap(fields):
            nonlocal position
            for width, count in fields:
                if width > 1:
                    self.swap_words(position, width * count, width)
                position += width * count

        for fields in UNIT_CONTROL_FIELDS[:min(max(control_version, 1), len(UNIT_CONTROL_FIELDS))]:
            swap(fields)
        if version == 4:
            swap([(2, 6)])
            while position < end:
                header = self.source[position]
                time_delta, event = header & 3, header >> 2
                self.output[position] = (time_delta << 6) | event
                position += 1
                if time_delta == 2:
                    position += 1
                elif time_delta == 3:
                    self.swap_words(position, 2, 2)
                    position += 2
                if event == RECORDED_ANIMATION_END or event not in EVENTS:
                    break
                swap(EVENTS[event])
        elif 1 <= version <= 3:
            while position + 4 <= end:
                event = struct.unpack_from("<h", self.source, position)[0]
                self.swap_words(position, 4, 2)
                position += 4
                if event == RECORDED_ANIMATION_END or event not in EVENTS_V1:
                    break
                swap(EVENTS_V1[event])
        else:
            self.notes.append(f"a recorded animation of version {version}, which the game does not play")
            return
        if position > end or (event not in EVENTS and event not in EVENTS_V1):
            self.notes.append(f"a recorded animation's stream (version {version}) does not end as its events say")

    def bitmap_pixels(self, region: Region, group: int):
        """a bitmap group's bitmaps' pixels, in the map past the tags, in GX's formats where they
        fit (tools/wii_gx_textures.py), each texture's levels as rasterizer_swizzle.c counts them.

        The texture cache reads a bitmap's pixels at its offset plus the group's pixel data's file
        offset (xbox_texture_cache.c), but the Xbox's maps keep in that field where the pixels
        were in the bitmap's tag file: a bitmap's own offset is already in the map. It is made 0
        here. An interface bitmap is linear whatever its flags (bitmap_group.c)."""
        pixel_data = region.offset(group + self.bitmap_group["pixel_data"]) + self.tag_data["file_offset"]
        struct.pack_into(">i", self.output, pixel_data, 0)
        group_type = struct.unpack_from("<h", self.source, region.offset(group + self.bitmap_group["type"]))[0]
        count, address = struct.unpack_from("<iI", self.source, region.offset(group + self.bitmap_group["bitmaps"]))
        if count <= 0 or not region.contains(address, count * self.bitmap_data_size):
            return
        fields = self.bitmap_data
        for index in range(count):
            element = region.offset(address + index * self.bitmap_data_size)

            def value(name, code="<h"):
                return struct.unpack_from(code, self.source, element + fields[name])[0]

            width, height, depth = value("width"), value("height"), value("depth")
            bitmap_type, bitmap_format, flags = value("type"), value("format"), value("flags", "<H")
            mipmap_count, pixels_offset = value("mipmap_count"), value("pixels_offset", "<i")
            if group_type == BITMAP_GROUP_TYPE_INTERFACE:
                flags |= 1 << BITMAP_LINEAR_BIT
            kind = wii_gx_textures.BITMAP_FORMATS.get(bitmap_format)
            outcome = kind or f"format {bitmap_format}"
            linear = flags >> BITMAP_LINEAR_BIT & 1
            compressed = flags >> BITMAP_COMPRESSED_BIT & 1
            if (kind is not None and bitmap_type == BITMAP_TYPE_CUBE and not linear and width == height and width > 0 and
                    not flags >> BITMAP_V16U16_BIT & 1 and kind != "p8"):
                # (a cube map as a sphere map, which GX can sample: wii_gx_textures.py's convert_cube)
                levels = 1
                if flags >> BITMAP_POWER_OF_TWO_BIT & 1:
                    largest = max(width // 4, 1) if compressed else width
                    levels = min(mipmap_count, max(largest, 1).bit_length() - 1) + 1
                face = sum(wii_gx_textures.xbox_level_bytes(kind, width, width, level) for level in range(levels))
                face = (face + wii_gx_textures.CUBE_FACE_ALIGNMENT - 1) & ~(wii_gx_textures.CUBE_FACE_ALIGNMENT - 1)
                if pixels_offset < 0 or pixels_offset + 6 * face > len(self.source):
                    self.notes.append(f"a cube map's pixels are outside the map ({pixels_offset:x})")
                    continue
                converted = wii_gx_textures.convert_cube(self.source[pixels_offset:pixels_offset + 6 * face], kind, width,
                                                         levels)
                if converted is None:
                    outcome = "left: a cube map too small for a sphere map"
                else:
                    self.output[pixels_offset:pixels_offset + 6 * face] = converted
                    outcome = f"cube map ({kind}) as a sphere map"
            elif (kind is None or bitmap_type != BITMAP_TYPE_2D or flags >> BITMAP_V16U16_BIT & 1 or width <= 0 or
                    height <= 0 or (linear and kind.startswith("dxt"))):
                outcome = "left: " + (outcome if kind is None else "type or shape")
            else:
                levels = 1
                if flags >> BITMAP_POWER_OF_TWO_BIT & 1 and not linear:
                    largest = max(width // 4, height // 4, depth) if compressed else max(width, height, depth)
                    levels = min(mipmap_count, max(largest, 1).bit_length() - 1) + 1
                row = width * wii_gx_textures.XBOX_BYTES[kind]
                pitch = (row + 63) & ~63 if linear else 0
                size = sum(wii_gx_textures.xbox_level_bytes(kind, width, height, level, pitch) for level in range(levels))
                if linear and pitch != row:
                    # (rows the game does not draw: texture_cache_bitmap_valid)
                    outcome = "left: linear rows of no pitch"
                elif pixels_offset < 0 or pixels_offset + size > len(self.source):
                    self.notes.append(f"a bitmap's pixels are outside the map ({pixels_offset:x})")
                    continue
                else:
                    converted = wii_gx_textures.convert(self.source[pixels_offset:pixels_offset + size], kind, width,
                                                        height, levels, pitch)
                    if converted is None:
                        outcome = "left: too small or large"
                    else:
                        self.output[pixels_offset:pixels_offset + size] = converted
            self.bitmaps[outcome] = self.bitmaps.get(outcome, 0) + 1

    def swap_words(self, offset: int, size: int, width: int):
        code = {2: "H", 4: "I"}[width]
        count = size // width
        struct.pack_into(f">{count}{code}", self.output, offset, *struct.unpack_from(f"<{count}{code}", self.source, offset))

    def bsp_vertices(self, definition: Definition, field, region: Region, element: int, address: int, size: int):
        """a structure bsp material's vertices, as the tools keep them beside its buffers: its
        rendered vertices, then its lightmap's, each in the (un)compressed environment format"""
        leaves = {leaf.name.split(".", 1)[1]: leaf.offset for leaf in definition.leaves}
        compressed = field.name.startswith("compressed")
        count = struct.unpack_from("<i", self.source, region.offset(element + leaves["vertices.count"]))[0]
        lightmap = struct.unpack_from("<i", self.source, region.offset(element + leaves["lightmap_vertices.count"]))[0]
        rendered_type, lightmap_type = (1, 3) if compressed else (0, 2)
        offset = region.offset(address)
        for vertex_type, vertices in ((rendered_type, count), (lightmap_type, lightmap)):
            little, big = self.vertex_structs[vertex_type]
            for vertex in range(vertices):
                big.pack_into(self.output, offset, *little.unpack_from(self.source, offset))
                offset += little.size
        if offset != region.offset(address) + size:
            self.notes.append(f"{definition.name}.{field.name}: {size} bytes, not its {count} and {lightmap} vertices")

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

    def buffer_data(self, region: Region):
        """the vertices and indices of the buffers whose Direct3D headers are in region, by the
        type and count the tags that draw them give: vertices by their type's format, indices as
        16-bit words (a strip of count triangles has count + 2)"""
        for jobs, kind in ((self.vertex_jobs, "vertex"), (self.triangle_jobs, "triangle")):
            done = []
            for hardware, (buffer_type, count) in list(jobs.items()):
                if not region.contains(hardware, BUFFER_SIZE):
                    continue
                del jobs[hardware]
                data = struct.unpack_from("<I", self.source, region.offset(hardware) + 4)[0]
                if kind == "vertex":
                    if buffer_type not in self.vertex_structs:
                        self.notes.append(f"a vertex buffer of type {buffer_type}, which has no format here")
                        continue
                    little, big = self.vertex_structs[buffer_type]
                    size = count * little.size
                else:
                    indices = count + 2 if buffer_type == TRIANGLE_BUFFER_STRIP else count * 3
                    size = indices * 2
                if not region.contains(data, size):
                    self.notes.append(f"a {kind} buffer's data is outside {region.name}")
                    continue
                offset = region.offset(data)
                if kind == "vertex":
                    for vertex in range(count):
                        big.pack_into(self.output, offset + vertex * little.size,
                                      *little.unpack_from(self.source, offset + vertex * little.size))
                else:
                    words = struct.unpack_from(f"<{indices}H", self.source, offset)
                    struct.pack_into(f">{indices}H", self.output, offset, *words)
                start = data - region.address
                region.covered[start:start + size] = b"\x01" * size
                done.append((data, size))
            # (each buffer's data ends where the next's begins, or before: a check of the counts)
            done.sort()
            for (first, size), (second, _) in zip(done, done[1:]):
                if first + size > second:
                    self.notes.append(f"{kind} buffers overlap at {first:08x}: their counts are not what they seem")
                    break

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
