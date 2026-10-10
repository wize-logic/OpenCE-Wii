#!/usr/bin/env python3
"""The tag schema and the tags' structure layouts, read from the Wii build's executable.

The map converter (tools/wii_convert_maps.py) turns an Xbox map's little-endian tags into the
Wii's big-endian ones. What it needs to know comes from build/wii/halo.elf alone, so nothing is
written out by hand:

- the tag schema (port/linux/game/tag_schema*.c): every tag group's definition, and for each
  definition its blocks, data, references and the rest, as the linked tables hold them;
- each definition's C type, with every field down to its scalars, from the DWARF debug
  information: where each 2-, 4- and 8-byte value is (byteswapped), where the pointers are
  (byteswapped and moved to the Wii's tag cache) and where the bytes are (left as they are).

The Wii's and the Xbox's layouts are the same (port/wii/README.md, "Map conversion"); the
executable is big-endian, which this reads as such.

usage: tools/wii_tag_layout.py [--elf build/wii/halo.elf] [--dump DEFINITION]
       prints the schema's groups and definitions and checks every definition's type resolves
"""

import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from elftools.elf.elffile import ELFFile

SCHEMA_FIELD_TYPES = ["block", "data", "file_data", "reference", "tag_index", "block_index", "enum", "string",
                      "reset", "struct", "check", "terminator"]


@dataclass
class Leaf:
    """one scalar of a structure: at offset, of size bytes; kind is "int", "float", "pointer" or
    "bytes" (characters and byte arrays, which keep their order)"""
    offset: int
    size: int
    kind: str
    name: str


@dataclass
class SchemaField:
    type: str
    size: int
    count: int
    flags: int
    offset: int
    maximum: int
    target_level: int
    target_offset: int
    definition: Optional["Definition"]
    groups: List[int]
    name: str


@dataclass
class Definition:
    name: str
    size: int
    type_name: str
    fields: List[SchemaField] = field(default_factory=list)
    leaves: List[Leaf] = field(default_factory=list)


@dataclass
class Group:
    tag: int
    parents: Tuple[int, int]
    # (None for a group whose tags are all their parents': the schema checks only those)
    definition: Optional[Definition]


def fourcc(value: int) -> str:
    return value.to_bytes(4, "big").decode("latin-1") if value else "----"


def _swap_shape(leaves: List[Leaf]):
    """what a byteswap does to leaves: each swapped value's offset and size (bytes, which keep
    their order, are left out, however they are grouped)"""
    return tuple((leaf.offset, leaf.size) for leaf in leaves if leaf.kind != "bytes")


class Executable:
    """the executable's memory (its loaded segments), symbols and DWARF types"""

    def __init__(self, path: Path):
        self.file = open(path, "rb")
        self.elf = ELFFile(self.file)
        self.segments = [(segment["p_vaddr"], segment["p_filesz"], segment.data())
                         for segment in self.elf.iter_segments() if segment["p_type"] == "PT_LOAD"]
        self.symbols: Dict[str, int] = {}
        self.symbol_sizes: Dict[str, int] = {}
        for symbol in self.elf.get_section_by_name(".symtab").iter_symbols():
            if symbol.name:
                self.symbols.setdefault(symbol.name, symbol["st_value"])
                self.symbol_sizes.setdefault(symbol.name, symbol["st_size"])
        self.dwarf = self.elf.get_dwarf_info()
        self.types: Dict[str, object] = {}
        # the game names different structures alike in different files (MSVC's file-scope
        # structure tags, which the port keeps: tools/linux_msvc_semantics.py); the schema's types
        # are those its own units (tag_schema*.c) see
        self.schema_types: Dict[str, object] = {}
        # the game's own structures (outside the schema's units), every one of a name (the game
        # declares some differently in different files, a placeholder in one, every field in
        # another): what fills the schema's placeholders (Schema.fill_placeholders)
        self.game_structs: Dict[str, List[object]] = {}
        self._index_types()

    def read(self, address: int, size: int) -> bytes:
        for base, length, data in self.segments:
            if base <= address and address + size <= base + length:
                return data[address - base:address - base + size]
        raise ValueError(f"0x{address:08x} is not in the executable's data")

    def u32(self, address: int) -> int:
        return struct.unpack(">I", self.read(address, 4))[0]

    def s32(self, address: int) -> int:
        return struct.unpack(">i", self.read(address, 4))[0]

    def s16(self, address: int) -> int:
        return struct.unpack(">h", self.read(address, 2))[0]

    def string(self, address: int) -> str:
        if not address:
            return ""
        out = bytearray()
        while True:
            byte = self.read(address + len(out), 1)[0]
            if byte == 0:
                return out.decode("latin-1")
            out.append(byte)

    def _index_types(self):
        """complete struct, union and typedef DIEs by the names C spells them with"""
        for unit in self.dwarf.iter_CUs():
            schema_unit = "tag_schema" in unit.get_top_DIE().get_full_path()
            for die in unit.iter_DIEs():
                if die.tag not in ("DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_typedef",
                                   "DW_TAG_base_type", "DW_TAG_enumeration_type"):
                    continue
                if "DW_AT_name" not in die.attributes or "DW_AT_declaration" in die.attributes:
                    continue
                name = die.attributes["DW_AT_name"].value.decode()
                key = {"DW_TAG_structure_type": "struct ", "DW_TAG_union_type": "union ",
                       "DW_TAG_enumeration_type": "enum "}.get(die.tag, "") + name
                self.types.setdefault(key, die)
                if schema_unit:
                    self.schema_types.setdefault(key, die)
                elif die.tag == "DW_TAG_structure_type" and "DW_AT_byte_size" in die.attributes:
                    self.game_structs.setdefault(key, []).append(die)

    def struct_layout(self, type_name: str) -> List[Leaf]:
        die = self.schema_types.get(type_name.strip()) or self.types.get(type_name.strip())
        if die is None:
            raise KeyError(f"no type {type_name!r} in the debug information")
        leaves: List[Leaf] = []
        self._flatten(die, 0, type_name, leaves)
        return leaves

    def _flatten(self, die, base: int, path: str, leaves: List[Leaf]):
        tag = die.tag
        if tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
            if "DW_AT_type" not in die.attributes:
                return
            self._flatten(die.get_DIE_from_attribute("DW_AT_type"), base, path, leaves)
        elif tag == "DW_TAG_structure_type":
            for member in die.iter_children():
                if member.tag != "DW_TAG_member":
                    continue
                if "DW_AT_bit_size" in member.attributes:
                    raise ValueError(f"{path}: a bitfield, whose bits a byteswap would not keep")
                offset = member.attributes["DW_AT_data_member_location"].value \
                    if "DW_AT_data_member_location" in member.attributes else 0
                name = member.attributes["DW_AT_name"].value.decode() if "DW_AT_name" in member.attributes else "?"
                self._flatten(member.get_DIE_from_attribute("DW_AT_type"), base + offset, f"{path}.{name}", leaves)
        elif tag == "DW_TAG_union_type":
            # every member must swap alike, or the converter cannot know which one is meant; a
            # member named unused, pad or reserved beside others is not (hud_color's custom:
            # an unused long over the two shorts the game reads)
            layouts = []
            for member in die.iter_children():
                if member.tag != "DW_TAG_member":
                    continue
                name = member.attributes["DW_AT_name"].value.decode() if "DW_AT_name" in member.attributes else ""
                member_leaves: List[Leaf] = []
                self._flatten(member.get_DIE_from_attribute("DW_AT_type"), base, f"{path}.{name}", member_leaves)
                layouts.append((name, member_leaves))
            used = [layout for name, layout in layouts if not name.startswith(("unused", "pad", "reserved"))]
            layouts = used or [layout for _, layout in layouts]
            shapes = {_swap_shape(layout) for layout in layouts}
            if len(shapes) > 1:
                raise ValueError(f"{path}: a union whose members swap differently")
            leaves.extend(layouts[0] if layouts else [])
        elif tag == "DW_TAG_array_type":
            element = die.get_DIE_from_attribute("DW_AT_type")
            element_size = self.size_of(element)
            counts = [child.attributes["DW_AT_count"].value if "DW_AT_count" in child.attributes
                      else child.attributes["DW_AT_upper_bound"].value + 1
                      for child in die.iter_children() if child.tag == "DW_TAG_subrange_type"]
            total = 1
            for count in counts:
                total *= count
            element_leaves: List[Leaf] = []
            self._flatten(element, 0, path + "[]", element_leaves)
            if all(leaf.kind == "bytes" for leaf in element_leaves):
                if total * element_size:
                    leaves.append(Leaf(base, total * element_size, "bytes", path))
                return
            for index in range(total):
                for leaf in element_leaves:
                    leaves.append(Leaf(base + index * element_size + leaf.offset, leaf.size, leaf.kind,
                                       f"{path}[{index}]"))
        elif tag == "DW_TAG_pointer_type":
            leaves.append(Leaf(base, 4, "pointer", path))
        elif tag == "DW_TAG_enumeration_type":
            leaves.append(Leaf(base, die.attributes["DW_AT_byte_size"].value, "int", path))
        elif tag == "DW_TAG_base_type":
            size = die.attributes["DW_AT_byte_size"].value
            encoding = die.attributes["DW_AT_encoding"].value
            # DW_ATE_float 4; the characters (signed_char 6, unsigned_char 8) and one-byte
            # integers keep their order
            if size == 1:
                leaves.append(Leaf(base, 1, "bytes", path))
            else:
                leaves.append(Leaf(base, size, "float" if encoding == 4 else "int", path))
        else:
            raise ValueError(f"{path}: cannot lay out a {tag}")

    def size_of(self, die) -> int:
        while "DW_AT_byte_size" not in die.attributes:
            if die.tag == "DW_TAG_array_type":
                element = die.get_DIE_from_attribute("DW_AT_type")
                total = 1
                for child in die.iter_children():
                    if child.tag == "DW_TAG_subrange_type":
                        total *= child.attributes["DW_AT_count"].value if "DW_AT_count" in child.attributes \
                            else child.attributes["DW_AT_upper_bound"].value + 1
                return total * self.size_of(element)
            if die.tag == "DW_TAG_pointer_type":
                return 4
            die = die.get_DIE_from_attribute("DW_AT_type")
        return die.attributes["DW_AT_byte_size"].value

    def member_offsets(self, type_name: str) -> Dict[str, int]:
        die = self.types[type_name]
        return {member.attributes["DW_AT_name"].value.decode(): member.attributes["DW_AT_data_member_location"].value
                for member in die.iter_children()
                if member.tag == "DW_TAG_member" and "DW_AT_name" in member.attributes}


class Schema:
    """the groups and definitions the linked schema tables hold"""

    def __init__(self, executable: Executable):
        self.executable = executable
        self.field_offsets = executable.member_offsets("struct tag_schema_field")
        self.field_size = executable.size_of(executable.types["struct tag_schema_field"])
        self.definition_offsets = executable.member_offsets("struct tag_schema_definition")
        self.group_offsets = executable.member_offsets("struct tag_schema_group")
        self.group_size = executable.size_of(executable.types["struct tag_schema_group"])
        self.definitions: Dict[int, Definition] = {}
        self.groups: Dict[int, Group] = {}
        lists = executable.symbols["tag_schema_group_lists"]
        index = 0
        while True:
            group_list = executable.u32(lists + 4 * index)
            if not group_list:
                break
            entry = group_list
            while True:
                tag = executable.u32(entry + self.group_offsets["group_tag"])
                if not tag:
                    break
                parents = (executable.u32(entry + self.group_offsets["parent_group_tags"]),
                           executable.u32(entry + self.group_offsets["parent_group_tags"] + 4))
                address = executable.u32(entry + self.group_offsets["definition"])
                definition = self.definition(address) if address else None
                self.groups[tag] = Group(tag, parents, definition)
                entry += self.group_size
            index += 1

    def definition(self, address: int) -> Definition:
        if address in self.definitions:
            return self.definitions[address]
        executable = self.executable
        offsets = self.definition_offsets
        result = Definition(executable.string(executable.u32(address + offsets["name"])),
                            executable.s32(address + offsets["size"]),
                            executable.string(executable.u32(address + offsets["type_name"])))
        self.definitions[address] = result
        fields = executable.u32(address + offsets["fields"])
        while fields:
            o = self.field_offsets
            kind = SCHEMA_FIELD_TYPES[executable.s16(fields + o["type"])]
            if kind == "terminator":
                break
            target = executable.u32(fields + o["definition"])
            definition = self.definition(target) if kind in ("block", "struct") and target else None
            groups: List[int] = []
            if kind in ("reference", "tag_index") and target:
                while executable.u32(target + 4 * len(groups)):
                    groups.append(executable.u32(target + 4 * len(groups)))
            result.fields.append(SchemaField(kind, executable.s16(fields + o["size"]),
                                             executable.s16(fields + o["count"]), executable.s16(fields + o["flags"]),
                                             executable.s32(fields + o["offset"]), executable.s32(fields + o["maximum"]),
                                             executable.s32(fields + o["target_level"]),
                                             executable.s32(fields + o["target_offset"]), definition, groups,
                                             executable.string(executable.u32(fields + o["name"]))))
            fields += self.field_size
        return result

    PLACEHOLDER_PREFIXES = ("reserved", "unused", "unknown", "pad")
    # structures a tag holds that the game declares as bytes and reads as another structure: a
    # contrail's shader (struct contrail_shader) is an effect shader (render_contrails casts it)
    EMBEDDED = {"contrail": [("shader.", "shader_effect_definition")]}

    def _game_structures(self, definition: Definition) -> List[Tuple[str, List[Leaf]]]:
        """the game's structures named for a definition (sky: struct sky, struct sky_definition),
        each declaration of each (some cover only the first part of the tag, the fields their file
        reads), as leaves"""
        base = definition.name[:-len("_definition")] if definition.name.endswith("_definition") else definition.name
        # (the game's names for a tag's structures: its own, ..._definition, a scenario block's
        # scenario_..., a widget's ..._reference)
        names = {base, base + "_definition", "_" + base + "_definition", "scenario_" + base, base + "_reference",
                 "scenario_" + base + "_view", definition.type_name.split()[-1]}
        result = []
        for name in sorted(names):
            seen = set()
            for die in self.executable.game_structs.get(f"struct {name}", []):
                if die.attributes["DW_AT_byte_size"].value > definition.size:
                    continue
                leaves: List[Leaf] = []
                try:
                    self.executable._flatten(die, 0, f"struct {name}", leaves)
                except (ValueError, KeyError):
                    continue
                shape = tuple((leaf.offset, leaf.size, leaf.kind) for leaf in leaves)
                if shape not in seen:
                    seen.add(shape)
                    result.append((f"struct {name}", leaves))
        for prefix, name in self.EMBEDDED.get(definition.name, []):
            inside = [leaf.offset for leaf in definition.leaves if leaf.name.split(".", 1)[-1].startswith(prefix)]
            for die in self.executable.game_structs.get(f"struct {name}", [])[:1]:
                if not inside or min(inside) + die.attributes["DW_AT_byte_size"].value > definition.size:
                    continue
                leaves = []
                try:
                    self.executable._flatten(die, min(inside), f"struct {name}", leaves)
                except (ValueError, KeyError):
                    continue
                result.append((f"struct {name} (its {prefix[:-1]})", leaves))
        return result

    def fill_placeholders(self) -> Tuple[List[str], List[str]]:
        """the schema's structures (tag_schema*.c) name only the fields the validator checks, and
        stand byte arrays (reserved..., unused..., unknown..., pad...) for the rest, which a
        byteswap would leave as they are though the game reads values there (a sky's fog
        colours). They are filled from the game's own declarations of the definition's name (struct
        sky, or struct sky_definition), which may cover only the tag's first part: one agrees when
        none of its fields contradicts one the schema names (where it has bytes, it says nothing)
        and at least one is the same, or, for a view that declares bytes up to the fields its file
        reads (actions.c's vehicle_definition), when all its fields lie within the placeholders.
        The fields within the placeholders of every declaration that agrees are taken, unless two
        contradict each other. Returns what was filled, and the placeholders left as they are and
        why"""
        filled, unfilled = [], []
        for definition in self.definitions.values():
            placeholders = [leaf for leaf in definition.leaves if leaf.kind == "bytes" and leaf.size >= 4 and
                            leaf.name.split(".")[-1].startswith(self.PLACEHOLDER_PREFIXES)]
            if not placeholders:
                continue
            anchors = {(leaf.offset, leaf.size, leaf.kind == "pointer") for leaf in definition.leaves
                       if leaf.kind != "bytes"}
            ranges = [(leaf.offset, leaf.offset + leaf.size) for leaf in placeholders]
            taken: Dict[int, Tuple[int, str, str]] = {}
            names = set()
            conflict = None
            for name, leaves in self._game_structures(definition):
                fields = {(leaf.offset, leaf.size, leaf.kind == "pointer") for leaf in leaves if leaf.kind != "bytes"}
                # (some field the same, or, where the schema names none, a declaration of the
                # tag's whole size, or a view of a later part: one that starts with bytes, over
                # what its file does not read, and whose fields all lie within the placeholders, as
                # actions.c's vehicle_definition, bytes up to 0x390 and the two floats it reads)
                whole = max((leaf.offset + leaf.size for leaf in leaves), default=0) == definition.size
                first = min(leaves, key=lambda leaf: leaf.offset, default=None)
                tail = bool(fields) and first.offset == 0 and first.kind == "bytes" and \
                    all(any(start <= offset and offset + size <= end for start, end in ranges)
                        for offset, size, _ in fields)
                if not (fields & anchors or (not anchors and whole) or tail):
                    continue
                # (a field of its outside the placeholders that is not the schema's contradicts it)
                if any(not any(start <= offset and offset + size <= end for start, end in ranges) and
                       (offset, size, pointer) not in anchors for offset, size, pointer in fields):
                    continue
                for leaf in leaves:
                    if leaf.kind == "bytes" or not any(start <= leaf.offset and leaf.offset + leaf.size <= end
                                                       for start, end in ranges):
                        continue
                    earlier = taken.get(leaf.offset)
                    if earlier and earlier[:2] != (leaf.size, leaf.kind):
                        conflict = f"{name} and {earlier[2]} differ at {leaf.offset:#x}"
                    overlapping = [offset for offset, (size, _, _) in taken.items()
                                   if offset != leaf.offset and offset < leaf.offset + leaf.size and leaf.offset < offset + size]
                    if overlapping:
                        conflict = f"{name}'s fields overlap another declaration's at {leaf.offset:#x}"
                    taken.setdefault(leaf.offset, (leaf.size, leaf.kind, name))
                names.add(name)
            if conflict:
                unfilled.append(f"{definition.name}: {conflict}")
                continue
            if not taken:
                unfilled.append(f"{definition.name}: no game structure of its name says more")
                continue
            definition.leaves = [leaf for leaf in definition.leaves if leaf not in placeholders] + \
                [Leaf(offset, size, kind, f"{name}+{offset:#x}") for offset, (size, kind, name) in sorted(taken.items())]
            filled.append(f"{definition.name}: {sum(size for size, _, _ in taken.values())} of "
                          f"{sum(leaf.size for leaf in placeholders)} placeholder bytes from {', '.join(sorted(names))}")
        return filled, unfilled

    def lay_out(self) -> List[str]:
        """every definition's leaves; the problems found"""
        problems = []
        for definition in self.definitions.values():
            if not definition.type_name:
                problems.append(f"{definition.name}: no type name")
                continue
            try:
                leaves = self.executable.struct_layout(definition.type_name)
            except (KeyError, ValueError) as error:
                problems.append(f"{definition.name} ({definition.type_name}): {error}")
                continue
            # a definition may cover the first part of its type (control's, up to reserved88);
            # the bytes no field covers are the compiler's padding, left as they are
            definition.leaves = [leaf for leaf in leaves if leaf.offset < definition.size]
            end = 0
            for leaf in sorted(definition.leaves, key=lambda leaf: leaf.offset):
                if leaf.offset < end:
                    problems.append(f"{definition.name} ({definition.type_name}): {leaf.name} overlaps "
                                    f"the field before it")
                    break
                if leaf.offset + leaf.size > definition.size:
                    problems.append(f"{definition.name} ({definition.type_name}): {leaf.name} runs past "
                                    f"its {definition.size} bytes")
                    break
                end = leaf.offset + leaf.size
        return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--elf", type=Path, default=Path("build/wii/halo.elf"))
    parser.add_argument("--dump", help="print one definition's fields and leaves")
    args = parser.parse_args()
    executable = Executable(args.elf)
    schema = Schema(executable)
    problems = schema.lay_out()
    print(f"{len(schema.groups)} groups, {len(schema.definitions)} definitions, "
          f"{sum(len(d.leaves) for d in schema.definitions.values())} leaves")
    for problem in problems:
        print(f"  {problem}")
    if args.dump:
        for definition in schema.definitions.values():
            if definition.name == args.dump:
                print(f"{definition.name}: {definition.type_name}, {definition.size} bytes")
                for f in definition.fields:
                    print(f"  field {f.type:12} {f.name:32} @{f.offset} x{f.count} size {f.size}")
                for leaf in definition.leaves:
                    print(f"  leaf  {leaf.kind:8} @{leaf.offset:5} {leaf.size} {leaf.name}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
