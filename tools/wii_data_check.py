#!/usr/bin/env python3
"""Checks the data blocks of converted maps against the Xbox's (port/wii/README.md, "Map conversion").

tools/wii_convert_maps.py converts the data blocks whose contents are not plain fields by what the
game reads in them. This reads each of them as the game does, once from the Xbox's map
little-endian and once from the converted map big-endian, and compares the values (floats by their
bits); the walk that finds them is the converter's own:

- script syntax (scenario hs_syntax_data; source/hs/hs_runtime.c's hs_evaluate and
  hs_compile.c's hs_compile_postprocess): the data array's header, each node's fields, and its
  32-bit value cell as it is read: a constant of a type before string (boolean, real, short,
  long) by its type's size from the cell's low-order end (port/linux/include/halo_cells.h), a call's
  or script's first argument (an expression index) whole. A primitive of string or a later type,
  and a global's name, are parsed again from the source text as the map loads, so its cell is not
  read; it is compared as the constant's type's size all the same.
- recorded animations (source/cutscene): the unit control's fields of each version
  (recorded_animation_initialize.c), then the current format's animation state and its events,
  each a header byte of two bitfields (time delta, event type: MSVC's from the low bit, the Wii's
  from the high one), a time delta of a byte or a word, and the event's data
  (recorded_animation_playback.c), or a version 1 to 3 stream's events (_v1.c), up to the end event.
- cluster visibility (structure_bsp cluster_data; structure_bsp_get_cluster_pvs): each cluster's
  bit vector, read a long at a time, as the set of clusters it sees.
- vertices: a structure bsp material's vertex data (struct environment_vertex_compressed and
  _lightmap_vertex_compressed, as the game reads them on the processor: structure_visibility.c,
  object_lights.c), and every vertex buffer by the elements its vertex shader declaration gives
  (rasterizer_xbox_vertex_shaders_initialize.c), as port/wii/src/d3d8_gx.c fetches them; each
  index buffer's 16-bit indices, which must name vertices of the part's vertex buffer.
- sounds: what the permutations' samples are (the converter leaves them as they are; the Xbox
  ADPCM decoder, port/linux/src/dsound_sdl.c's decode_adpcm, reads its block headers a byte at a
  time, but decode_pcm copies 16-bit PCM as the processor's own shorts).
- bitmaps (tools/wii_gx_textures.py): each texture port/wii/src/d3d8_gx.c binds as GX's (2D, its
  levels gx_native_levels counts), decoded from the Xbox's texels (swizzled, linear or DXT blocks)
  and from GX's tiles (CMPR, I4, RGBA8, RGB565, RGB5A3, I8, IA8, CI8) by GX's own rules, texel by
  texel; and each cube map's sphere map, a texel at a time against the cube map's texel in the
  direction d3d8_gx.c's sphere_coordinates gives it.

usage: tools/wii_data_check.py [--elf build/wii/halo.elf] [--converted DIR] map.map...
(without --converted, the maps are converted in memory)
"""

import argparse
import struct
import sys
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Tuple

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wii_convert_maps import (BITMAP_GROUP_TYPE_INTERFACE, TAG_CACHE_SIZE, XBOX_TAG_CACHE_ADDRESS,  # noqa: E402
                              Converter, Region)

# ---------- what the game reads

# hs.h's types: the constants a map's value is read for (those before _hs_type_string)
HS_TYPE_BOOLEAN, HS_TYPE_LONG, HS_TYPE_STRING = 5, 8, 9
HS_PRIMITIVE, HS_VARIABLE = 1, 4  # (_hs_syntax_node_primitive_bit, _variable_bit)

# a recorded animation's unit control, each version's fields (recorded_animation_initialize.c's
# tables): (width, count), width 1 bytes
UNIT_CONTROL = [[(1, 1), (1, 1), (2, 1), (2, 1), (2, 1), (4, 2), (4, 3), (4, 3), (4, 3)], [(4, 1)], [(2, 1)], [(2, 1)]]
# the current events' data (recorded_animation_playback.c's apply procs): signed or not, width, count
EVENT_DATA = {2: ("B", 1), 3: ("B", 1), 4: ("H", 1), 5: ("h", 1), 6: ("I", 2),
              **{event: ("b", 2) for event in range(7, 15)}, **{event: ("h", 2) for event in range(15, 23)}}
# a version 1 stream's events after their type and time delta (recorded_animation_playback_v1.c's structures)
EVENT_DATA_V1 = {0: [], 1: [], 2: ["B", "B"], 3: ["B", "B"], 4: ["H"], 5: ["h"], 6: ["I"] * 2, 7: [], 8: [],
                 **{event: ["I"] * 3 for event in range(9, 16)}, **{event: ["I"] * 2 for event in range(16, 23)}}
PLAYBACK_END = 1

# the vertex shader declarations' stream elements (rasterizer_xbox_vertex_shaders_initialize.c), by
# the vertex types (rasterizer_geometry.h) whose buffers they read; D3DVSDT types as d3d8_gx.c
# fetches them: FLOAT n, NORMPACKED3 (a long), NORMSHORT n, PBYTE n
D3DVSDT = {0x12: ("I", 1), 0x22: ("I", 2), 0x32: ("I", 3), 0x16: ("I", 1), 0x11: ("h", 1), 0x21: ("h", 2),
           0x31: ("h", 3), 0x14: ("B", 1), 0x24: ("B", 2), 0x34: ("B", 3), 0x40: ("I", 1)}
DECLARATIONS = {
    1: [0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004],  # environment (0xC4)
    3: [0x40160007, 0x40210008],  # its lightmap: 0x9C's second stream
    5: [0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40210004, 0x40240005, 0x40110006],  # model (0x78)
}
# the uncompressed types, which no declaration reads: their structures (rasterizer_geometry.c,
# rasterizer_model_types.h)
VERTEX_STRUCTURES = {0: "struct environment_vertex_uncompressed", 2: "struct environment_lightmap_vertex_uncompressed",
                     4: "struct model_vertex_uncompressed", 1: "struct environment_vertex_compressed",
                     3: "struct environment_lightmap_vertex_compressed", 5: "struct model_vertex_compressed"}
TRIANGLE_STRIP = 1

SOUND_COMPRESSIONS = {0: "none (16-bit PCM)", 1: "xbox adpcm", 2: "ima adpcm", 3: "ogg"}

# bitmaps: the formats (xbox_texture_cache.c's order), the Xbox's bytes per texel (per block for DXT)
BITMAP_KINDS = {0: "a8", 1: "l8", 2: "al8", 3: "a8l8", 6: "r5g6b5", 8: "a1r5g5b5", 9: "a4r4g4b4",
                10: "x8r8g8b8", 11: "a8r8g8b8", 14: "dxt1", 15: "dxt3", 16: "dxt5", 17: "p8"}
TEXEL_BYTES = {"a8": 1, "l8": 1, "al8": 1, "a8l8": 2, "r5g6b5": 2, "a1r5g5b5": 2, "a4r4g4b4": 2,
               "x8r8g8b8": 4, "a8r8g8b8": 4, "dxt1": 8, "dxt3": 16, "dxt5": 16, "p8": 1}
# d3d8_gx.c's gx_native_format: GX's format and tile (width, height, bytes); DXT3 and DXT5 are
# CMPR and, after its levels, I4 ("dual")
GX_FORMATS = {"a8r8g8b8": ("RGBA8", 4, 4, 64), "x8r8g8b8": ("RGBA8", 4, 4, 64), "r5g6b5": ("RGB565", 4, 4, 32),
              "a1r5g5b5": ("RGB5A3", 4, 4, 32), "a4r4g4b4": ("RGB5A3", 4, 4, 32), "l8": ("I8", 8, 4, 32),
              "a8": ("I8", 8, 4, 32), "al8": ("I8", 8, 4, 32), "a8l8": ("IA8", 4, 4, 32),
              "dxt1": ("CMPR", 8, 8, 32), "dxt3": ("CMPR", 8, 8, 64), "dxt5": ("CMPR", 8, 8, 64),
              "p8": ("CI8", 8, 4, 32)}
GX_TILES = {"RGBA8": (4, 4), "RGB565": (4, 4), "RGB5A3": (4, 4), "IA8": (4, 4), "I8": (8, 4), "CI8": (8, 4),
            "I4": (8, 8), "CMPR": (8, 8)}
LINEAR_FORMATS = {"a8", "l8", "al8", "a8l8", "r5g6b5", "a1r5g5b5", "a4r4g4b4", "x8r8g8b8", "a8r8g8b8"}
BITMAP_TYPE_2D, BITMAP_TYPE_CUBE = 0, 2
POWER_OF_TWO_BIT, COMPRESSED_BIT, LINEAR_BIT, V16U16_BIT = 0, 1, 4, 5
PITCH_ALIGNMENT, CUBE_FACE_ALIGNMENT = 64, 128
SPHERE_MAGIC, SPHERE_TRAILER = b"GXSPHERE", 32
# how far a texel may be from the Xbox's, 0-255, where GX's format holds less: (colour, alpha):
# A4R4G4B4's alpha in three bits where it is not opaque, its colour in five bits where it is (a
# clear A1R5G5B5 texel's colour is in four bits, and DXT5's alpha in I4's four: they are compared
# as such, exactly)
TOLERANCES = {"a4r4g4b4": (8, 18)}
# (a sphere map's RGB565, or RGB5A3: four bits of colour and three of alpha where it is not opaque)
SPHERE_TOLERANCE = np.array([16, 16, 16, 32])


def floor_log2(value: int) -> int:
    return max(value, 1).bit_length() - 1


class Recorder(Converter):
    """the converter, noting what its walk finds: each definition's elements, each data block,
    each vertex and index buffer, each bitmap group"""

    def convert(self, path: Path):
        self.elements: Dict[str, List[Tuple[Region, int]]] = defaultdict(list)
        self.blocks: List[tuple] = []
        self.buffer_list: List[tuple] = []
        self.bitmap_groups: List[Tuple[Region, int]] = []
        return super().convert(path)

    def walk(self, definition, region, address, count, swap):
        for element in range(count):
            self.elements[definition.name].append((region, address + element * definition.size))
        return super().walk(definition, region, address, count, swap)

    def data(self, definition, field, region, address, element):
        at = region.offset(address)
        size = struct.unpack_from("<i", self.source, at + self.tag_data["size"])[0]
        data_address = struct.unpack_from("<I", self.source, at + self.tag_data["address"])[0]
        if size > 0 and region.contains(data_address, size):
            self.blocks.append((definition.name, field.name, region, data_address, size, element))
        return super().data(definition, field, region, address, element)

    def buffer_data(self, region):
        for jobs, kind in ((self.vertex_jobs, "vertex"), (self.triangle_jobs, "triangle")):
            for hardware, (buffer_type, count) in jobs.items():
                if region.contains(hardware, 12):
                    self.buffer_list.append((kind, region, hardware, buffer_type, count))
        return super().buffer_data(region)

    def bitmap_pixels(self, region, group):
        self.bitmap_groups.append((region, group))
        return super().bitmap_pixels(region, group)


class Side:
    """one map's bytes, read as one processor does"""

    def __init__(self, data: bytes, endian: str, little_bitfields: bool):
        self.data, self.endian, self.little_bitfields = memoryview(data), endian, little_bitfields

    def get(self, code: str, offset: int):
        return struct.unpack_from(self.endian + code, self.data, offset)

    def one(self, code: str, offset: int):
        return struct.unpack_from(self.endian + code, self.data, offset)[0]


class Tally:
    def __init__(self):
        self.counts: Dict[str, List[int]] = defaultdict(lambda: [0, 0])
        self.examples: List[str] = []
        self.notes: List[str] = []

    def compare(self, kind: str, what: str, xbox, wii):
        entry = self.counts[kind]
        entry[0] += 1
        if xbox != wii:
            entry[1] += 1
            if len(self.examples) < 12:
                self.examples.append(f"{kind}: {what}: {xbox} on the Xbox, {wii} converted")

    def compare_many(self, kind: str, what: str, xbox: list, wii: list):
        if len(xbox) != len(wii):
            self.compare(kind, what + " (how many values)", len(xbox), len(wii))
            return
        for (name, a), (_, b) in zip(xbox, wii):
            self.compare(kind, f"{what} {name}", a, b)


# ---------- script syntax

def check_scripts(converter: Recorder, sides, tally: Tally):
    array = converter.executable.member_offsets("struct data_array")
    plan = converter.data_array_plan
    sizes = converter.hs_type_sizes
    for name, field, region, address, size, _ in converter.blocks:
        if (name, field) != ("scenario", "hs_syntax_data"):
            continue
        offset = region.offset(address)
        xbox = list(plan.little.unpack_from(sides[0].data, offset))
        wii = list(plan.big.unpack_from(sides[1].data, offset))
        for index in plan.pointers:
            if XBOX_TAG_CACHE_ADDRESS <= xbox[index] < XBOX_TAG_CACHE_ADDRESS + TAG_CACHE_SIZE:
                xbox[index] += converter.delta
        for index, (a, b) in enumerate(zip(xbox, wii)):
            tally.compare("scripts: data array header", f"value {index}", a, b)
        element_size = sides[0].one("h", offset + array["size"])
        maximum = sides[0].one("h", offset + array["maximum_count"])
        first = offset + plan.little.size
        inconsistent = 0
        for node in range(min(maximum, (size - plan.little.size) // max(element_size, 1))):
            values = []
            for side in sides:
                at = first + node * element_size
                datum, index, node_type, flags = side.get("hhhh", at)
                next_node, source = side.get("ii", at + 8)
                cell = side.one("I", at + 16)
                fields = [("datum header", datum), ("index", index), ("type", node_type), ("flags", flags),
                          ("next node", next_node), ("source offset", source)]
                kind = "free"
                if datum:
                    if flags & HS_PRIMITIVE:
                        width = sizes[node_type] if 0 <= node_type < len(sizes) and sizes[node_type] else 4
                        value = cell & ((1 << (8 * width)) - 1)
                        if not flags & HS_VARIABLE and HS_TYPE_BOOLEAN <= node_type < HS_TYPE_STRING:
                            kind = "constant"
                        else:
                            kind = "parsed again"
                    else:
                        value, kind = cell, "call"
                else:
                    value = None
                values.append((kind, fields, index, node_type, value))
            tally.compare_many("scripts: syntax nodes", f"node {node}", values[0][1], values[1][1])
            kind = values[0][0]
            if kind != "free":
                tally.compare(f"scripts: {kind} values", f"node {node}", values[0][4], values[1][4])
            if kind == "constant" and values[0][2] != values[0][3]:
                inconsistent += 1
        if inconsistent:
            tally.notes.append(f"scripts: {inconsistent} constants whose constant type is not their type "
                               "(hs_compile_postprocess refuses them)")


# ---------- recorded animations

def recorded_stream(side: Side, start: int, end: int, version: int, control_version: int) -> list:
    """a recorded animation's values in the order the game reads them, each (what, value)"""
    values = []
    position = start

    def take(code: str, what: str):
        nonlocal position
        size = struct.calcsize(code)
        if position + size > end:
            raise EOFError
        values.append((what, side.one(code, position)))
        position += size

    tables = UNIT_CONTROL[:min(max(control_version, 1), len(UNIT_CONTROL))]
    try:
        for table, fields in enumerate(tables):
            for width, count in fields:
                for _ in range(count):
                    take({1: "B", 2: "H", 4: "I"}[width], f"unit control {table}")
        if version == 4:
            for _ in range(6):
                take("h", "animation state")
            while True:
                if position >= end:
                    raise EOFError
                header = side.data[position]
                if side.little_bitfields:
                    time_delta, event = header & 3, header >> 2
                else:
                    time_delta, event = header >> 6, header & 0x3F
                position += 1
                values.append(("event", (time_delta, event)))
                if time_delta == 2:
                    take("B", "time delta")
                elif time_delta == 3:
                    take("H", "time delta")
                if event == PLAYBACK_END or event not in EVENT_DATA and event > 1:
                    break
                if event in EVENT_DATA:
                    code, count = EVENT_DATA[event]
                    for _ in range(count):
                        take(code, f"event {event}")
        else:
            while True:
                take("h", "event")
                take("H", "time delta")
                event = values[-2][1]
                if event == PLAYBACK_END or event not in EVENT_DATA_V1:
                    break
                for code in EVENT_DATA_V1[event]:
                    take(code, f"event {event}")
    except EOFError:
        values.append(("stream ends before its end event", position - start))
    return values


def check_recorded_animations(converter: Recorder, sides, tally: Tally):
    for name, field, region, address, size, element in converter.blocks:
        if (name, field) != ("recorded_animation", "event_stream"):
            continue
        definition = next(d for d in converter.schema.definitions.values() if d.name == name)
        leaves = {leaf.name.split(".", 1)[1]: leaf.offset for leaf in definition.leaves}
        version = sides[0].data[region.offset(element + leaves["version"])]
        control_version = sides[0].data[region.offset(element + leaves["unit_control_data_version"])]
        if not 1 <= version <= 4:
            tally.notes.append(f"recorded animations: one of version {version}, which the game does not play")
            continue
        start = region.offset(address)
        streams = [recorded_stream(side, start, start + size, version, control_version) for side in sides]
        tally.compare_many(f"recorded animations (version {version})", "stream", *streams)


# ---------- cluster visibility

def check_cluster_visibility(converter: Recorder, sides, tally: Tally):
    for name, field, region, address, size, element in converter.blocks:
        if (name, field) != ("structure_bsp", "cluster_data"):
            continue
        definition = next(d for d in converter.schema.definitions.values() if d.name == name)
        leaves = {leaf.name.split(".", 1)[1]: leaf.offset for leaf in definition.leaves}
        clusters = sides[0].one("i", region.offset(element + leaves["clusters.count"]))
        longs = (clusters + 31) // 32
        start = region.offset(address)
        for cluster in range(clusters):
            if (cluster + 1) * 4 * longs > size:
                tally.notes.append(f"cluster visibility: cluster {cluster} of {clusters} is past the data")
                break
            seen = []
            for side in sides:
                words = side.get(f"{longs}I", start + 4 * longs * cluster)
                seen.append(frozenset(other for other in range(clusters) if words[other >> 5] >> (other & 31) & 1))
            tally.compare("cluster visibility: clusters seen", f"cluster {cluster}", *seen)
            if cluster not in seen[1]:
                tally.notes.append(f"cluster visibility: cluster {cluster} does not see itself")


# ---------- vertices and indices

def structure_codes(converter: Recorder, type_name: str) -> Tuple[str, int]:
    """a structure's values as the processor reads them (from the DWARF types): its codes and size"""
    codes = []
    position = 0
    for leaf in sorted(converter.executable.struct_layout(type_name), key=lambda leaf: leaf.offset):
        assert leaf.offset == position, (type_name, leaf)
        integer = {1: "B", 2: "h", 4: "i"}.get(leaf.size)
        codes.append({"float": "I", "int": integer, "bytes": f"{leaf.size}s"}[leaf.kind])
        position += leaf.size
    size = converter.executable.size_of(converter.executable.types[type_name])
    assert position == size, type_name
    return "".join(codes), size


def declaration_codes(tokens: List[int]) -> str:
    return "".join(D3DVSDT[(token >> 16) & 0xFF][0] * D3DVSDT[(token >> 16) & 0xFF][1] for token in tokens)


def compare_vertices(tally: Tally, kind: str, sides, offset: int, code: str, size: int, count: int, what: str):
    xbox = struct.iter_unpack("<" + code, sides[0].data[offset:offset + size * count])
    wii = struct.iter_unpack(">" + code, sides[1].data[offset:offset + size * count])
    entry = tally.counts[kind]
    for index, (a, b) in enumerate(zip(xbox, wii)):
        entry[0] += len(a)
        if a != b:
            entry[1] += sum(x != y for x, y in zip(a, b))
            if len(tally.examples) < 12:
                tally.examples.append(f"{kind}: {what} vertex {index}: {a} on the Xbox, {b} converted")


def check_vertices(converter: Recorder, sides, tally: Tally):
    structures = {vertex_type: structure_codes(converter, name) for vertex_type, name in VERTEX_STRUCTURES.items()}
    for vertex_type, tokens in DECLARATIONS.items():
        code = declaration_codes(tokens)
        assert struct.calcsize("<" + code) == structures[vertex_type][1], (vertex_type, code)
    # a structure bsp material's vertex data, as the processor reads it
    for name, field, region, address, size, element in converter.blocks:
        if name != "structure_material" or not field.endswith("vertex_data"):
            continue
        definition = next(d for d in converter.schema.definitions.values() if d.name == name)
        leaves = {leaf.name.split(".", 1)[1]: leaf.offset for leaf in definition.leaves}
        vertices = sides[0].one("i", region.offset(element + leaves["vertices.count"]))
        lightmap = sides[0].one("i", region.offset(element + leaves["lightmap_vertices.count"]))
        rendered_type, lightmap_type = (1, 3) if field.startswith("compressed") else (0, 2)
        offset = region.offset(address)
        for vertex_type, count in ((rendered_type, vertices), (lightmap_type, lightmap)):
            code, vertex_size = structures[vertex_type]
            if offset + count * vertex_size > region.offset(address) + size:
                tally.notes.append(f"bsp vertices: a material's {field} is shorter than its vertices")
                break
            compare_vertices(tally, f"bsp vertex data ({field.split('_')[0]})", sides, offset, code, vertex_size,
                             count, "material")
            offset += count * vertex_size
    # the vertex buffers, as the device reads them
    buffers = {}
    for kind, region, hardware, buffer_type, count in converter.buffer_list:
        data = sides[0].one("I", region.offset(hardware) + 4)
        if kind == "vertex":
            if buffer_type in DECLARATIONS:
                code = declaration_codes(DECLARATIONS[buffer_type])
                label = f"vertex buffers (type {buffer_type}, by its declaration)"
            elif buffer_type in structures:
                code = structures[buffer_type][0]
                label = f"vertex buffers (type {buffer_type}, by its structure)"
            else:
                tally.notes.append(f"vertex buffers: one of type {buffer_type}, which has no layout here")
                continue
            vertex_size = struct.calcsize("<" + code)
            if not region.contains(data, vertex_size * count):
                continue
            compare_vertices(tally, label, sides, region.offset(data), code, vertex_size, count, "buffer")
            buffers[hardware] = count
    # the index buffers: their indices, each of which must name one of the part's vertices
    out_of_range = parts = 0
    for region, element in converter.elements.get("model_geometry_part", []):
        definition = next(d for d in converter.schema.definitions.values() if d.name == "model_geometry_part")
        triangle_field, vertex_field = sorted(converter.buffer_fields[id(definition)], key=lambda field: field[0])
        at_triangles = region.offset(element + triangle_field[1])
        at_vertices = region.offset(element + vertex_field[1])
        triangle_type, triangle_count = sides[0].get("hxxi", at_triangles)
        triangle_hardware = sides[0].one("I", at_triangles + 12)
        vertex_count = sides[0].one("i", at_vertices + 4)
        if triangle_count <= 0 or not region.contains(triangle_hardware, 12):
            continue
        data = sides[0].one("I", region.offset(triangle_hardware) + 4)
        indices = triangle_count + 2 if triangle_type == TRIANGLE_STRIP else 3 * triangle_count
        if not region.contains(data, 2 * indices):
            continue
        parts += 1
        xbox = sides[0].get(f"{indices}H", region.offset(data))
        wii = sides[1].get(f"{indices}H", region.offset(data))
        entry = tally.counts["index buffers (model parts)"]
        entry[0] += indices
        entry[1] += sum(a != b for a, b in zip(xbox, wii))
        # (a strip's restarts are 0xFFFF)
        out_of_range += sum(1 for index in wii if index >= vertex_count and index != 0xFFFF)
    if out_of_range:
        tally.notes.append(f"index buffers: {out_of_range} indices past their part's {parts} vertex buffers")
    # the structure bsps' index buffers (whose vertices are the materials')
    for kind, region, hardware, buffer_type, count in converter.buffer_list:
        if kind != "triangle":
            continue
        data = sides[0].one("I", region.offset(hardware) + 4)
        indices = count + 2 if buffer_type == TRIANGLE_STRIP else 3 * count
        if region.name == "tags" or not region.contains(data, 2 * indices):
            continue
        xbox = sides[0].get(f"{indices}H", region.offset(data))
        wii = sides[1].get(f"{indices}H", region.offset(data))
        entry = tally.counts["index buffers (structure bsps)"]
        entry[0] += indices
        entry[1] += sum(a != b for a, b in zip(xbox, wii))


# ---------- sounds

def check_sounds(converter: Recorder, sides, tally: Tally):
    definitions = {d.name: d for d in converter.schema.definitions.values()}

    def leaves(name):
        return {leaf.name.split(".", 1)[1]: leaf.offset for leaf in definitions[name].leaves}

    sound, pitch_range, permutation = leaves("sound"), leaves("sound_pitch_range"), leaves("sound_permutation")
    found: Dict[Tuple[int, int], List[int]] = defaultdict(lambda: [0, 0])
    same = differing = 0
    for region, element in converter.elements.get("sound", []):
        compression = sides[0].one("h", region.offset(element + sound["compression"]))
        ranges, ranges_address = sides[0].get("iI", region.offset(element + sound["pitch_ranges.count"]))
        for range_index in range(max(ranges, 0)):
            at = ranges_address + range_index * definitions["sound_pitch_range"].size
            count, address = sides[0].get("iI", region.offset(at + pitch_range["permutations.count"]))
            for index in range(max(count, 0)):
                element_at = region.offset(address + index * definitions["sound_permutation"].size)
                own = sides[0].one("h", element_at + permutation["compression"])
                size, _, file_offset = sides[0].get("iii", element_at + permutation["samples.size"])
                entry = found[(compression, own)]
                entry[0] += 1
                entry[1] += max(size, 0)
                if size > 0 and file_offset + size <= len(sides[0].data):
                    if sides[0].data[file_offset:file_offset + size] == sides[1].data[file_offset:file_offset + size]:
                        same += 1
                    else:
                        differing += 1
                # the fields the sound manager and the cache read, as values
                for name, code in (("compression", "h"), ("samples.size", "i"), ("samples.file_offset", "i"),
                                   ("sample_buffer_size", "I")):
                    at = element_at + permutation[name]
                    tally.compare("sounds: permutation fields", name, sides[0].one(code, at), sides[1].one(code, at))
    tally.counts["sounds: samples as they were"] = [same + differing, differing]
    for (compression, own), (count, size) in sorted(found.items()):
        line = (f"sounds: {count} permutations of sounds compressed {SOUND_COMPRESSIONS.get(compression, compression)} "
                f"(the permutations' {SOUND_COMPRESSIONS.get(own, own)}), {size} bytes of samples")
        if compression == 0 and size:
            line += ": 16-bit PCM, little-endian, which decode_pcm reads as the processor's shorts"
        tally.notes.append(line)


# ---------- bitmaps

def swizzle_index(width: int, height: int) -> np.ndarray:
    """each texel's place in a swizzled level (height, width): the bits of x and y taken in
    turns from the low bit, x first, while each has bits left"""
    xs = np.arange(width, dtype=np.int64)
    ys = np.arange(height, dtype=np.int64)
    out_x = np.zeros(width, dtype=np.int64)
    out_y = np.zeros(height, dtype=np.int64)
    out_bit = 0
    bit = 0
    while (1 << bit) < width or (1 << bit) < height:
        if (1 << bit) < width:
            out_x |= ((xs >> bit) & 1) << out_bit
            out_bit += 1
        if (1 << bit) < height:
            out_y |= ((ys >> bit) & 1) << out_bit
            out_bit += 1
        bit += 1
    return out_y[:, None] | out_x[None, :]


def expand(value: np.ndarray, bits: int) -> np.ndarray:
    """a channel of so many bits as 0-255, its high bits repeated below it"""
    value = value.astype(np.int32)
    out = value << (8 - bits)
    shift = 8 - bits
    while shift > 0:
        shift -= bits
        out |= (value << shift) if shift >= 0 else (value >> -shift)
    return out


def interpolate(c0: np.ndarray, c1: np.ndarray, four: np.ndarray) -> np.ndarray:
    """a DXT or CMPR block's four colours (..., 4, 4 RGBA) from its two RGB565 colours: two thirds
    and one third where four (c0 > c1, or always for DXT3 and DXT5), else a half and transparent"""
    def rgb(c):
        return np.stack([expand(c >> 11 & 31, 5), expand(c >> 5 & 63, 6), expand(c & 31, 5)], axis=-1)

    p0, p1 = rgb(c0.astype(np.int32)), rgb(c1.astype(np.int32))
    four = four[..., None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    alpha = np.full(c0.shape + (4,), 255, dtype=np.int32)
    alpha[..., 3] = np.where(four[..., 0], 255, 0)
    colours = np.stack([p0, p1, p2, p3], axis=-2)
    return np.concatenate([colours, alpha[..., None]], axis=-1)


def pick(palette: np.ndarray, code: np.ndarray) -> np.ndarray:
    """each block's colour (..., 4 RGBA) of its four (palette: ..., 4, 4) by a texel's code (...)"""
    return np.take_along_axis(palette, code[..., None, None].repeat(4, -1), -2)[..., 0, :]


def xbox_level(data: bytes, kind: str, width: int, height: int, pitch: int) -> np.ndarray:
    """an Xbox level's texels (height, width, 4) as R, G, B, A 0-255 (P8's index in all four)"""
    if kind.startswith("dxt"):
        columns, rows = (width + 3) // 4, (height + 3) // 4
        count = columns * rows * TEXEL_BYTES[kind]
        blocks = np.frombuffer(data, dtype=np.uint8, count=count).reshape(rows, columns, -1)
        colour = blocks[..., 8:16] if kind != "dxt1" else blocks[..., 0:8]
        c0 = colour[..., 0].astype(np.int32) | colour[..., 1].astype(np.int32) << 8
        c1 = colour[..., 2].astype(np.int32) | colour[..., 3].astype(np.int32) << 8
        palette = interpolate(c0, c1, c0 > c1 if kind == "dxt1" else np.ones(c0.shape, dtype=bool))
        image = np.zeros((rows * 4, columns * 4, 4), dtype=np.int32)
        for row in range(4):
            for column in range(4):
                code = (colour[..., 4 + row].astype(np.int32) >> (2 * column)) & 3
                image[row::4, column::4] = pick(palette, code)
        if kind == "dxt3":
            for texel in range(16):
                nibble = (blocks[..., texel // 2].astype(np.int32) >> (4 * (texel & 1))) & 15
                image[texel // 4::4, texel % 4::4, 3] = nibble * 17
        elif kind == "dxt5":
            a0, a1 = blocks[..., 0].astype(np.int32), blocks[..., 1].astype(np.int32)
            bits = np.zeros(a0.shape, dtype=np.uint64)
            for byte in range(6):
                bits |= blocks[..., 2 + byte].astype(np.uint64) << np.uint64(8 * byte)
            for texel in range(16):
                code = ((bits >> np.uint64(3 * texel)) & np.uint64(7)).astype(np.int32)
                eight = a0 > a1
                value = np.where(code == 0, a0, np.where(code == 1, a1, 0))
                value = np.where(eight & (code >= 2), ((8 - code) * a0 + (code - 1) * a1) // 7, value)
                six = ~eight & (code >= 2) & (code <= 5)
                value = np.where(six, ((6 - code) * a0 + (code - 1) * a1) // 5, value)
                value = np.where(~eight & (code == 7), 255, np.where(~eight & (code == 6), 0, value))
                image[texel // 4::4, texel % 4::4, 3] = value
        return image[:height, :width]
    size = TEXEL_BYTES[kind]
    if pitch:
        raw = np.frombuffer(data, dtype=np.uint8, count=pitch * height).reshape(height, pitch)[:, :width * size]
        texel = raw.reshape(height, width, size).astype(np.int32)
    else:
        raw = np.frombuffer(data, dtype=np.uint8, count=width * height * size).reshape(-1, size)
        texel = raw[swizzle_index(width, height)].astype(np.int32)
    out = np.zeros((height, width, 4), dtype=np.int32)
    word = texel[..., 0] | (texel[..., 1] << 8) if size >= 2 else None
    if kind in ("a8r8g8b8", "x8r8g8b8"):
        out[..., 0], out[..., 1], out[..., 2] = texel[..., 2], texel[..., 1], texel[..., 0]
        out[..., 3] = texel[..., 3] if kind == "a8r8g8b8" else 255
    elif kind == "r5g6b5":
        out[..., 0], out[..., 1], out[..., 2] = expand(word >> 11, 5), expand(word >> 5 & 63, 6), expand(word & 31, 5)
        out[..., 3] = 255
    elif kind == "a1r5g5b5":
        out[..., 0], out[..., 1] = expand(word >> 10 & 31, 5), expand(word >> 5 & 31, 5)
        out[..., 2] = expand(word & 31, 5)
        out[..., 3] = (word >> 15) * 255
    elif kind == "a4r4g4b4":
        out[..., 0], out[..., 1], out[..., 2], out[..., 3] = (word >> 8 & 15) * 17, (word >> 4 & 15) * 17, \
            (word & 15) * 17, (word >> 12) * 17
    elif kind == "a8l8":
        out[..., 0] = out[..., 1] = out[..., 2] = word & 255
        out[..., 3] = word >> 8
    else:
        # (L8, A8, AL8 and P8: the byte, which GX's I8 and CI8 hold as it is)
        out[...] = texel[..., 0:1]
    return out


def gx_untile(values: np.ndarray, tile_width: int, tile_height: int, width: int, height: int) -> np.ndarray:
    """(tiles, tile_height * tile_width, ...) in GX's order, row after row of tiles, as (height, width, ...)"""
    across = -(-width // tile_width)
    down = -(-height // tile_height)
    rest = values.shape[2:]
    image = values.reshape((down, across, tile_height, tile_width) + rest)
    image = image.transpose((0, 2, 1, 3) + tuple(range(4, 4 + len(rest))))
    return image.reshape((down * tile_height, across * tile_width) + rest)[:height, :width]


def gx_level(data: bytes, gx: str, width: int, height: int) -> np.ndarray:
    """a GX level's texels (height, width, 4) as R, G, B, A 0-255 (I8 and CI8: the byte in all four;
    IA8: the intensity in R, G and B), by GX's formats"""
    tile_width, tile_height = GX_TILES[gx]
    tiles = -(-width // tile_width) * -(-height // tile_height)
    out = None
    if gx == "RGBA8":
        raw = np.frombuffer(data, dtype=np.uint8, count=tiles * 64).reshape(tiles, 2, 16, 2).astype(np.int32)
        texel = np.stack([raw[:, 0, :, 1], raw[:, 1, :, 0], raw[:, 1, :, 1], raw[:, 0, :, 0]], axis=-1)
        out = gx_untile(texel, 4, 4, width, height)
    elif gx in ("RGB565", "RGB5A3", "IA8"):
        word = np.frombuffer(data, dtype=">u2", count=tiles * 16).reshape(tiles, 16).astype(np.int32)
        if gx == "RGB565":
            texel = np.stack([expand(word >> 11, 5), expand(word >> 5 & 63, 6), expand(word & 31, 5),
                              np.full_like(word, 255)], axis=-1)
        elif gx == "RGB5A3":
            opaque = word >> 15 == 1
            texel = np.stack([np.where(opaque, expand(word >> 10 & 31, 5), (word >> 8 & 15) * 17),
                              np.where(opaque, expand(word >> 5 & 31, 5), (word >> 4 & 15) * 17),
                              np.where(opaque, expand(word & 31, 5), (word & 15) * 17),
                              np.where(opaque, 255, expand(word >> 12 & 7, 3))], axis=-1)
        else:
            texel = np.stack([word & 255, word & 255, word & 255, word >> 8], axis=-1)
        out = gx_untile(texel, 4, 4, width, height)
    elif gx in ("I8", "CI8"):
        raw = np.frombuffer(data, dtype=np.uint8, count=tiles * 32).reshape(tiles, 32).astype(np.int32)
        out = gx_untile(np.repeat(raw[..., None], 4, axis=-1), 8, 4, width, height)
    elif gx == "I4":
        raw = np.frombuffer(data, dtype=np.uint8, count=tiles * 32).reshape(tiles, 32).astype(np.int32)
        nibbles = np.stack([raw >> 4, raw & 15], axis=-1).reshape(tiles, 64) * 17
        out = gx_untile(np.repeat(nibbles[..., None], 4, axis=-1), 8, 8, width, height)
    elif gx == "CMPR":
        raw = np.frombuffer(data, dtype=np.uint8, count=tiles * 32).reshape(tiles, 4, 8).astype(np.int32)
        c0 = raw[..., 0] << 8 | raw[..., 1]
        c1 = raw[..., 2] << 8 | raw[..., 3]
        palette = interpolate(c0, c1, c0 > c1)
        block = np.zeros((tiles, 4, 4, 4, 4), dtype=np.int32)
        for row in range(4):
            for column in range(4):
                code = (raw[..., 4 + row] >> (6 - 2 * column)) & 3
                block[:, :, row, column] = pick(palette, code)
        # (a tile's four blocks: top left, top right, bottom left, bottom right)
        texel = block.reshape(tiles, 2, 2, 4, 4, 4).transpose(0, 1, 3, 2, 4, 5).reshape(tiles, 64, 4)
        out = gx_untile(texel, 8, 8, width, height)
    return out


def gx_level_bytes(kind: str, width: int, height: int, level: int) -> int:
    _, tile_width, tile_height, tile_bytes = GX_FORMATS[kind]
    w, h = max(width >> level, 1), max(height >> level, 1)
    return -(-w // tile_width) * -(-h // tile_height) * tile_bytes


def xbox_level_bytes(kind: str, width: int, height: int, level: int, pitch: int) -> int:
    w, h = max(width >> level, 1), max(height >> level, 1)
    if kind.startswith("dxt"):
        return -(-w // 4) * -(-h // 4) * TEXEL_BYTES[kind]
    return pitch * h if pitch else w * h * TEXEL_BYTES[kind]


def sphere_coordinates(direction: np.ndarray) -> np.ndarray:
    """d3d8_gx.c's sphere_coordinates: a direction's (s, t) on a sphere map"""
    length = np.sqrt((direction ** 2).sum(axis=-1, keepdims=True))
    x, y, z = np.moveaxis(direction / length, -1, 0)
    m = 2.0 * np.sqrt(x * x + y * y + (z + 1.0) ** 2)
    return np.stack([x / m + 0.5, y / m + 0.5], axis=-1)


def cube_texels(faces: List[np.ndarray], direction: np.ndarray) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """the face, row and column a direction reads of a cube map (Direct3D's faces: +X, -X, +Y,
    -Y, +Z, -Z; the major axis's face, s and t from the other two)"""
    x, y, z = np.moveaxis(direction, -1, 0)
    ax, ay, az = np.abs(x), np.abs(y), np.abs(z)
    face = np.where((ax >= ay) & (ax >= az), np.where(x >= 0, 0, 1),
                    np.where(ay >= az, np.where(y >= 0, 2, 3), np.where(z >= 0, 4, 5)))
    major = np.choose(face, [ax, ax, ay, ay, az, az])
    s = np.choose(face, [-z, z, x, x, x, -x]) / major
    t = np.choose(face, [-y, -y, z, -z, -y, -y]) / major
    size = faces[0].shape[0]
    column = np.clip(((s + 1) / 2 * size).astype(np.int64), 0, size - 1)
    row = np.clip(((t + 1) / 2 * size).astype(np.int64), 0, size - 1)
    return face, row, column


class Bitmaps:
    def __init__(self):
        self.formats: Dict[str, List[int]] = defaultdict(lambda: [0, 0, 0, 0])  # bitmaps, texels, differing, largest

    def compare(self, kind: str, xbox: np.ndarray, wii: np.ndarray, tally: Tally, what: str):
        colour_tolerance, alpha_tolerance = TOLERANCES.get(kind, (0, 0))
        if kind == "a1r5g5b5":
            # (a clear texel's colour is in four bits: a five-bit channel's high four)
            xbox = xbox.copy()
            xbox[..., 0:3] = np.where(xbox[..., 3:4] == 0, (xbox[..., 0:3] >> 4) * 17, xbox[..., 0:3])
        difference = np.abs(xbox - wii)
        over = (difference[..., 0:3] > colour_tolerance).any(axis=-1) | (difference[..., 3] > alpha_tolerance)
        entry = self.formats[kind]
        entry[1] += over.size
        entry[2] += int(over.sum())
        entry[3] = max(entry[3], int(difference.max()) if difference.size else 0)
        if over.any() and len(tally.examples) < 12:
            row, column = np.argwhere(over)[0]
            tally.examples.append(f"bitmaps: {what}: texel ({column}, {row}) {tuple(xbox[row, column])} on the Xbox, "
                                  f"{tuple(wii[row, column])} converted")


def check_bitmaps(converter: Recorder, sides, tally: Tally):
    fields = converter.bitmap_data
    group_fields = converter.bitmap_group
    result = Bitmaps()
    spheres = [0, 0, 0, 0]  # sphere maps, texels, differing, coordinates off
    left = defaultdict(int)
    for region, group in converter.bitmap_groups:
        group_type = sides[0].one("h", region.offset(group + group_fields["type"]))
        count, address = sides[0].get("iI", region.offset(group + group_fields["bitmaps"]))
        for index in range(max(count, 0)):
            element = region.offset(address + index * converter.bitmap_data_size)

            def value(name, code="h"):
                return sides[0].one(code, element + fields[name])

            width, height, depth = value("width"), value("height"), value("depth")
            bitmap_type, bitmap_format, flags = value("type"), value("format"), value("flags", "H")
            mipmap_count, offset = value("mipmap_count"), value("pixels_offset", "i")
            kind = BITMAP_KINDS.get(bitmap_format)
            if group_type == BITMAP_GROUP_TYPE_INTERFACE:
                flags |= 1 << LINEAR_BIT
            linear = flags >> LINEAR_BIT & 1
            if kind is None or width <= 0 or height <= 0 or depth <= 0 or bitmap_type > BITMAP_TYPE_CUBE:
                left["no format or shape the device converts"] += 1
                continue
            # (the texture cache's hardware format: rasterizer_xbox_bitmap_get_max_mipmap_count)
            levels = 1
            if flags >> POWER_OF_TWO_BIT & 1 and not linear:
                compressed = flags >> COMPRESSED_BIT & 1
                largest = max(width // 4, height // 4, depth) if compressed else max(width, height, depth)
                levels = min(mipmap_count, floor_log2(largest)) + 1
            pitch = 0
            if linear:
                if kind not in LINEAR_FORMATS:
                    left["linear, of no linear format"] += 1
                    continue
                pitch = -(-width * TEXEL_BYTES[kind] // PITCH_ALIGNMENT) * PITCH_ALIGNMENT
                if pitch != width * TEXEL_BYTES[kind]:
                    left["linear, rows the game does not draw"] += 1
                    continue
            v16u16 = flags >> V16U16_BIT & 1 and kind in ("a8r8g8b8", "x8r8g8b8")
            if bitmap_type == BITMAP_TYPE_CUBE:
                check_sphere(sides, kind, width, levels, offset, spheres, tally, left)
                continue
            if bitmap_type != BITMAP_TYPE_2D or depth > 1 or v16u16:
                left["3D or V16U16: decoded by the device"] += 1
                continue
            # (d3d8_gx.c's gx_native_levels)
            budget = sum(xbox_level_bytes(kind, width, height, level, pitch) for level in range(levels))
            kept = used = 0
            if width <= 1024 and height <= 1024:
                for level in range(levels):
                    used += gx_level_bytes(kind, width, height, level)
                    if used > budget:
                        break
                    kept = level + 1
            if not kept or offset < 0 or offset + budget > len(sides[0].data):
                left["no level GX's tiles fit"] += 1
                continue
            result.formats[kind][0] += 1
            xbox_offset = gx_offset = offset
            gx_bytes = sum(gx_level_bytes(kind, width, height, level) for level in range(kept))
            for level in range(kept):
                w, h = max(width >> level, 1), max(height >> level, 1)
                xbox = xbox_level(sides[0].data[xbox_offset:], kind, w, h, pitch)
                gx_format = GX_FORMATS[kind][0]
                wii = gx_level(sides[1].data[gx_offset:], gx_format, w, h)
                if kind in ("dxt3", "dxt5"):
                    # (the alpha: an I4 texture after the CMPR's levels, as many bytes: half of them)
                    alpha = gx_level(sides[1].data[gx_offset + gx_bytes // 2:], "I4", w, h)
                    wii = wii.copy()
                    wii[..., 3] = alpha[..., 0]
                    if kind == "dxt5":
                        xbox = xbox.copy()
                        xbox[..., 3] = (xbox[..., 3] >> 4) * 17
                result.compare(kind, xbox, wii, tally, f"{kind} {width}x{height} level {level}")
                xbox_offset += xbox_level_bytes(kind, width, height, level, pitch)
                gx_offset += gx_level_bytes(kind, width, height, level) // (2 if kind in ("dxt3", "dxt5") else 1)
    for kind, (bitmaps, texels, differing, largest) in sorted(result.formats.items()):
        gx_format = GX_FORMATS[kind][0] + ("+I4" if kind in ("dxt3", "dxt5") else "")
        tally.counts[f"bitmaps: {kind} as {gx_format} (texels, {bitmaps} bitmaps, largest difference {largest})"] = \
            [texels, differing]
    if spheres[0]:
        tally.counts[f"bitmaps: cube maps as sphere maps (texels, {spheres[0]} maps; "
                     f"{spheres[3]} where sphere_coordinates is off)"] = [spheres[1], spheres[2]]
    for reason, count in sorted(left.items()):
        tally.notes.append(f"bitmaps: {count} left for the device: {reason}")


def check_sphere(sides, kind: str, size: int, levels: int, offset: int, spheres: list, tally: Tally, left):
    """a cube map's sphere map: its texels inside the disc, each against the cube map's texel in the
    direction it shows, and that direction's coordinates by sphere_coordinates against the texel"""
    face_bytes = sum(xbox_level_bytes(kind, size, size, level, 0) for level in range(levels))
    face_bytes = -(-face_bytes // CUBE_FACE_ALIGNMENT) * CUBE_FACE_ALIGNMENT
    total = 6 * face_bytes
    trailer = bytes(sides[1].data[offset + total - SPHERE_TRAILER:offset + total])
    if not trailer.startswith(SPHERE_MAGIC):
        left["cube maps left as they are"] += 1
        return
    sphere, gx_format = struct.unpack_from(">HB", trailer, 8)
    gx = {4: "RGB565", 5: "RGB5A3"}.get(gx_format)
    if gx is None:
        tally.notes.append(f"bitmaps: a sphere map of GX format {gx_format}")
        return
    faces = [xbox_level(sides[0].data[offset + face * face_bytes:], kind, size, size, 0) for face in range(6)]
    for face in faces:
        # (the texel as Direct3D reads it: L8 opaque, A8 white)
        if kind in ("x8r8g8b8", "l8"):
            face[..., 3] = 255
        elif kind == "a8":
            face[..., 0:3] = 255
    image = gx_level(sides[1].data[offset:], gx, sphere, sphere)
    # (each texel's centre on the disc, the mirror ball's normal there and the view's reflection)
    centres = (np.arange(sphere) + 0.5) / sphere
    s, t = np.meshgrid(centres, centres)
    x, y = 2 * s - 1, 2 * t - 1
    inside = x * x + y * y < 0.81
    z = np.sqrt(np.maximum(0.0, 1 - x * x - y * y))
    direction = np.stack([2 * z * x, 2 * z * y, 2 * z * z - 1], axis=-1)[inside]
    back = sphere_coordinates(direction)
    off = (np.abs(back[:, 0] - s[inside]) > 0.5 / sphere) | (np.abs(back[:, 1] - t[inside]) > 0.5 / sphere)
    face, row, column = cube_texels(faces, direction)
    stacked = np.stack(faces)
    # (the nearest texel, or one beside it: a direction on a texel's edge rounds either way)
    best = np.full(direction.shape[0], 1 << 30)
    for d_row in (-1, 0, 1):
        for d_column in (-1, 0, 1):
            texel = stacked[face, np.clip(row + d_row, 0, size - 1), np.clip(column + d_column, 0, size - 1)]
            best = np.minimum(best, (np.abs(texel - image[inside]) - SPHERE_TOLERANCE).max(axis=-1))
    differing = best > 0
    spheres[0] += 1
    spheres[1] += int(inside.sum())
    spheres[2] += int(differing.sum())
    spheres[3] += int(off.sum())
    if differing.any() and len(tally.examples) < 12:
        tally.examples.append(f"bitmaps: a {kind} cube map's sphere map: {int(differing.sum())} of {int(inside.sum())} "
                              f"texels unlike the cube's (by up to {int(best.max())} past the tolerance)")


# ---------- the maps

CHECKS = [check_scripts, check_recorded_animations, check_cluster_visibility, check_vertices, check_sounds,
          check_bitmaps]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("maps", nargs="+", type=Path)
    parser.add_argument("--elf", type=Path, default=Path("build/wii/halo.elf"))
    parser.add_argument("--converted", type=Path, help="the folder of the converted maps")
    parser.add_argument("--tag-cache-address", type=lambda text: int(text, 0), default=0x90100000)
    parser.add_argument("--only", action="append", help="the checks to run (scripts, recorded, cluster, "
                        "vertices, sounds, bitmaps)")
    args = parser.parse_args()
    converter = Recorder(args.elf, args.tag_cache_address)
    failed = False
    for path in args.maps:
        output, _ = converter.convert(path)
        if args.converted:
            output = (args.converted / path.name).read_bytes()
        sides = (Side(converter.source, "<", True), Side(output, ">", False))
        tally = Tally()
        for check in CHECKS:
            if args.only and not any(name in check.__name__ for name in args.only):
                continue
            check(converter, sides, tally)
        print(f"{path.name}:")
        for kind, (values, differing) in tally.counts.items():
            print(f"  {kind}: {values} values, {differing} differ")
            failed |= differing > 0
        for note in tally.notes:
            print(f"  note: {note}")
        for example in tally.examples:
            print(f"  differs: {example}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
