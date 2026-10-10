#!/usr/bin/env python3
"""Checks that the game reads none of the tag bytes the map converter leaves as they are.

tools/wii_convert_maps.py byteswaps every field the tag layouts name and leaves the rest as they
are: the schema's placeholders (reserved..., unused..., unknown..., pad...) where no game
declaration fills them (wii_tag_layout.py's fill_placeholders), and the gaps between fields. A
value the game reads there stays little-endian on the Wii. Twice one was: a vehicle's AI charge
values at 0x390, which only actions.c's view of the tag names, and its slide's acceleration at
0x338, which vehicles.c reads through a cast of the address of the float before it to struct
physics_variable_speed_parameters.

This converts the maps in memory and takes, for each placeholder and gap, the bytes that hold data
in any element of any map. Then it looks for what reads those bytes, in the Wii build itself:

- the game's views of the tag (the DWARF types of build/wii/halo.elf): every structure of one of
  the tag's names (fill_placeholders' names, Schema.EMBEDDED) or bound to the tag by the code
  (below), in every unit that declares it, whose fields do not contradict the schema's; a field it
  declares over bytes that hold data is a read;
- the code (clang's AST of each of the build's units, compiled with the build's own flags,
  `ninja -t commands`): every value read through a pointer into a tag, at an offset known from
  the unit's own declarations: a field (definition->unknown330), the address of a field cast to a
  pointer to something wider ((struct physics_variable_speed_parameters *)&definition->unknown330
  reads 16 bytes), and offsets added to the tag's pointer (*(unsigned long *)((byte *)unit_definition
  + 0x17c)). The tag's pointer is one of a type that names it (above), what tag_get returns for a
  group (and the group's children), or an element of a tag block (TAG_BLOCK_GET_ELEMENT, a block's
  address cast), whose type is then bound to the block's definition.

A read is reported with the width of each value it reads over the bytes (a byte read reads the
same byte either way, so only reads of two bytes or more count). What it cannot see: a tag's
pointer kept as void * or byte * and read later, and an index that is not a constant.

usage: tools/wii_placeholder_check.py [--elf build/wii/halo.elf] [--jobs N] [--verbose] map.map...
exits 1 if the game reads a placeholder that holds data
"""

import argparse
import json
import multiprocessing
import os
import re
import shlex
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wii_convert_maps import Converter  # noqa: E402
from wii_tag_layout import Definition, Leaf, Schema, fourcc  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# the Wii's C types (powerpc-unknown-eabi, ILP32; -fshort-wchar): their sizes and kinds
BASE_TYPES = {
    "char": (1, "bytes"), "signed char": (1, "bytes"), "unsigned char": (1, "bytes"), "_Bool": (1, "bytes"),
    "short": (2, "int"), "unsigned short": (2, "int"), "wchar_t": (2, "int"),
    "int": (4, "int"), "unsigned int": (4, "int"), "long": (4, "int"), "unsigned long": (4, "int"),
    "long long": (8, "int"), "unsigned long long": (8, "int"), "__int64": (8, "int"),
    "unsigned __int64": (8, "int"), "float": (4, "float"), "double": (8, "float"),
}
QUALIFIERS = re.compile(r"\b(const|volatile|restrict|__restrict)\b")
ARRAY = re.compile(r"^(.*?)\s*\[(\d+)\]((?:\[\d+\])*)$")
# the tag's functions whose results are bound to a definition
TAG_GET = "tag_get"
BLOCK_GET = "tag_block_get_element_with_size"


def clean(type_name: str) -> str:
    return " ".join(QUALIFIERS.sub("", type_name).split())


def pointee(type_name: str) -> Optional[str]:
    """what a pointer type points to (None for another type, or a pointer to a function)"""
    type_name = clean(type_name)
    if not type_name.endswith("*") or type_name.endswith("(*)"):
        return None
    return type_name[:-1].strip()


def type_of(node: dict) -> str:
    kind = node.get("type", {})
    return kind.get("desugaredQualType") or kind.get("qualType", "")


# ---------- the code: what each unit reads through pointers into tags

class Walker:
    """a unit's AST, in the order clang prints it (whose locations leave out the file and the line
    when they are the last printed's): its reads through pointers to structures, and the types its
    tag_get and tag block elements are cast to

    An address is (root, steps): the root ("type", T) a pointer to T, or ("group", G) what tag_get
    returns for group G; the steps ("member", T, name) a field of a T, ("elements", T, n) n Ts
    further on (pointer arithmetic, a constant index)."""

    def __init__(self):
        self.file = ""
        self.line = 0
        self.enums: Dict[str, int] = {}
        self.reads: List[tuple] = []
        self.bindings: List[tuple] = []

    def _bare(self, location: dict) -> Tuple[str, int]:
        if "file" in location:
            self.file = location["file"]
        if "line" in location:
            self.line = location["line"]
        return self.file, self.line

    def _position(self, location: dict) -> Optional[Tuple[str, int]]:
        if not location:
            return None
        if "spellingLoc" in location:
            self._bare(location["spellingLoc"])
            return self._bare(location["expansionLoc"])
        return self._bare(location)

    def visit(self, tree: dict):
        stack = [tree]
        while stack:
            node = stack.pop()
            if "loc" in node:
                self._position(node["loc"])
            where = None
            if "range" in node:
                where = self._position(node["range"].get("begin", {}))
                self._position(node["range"].get("end", {}))
            kind = node.get("kind")
            if kind == "EnumConstantDecl":
                for inner in node.get("inner", ()):
                    if inner.get("kind") == "ConstantExpr" and "value" in inner:
                        self.enums[node["id"]] = int(inner["value"])
            elif kind in ("ImplicitCastExpr", "CStyleCastExpr"):
                self._cast(node, where or (self.file, self.line))
            stack.extend(reversed(node.get("inner", ())))

    # (expressions)

    @staticmethod
    def _strip(node: dict) -> dict:
        while node.get("kind") == "ParenExpr" or (node.get("kind") == "ImplicitCastExpr" and
                                                   node.get("castKind") in ("NoOp", "IntegralCast")):
            node = node["inner"][0]
        return node

    def constant(self, node: dict) -> Optional[int]:
        node = self._strip(node)
        kind = node.get("kind")
        if kind in ("IntegerLiteral", "CharacterLiteral"):
            return int(node["value"])
        if kind == "ConstantExpr" and "value" in node:
            return int(node["value"])
        if kind == "ConstantExpr":
            return self.constant(node["inner"][0])
        if kind == "DeclRefExpr" and node.get("referencedDecl", {}).get("kind") == "EnumConstantDecl":
            return self.enums.get(node["referencedDecl"]["id"])
        if kind == "UnaryOperator" and node.get("opcode") in ("-", "+"):
            value = self.constant(node["inner"][0])
            return None if value is None else (-value if node["opcode"] == "-" else value)
        if kind == "BinaryOperator" and node.get("opcode") in ("+", "-", "*", "<<"):
            left, right = self.constant(node["inner"][0]), self.constant(node["inner"][1])
            if left is None or right is None:
                return None
            return {"+": left + right, "-": left - right, "*": left * right, "<<": left << right}[node["opcode"]]
        return None

    def callee(self, node: dict) -> str:
        function = self._strip(node["inner"][0])
        while function.get("kind") == "ImplicitCastExpr":
            function = function["inner"][0]
        return function.get("referencedDecl", {}).get("name", "") if function.get("kind") == "DeclRefExpr" else ""

    def address(self, node: dict, depth: int = 0) -> Optional[tuple]:
        """the address a pointer expression holds, as (root, steps), if it is into a structure"""
        if depth > 64:
            return None
        kind = node.get("kind")
        if kind == "ParenExpr":
            return self.address(node["inner"][0], depth + 1)
        if kind in ("ImplicitCastExpr", "CStyleCastExpr"):
            cast = node.get("castKind")
            if cast in ("BitCast", "NoOp"):
                return self.address(node["inner"][0], depth + 1) or self._root(node)
            if cast == "ArrayToPointerDecay":
                return self.lvalue_address(node["inner"][0], depth + 1)
            if cast == "LValueToRValue":
                return self._root(node)
            return None
        if kind == "UnaryOperator" and node.get("opcode") == "&":
            return self.lvalue_address(node["inner"][0], depth + 1)
        if kind == "BinaryOperator" and node.get("opcode") in ("+", "-"):
            left, right = node["inner"]
            if pointee(type_of(left)) is None:
                if node["opcode"] == "-":
                    return None
                left, right = right, left
            element = pointee(type_of(left))
            count = self.constant(right)
            if element is None or count is None:
                return None
            base = self.address(left, depth + 1)
            if base is None:
                return None
            return base[0], base[1] + (("elements", element, -count if node["opcode"] == "-" else count),)
        if kind == "CallExpr" and self.callee(node) == TAG_GET and len(node["inner"]) == 3:
            group = self.constant(node["inner"][1])
            if group is not None:
                return ("group", group & 0xFFFFFFFF), ()
        return self._root(node)

    @staticmethod
    def _root(node: dict) -> Optional[tuple]:
        target = pointee(type_of(node))
        if target is None or target in BASE_TYPES or target == "void":
            return None
        return ("type", target), ()

    def lvalue_address(self, node: dict, depth: int = 0) -> Optional[tuple]:
        """the address of an lvalue, as (root, steps), if it is in a structure through a pointer"""
        if depth > 64:
            return None
        kind = node.get("kind")
        if kind == "ParenExpr":
            return self.lvalue_address(node["inner"][0], depth + 1)
        if kind == "MemberExpr":
            name = node.get("name", "")
            if not name:
                return None
            inner = node["inner"][0]
            if node.get("isArrow"):
                base, record = self.address(inner, depth + 1), pointee(type_of(inner))
            else:
                base, record = self.lvalue_address(inner, depth + 1), clean(type_of(inner))
            if base is None or record is None:
                return None
            return base[0], base[1] + (("member", record, name),)
        if kind == "ArraySubscriptExpr":
            base, index = node["inner"]
            if pointee(type_of(base)) is None:
                base, index = index, base
            count = self.constant(index)
            address = self.address(base, depth + 1)
            if address is None or count is None:
                return None
            return address[0], address[1] + (("elements", clean(type_of(node)), count),)
        if kind == "UnaryOperator" and node.get("opcode") == "*":
            return self.address(node["inner"][0], depth + 1)
        return None

    def _cast(self, node: dict, where: Tuple[str, int]):
        cast = node.get("castKind")
        inner = node["inner"][0]
        if cast == "LValueToRValue":
            address = self.lvalue_address(inner)
            if address is not None:
                self.reads.append((where, address, clean(type_of(inner)), "read"))
            return
        if cast != "BitCast":
            return
        target = pointee(type_of(node))
        if target is None or target == "void":
            return
        operand = self._strip(inner)
        while operand.get("kind") in ("ImplicitCastExpr", "CStyleCastExpr") and operand.get("castKind") == "BitCast":
            operand = self._strip(operand["inner"][0])
        if operand.get("kind") == "CallExpr":
            name = self.callee(operand)
            if name == TAG_GET and len(operand["inner"]) == 3:
                group = self.constant(operand["inner"][1])
                if group is not None:
                    self.bindings.append((target, ("group", group & 0xFFFFFFFF), (), "tag_get", where))
                return
            if name == BLOCK_GET and len(operand["inner"]) >= 2:
                block = self.address(operand["inner"][1])
                if block is not None:
                    self.bindings.append((target, block[0], block[1], "block", where))
                return
        # (a block's address read and cast: definition->things.address)
        if operand.get("kind") == "ImplicitCastExpr" and operand.get("castKind") == "LValueToRValue":
            member = self._strip(operand["inner"][0])
            if member.get("kind") == "MemberExpr" and member.get("name") == "address" and not member.get("isArrow"):
                block = self.lvalue_address(member["inner"][0])
                if block is not None:
                    self.bindings.append((target, block[0], block[1], "block", where))
                return
        # (a field's address cast to a pointer to something else: what it points to is read)
        address = self.address(inner)
        if address is not None and address[1]:
            self.reads.append((where, address, target, f"cast to {target} *"))


def unit_commands(elf: Path, root: Path) -> List[Tuple[str, List[str]]]:
    """each unit's compiler command, as the build runs it"""
    text = subprocess.run(["ninja", "-t", "commands", os.path.relpath(elf, root)], cwd=root, capture_output=True,
                          text=True, check=True).stdout
    result = []
    for line in text.splitlines():
        arguments = shlex.split(line)
        if not arguments or "clang" not in Path(arguments[0]).name or "-c" not in arguments:
            continue
        unit = arguments[arguments.index("-c") + 1]
        if not unit.endswith(".c"):
            continue
        command = []
        skip = 0
        for argument in arguments:
            if skip:
                skip -= 1
            elif argument in ("-MF", "-o"):
                skip = 1
            elif argument not in ("-MMD", "-MD", "-c"):
                command.append(argument)
        result.append((unit, command + ["-fsyntax-only", "-Xclang", "-ast-dump=json"]))
    return result


def unit_facts(job: Tuple[str, List[str], str]) -> Tuple[str, list, list, str]:
    unit, command, root = job
    run = subprocess.run(command, cwd=root, capture_output=True)
    if run.returncode != 0 or not run.stdout:
        return unit, [], [], run.stderr.decode(errors="replace")[-400:]
    walker = Walker()
    walker.visit(json.loads(run.stdout))
    return unit, walker.reads, walker.bindings, ""


# ---------- the maps: which left bytes hold data

_converter: Optional[Converter] = None


def map_bytes(path: Path) -> Dict[Tuple[int, int], int]:
    _converter.convert(path)
    return dict(_converter.unswapped_bytes)


# ---------- what the reads and views mean for the tags

class Checker:
    def __init__(self, converter: Converter):
        self.converter = converter
        self.executable = converter.executable
        self.schema = converter.schema
        self.definitions = list(self.schema.definitions.values())
        # each definition's runs left as they are: (offset, size, name)
        self.runs: Dict[int, List[Tuple[int, int, str]]] = {}
        for definition in self.definitions:
            plan = converter.plans[id(definition)]
            self.runs[id(definition)] = [(offset, size, name) for _, name, offset, size in plan.byte_runs]
        # (by the game's names, and the groups' children)
        self.by_name: Dict[str, List[Definition]] = defaultdict(list)
        for definition in self.definitions:
            for name in Schema.game_names(definition):
                self.by_name[name].append(definition)
        self.children: Dict[int, List[Definition]] = defaultdict(list)
        for group in self.schema.groups.values():
            for parent in group.parents:
                if parent in self.schema.groups and group.definition is not None:
                    self.children[parent].append(group.definition)
        self.group_of = {id(group.definition): tag for tag, group in self.schema.groups.items() if group.definition}
        self.embedded: Dict[str, List[Tuple[Definition, int]]] = defaultdict(list)
        for definition in self.definitions:
            for prefix, name in Schema.EMBEDDED.get(definition.name, []):
                inside = [leaf.offset for leaf in definition.leaves
                          if leaf.name.split(".", 1)[-1].startswith(prefix)]
                if inside:
                    self.embedded[name].append((definition, min(inside)))
        # names the code binds to definitions (tag_get, tag blocks): name -> {(definition, base)}
        self.bound: Dict[str, Dict[Tuple[int, int], Tuple[Definition, int]]] = defaultdict(dict)
        self.data: Dict[Tuple[int, int], int] = {}
        self._agree: Dict[Tuple[int, int, int], bool] = {}
        # (the reads found into tags; those into one definition, for --trace)
        self.into_tags = 0
        self.trace: Optional[str] = None
        self.traced: List[str] = []

    # (types, as a unit declares them)

    def die(self, unit: str, type_name: str):
        type_name = clean(type_name)
        die = self.executable.unit_types.get(unit, {}).get(type_name)
        if die is None:
            die = self.executable.types.get(type_name)
        return die

    def record(self, unit: str, type_name: str):
        """the structure or union a type names (through typedefs)"""
        die = self.die(unit, type_name)
        while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
            if "DW_AT_type" not in die.attributes:
                return None
            die = die.get_DIE_from_attribute("DW_AT_type")
        if die is None or die.tag not in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            return None
        if "DW_AT_declaration" in die.attributes:
            name = die.attributes["DW_AT_name"].value.decode() if "DW_AT_name" in die.attributes else ""
            return self.die(unit, f"struct {name}") if name and type_name != f"struct {name}" else None
        return die

    @staticmethod
    def record_name(die) -> str:
        return die.attributes["DW_AT_name"].value.decode() if "DW_AT_name" in die.attributes else ""

    def member(self, unit: str, type_name: str, name: str) -> Optional[int]:
        die = self.record(unit, type_name)
        if die is None:
            return None
        for child in die.iter_children():
            if child.tag == "DW_TAG_member" and "DW_AT_name" in child.attributes and \
                    child.attributes["DW_AT_name"].value.decode() == name:
                return child.attributes["DW_AT_data_member_location"].value \
                    if "DW_AT_data_member_location" in child.attributes else 0
        return None

    def leaves(self, unit: str, type_name: str, base: int) -> Optional[List[Leaf]]:
        type_name = clean(type_name)
        match = ARRAY.match(type_name)
        if match:
            element_type = match.group(1) + match.group(3)
            size = self.size(unit, element_type)
            if size is None:
                return None
            result = []
            for index in range(int(match.group(2))):
                inner = self.leaves(unit, element_type, base + index * size)
                if inner is None:
                    return None
                result.extend(inner)
            return result
        if pointee(type_name) is not None or "(*)" in type_name:
            return [Leaf(base, 4, "pointer", type_name)]
        if type_name in BASE_TYPES:
            size, kind = BASE_TYPES[type_name]
            return [Leaf(base, size, kind, type_name)]
        die = self.die(unit, type_name)
        if die is None and type_name.startswith("enum "):
            return [Leaf(base, 4, "int", type_name)]
        if die is None:
            return None
        result: List[Leaf] = []
        try:
            self.executable._flatten(die, base, type_name, result)
        except (ValueError, KeyError):
            size = self.executable.size_of(die)
            return [Leaf(base, size, "bits", type_name)]
        return result

    def size(self, unit: str, type_name: str) -> Optional[int]:
        type_name = clean(type_name)
        match = ARRAY.match(type_name)
        if match:
            element = self.size(unit, match.group(1) + match.group(3))
            return None if element is None else element * int(match.group(2))
        if pointee(type_name) is not None or "(*)" in type_name:
            return 4
        if type_name in BASE_TYPES:
            return BASE_TYPES[type_name][0]
        die = self.die(unit, type_name)
        if die is None:
            return 4 if type_name.startswith("enum ") else None
        try:
            return self.executable.size_of(die)
        except (KeyError, AttributeError):
            return None

    # (definitions)

    def family(self, definition: Definition) -> List[Definition]:
        """a group's definition and its children's, which begin with it"""
        tag = self.group_of.get(id(definition))
        return [definition] + (self.children.get(tag, []) if tag is not None else [])

    def agrees(self, definition: Definition, base: int, die) -> bool:
        """whether a structure, at base in a definition, contradicts none of its fields: each of its
        values is one the definition names, or lies in bytes it leaves as they are"""
        key = (id(definition), base, die.offset)
        if key in self._agree:
            return self._agree[key]
        result = True
        leaves: List[Leaf] = []
        try:
            self.executable._flatten(die, base, "", leaves)
        except (ValueError, KeyError):
            result = False
        if result and leaves and max(leaf.offset + leaf.size for leaf in leaves) > definition.size:
            result = False
        if result:
            anchors = {(leaf.offset, leaf.size, leaf.kind == "pointer") for leaf in definition.leaves
                       if leaf.kind != "bytes"}
            loose = [(leaf.offset, leaf.offset + leaf.size) for leaf in definition.leaves if leaf.kind == "bytes"] + \
                [(offset, offset + size) for offset, size, _ in self.runs[id(definition)]]
            for leaf in leaves:
                if leaf.kind == "bytes" or (leaf.offset, leaf.size, leaf.kind == "pointer") in anchors:
                    continue
                if not any(start <= leaf.offset and leaf.offset + leaf.size <= end for start, end in loose):
                    result = False
                    break
        self._agree[key] = result
        return result

    def definitions_of(self, unit: str, type_name: str) -> List[Tuple[Definition, int, str]]:
        """the definitions (and where in them) a pointer to a type of a unit points into"""
        die = self.record(unit, type_name)
        if die is None:
            return []
        name = self.record_name(die)
        if not name:
            return []
        candidates: List[Tuple[Definition, int]] = []
        for definition in self.by_name.get(name, []):
            candidates.extend((member, 0) for member in self.family(definition))
        candidates.extend(self.embedded.get(name, []))
        candidates.extend(self.bound.get(name, {}).values())
        result, seen = [], set()
        for definition, base in candidates:
            if (id(definition), base) in seen:
                continue
            seen.add((id(definition), base))
            if self.agrees(definition, base, die):
                result.append((definition, base, f"struct {name}"))
        return result

    def resolve(self, unit: str, root: tuple, steps: tuple) -> List[Tuple[Definition, int, str]]:
        if root[0] == "group":
            group = self.schema.groups.get(root[1])
            if group is None or group.definition is None:
                return []
            starts = [(definition, 0, f"tag_get('{fourcc(root[1])}')") for definition in self.family(group.definition)]
        else:
            starts = self.definitions_of(unit, root[1])
        if not starts:
            return []
        offset = 0
        for step in steps:
            if step[0] == "member":
                delta = self.member(unit, step[1], step[2])
            else:
                size = self.size(unit, step[1])
                delta = None if size is None else size * step[2]
            if delta is None:
                return []
            offset += delta
        return [(definition, base + offset, how) for definition, base, how in starts]

    def block_at(self, definition: Definition, offset: int) -> Optional[Definition]:
        for field in definition.fields:
            if field.type == "block" and field.definition is not None and \
                    field.offset <= offset < field.offset + max(field.count, 1) * field.size and \
                    (offset - field.offset) % field.size == 0:
                return field.definition
            if field.type == "struct" and field.definition is not None:
                for item in range(max(field.count, 1)):
                    start = field.offset + item * field.definition.size
                    if start <= offset < start + field.definition.size:
                        return self.block_at(field.definition, offset - start)
        return None

    def bind(self, facts: List[Tuple[str, list, list, str]]) -> int:
        """the types the code binds to definitions, until no more are found"""
        changed = True
        while changed:
            changed = False
            for unit, _, bindings, _ in facts:
                for target, root, steps, how, _ in bindings:
                    die = self.record(unit, target)
                    if die is None:
                        continue
                    name = self.record_name(die)
                    if not name:
                        continue
                    for definition, offset, _ in self.resolve(unit, root, steps):
                        if how == "block":
                            definition = self.block_at(definition, offset)
                            if definition is None:
                                continue
                            offset = 0
                        elif offset:
                            continue
                        key = (id(definition), offset)
                        if key not in self.bound[name]:
                            self.bound[name][key] = (definition, offset)
                            changed = True
        return sum(len(bound) for bound in self.bound.values())

    # (the check)

    def held(self, definition: Definition, leaf: Leaf) -> List[Tuple[int, int, str, int]]:
        """the runs with data a value overlaps, and the bytes of it that hold data"""
        found = []
        for offset, size, name in self.runs[id(definition)]:
            mask = self.data.get((id(definition), offset), 0)
            if not mask:
                continue
            start, end = max(offset, leaf.offset), min(offset + size, leaf.offset + leaf.size)
            if start >= end:
                continue
            data = mask.to_bytes(size, "big")[start - offset:end - offset]
            if any(data):
                found.append((offset, size, name, start))
        return found

    def check(self, facts) -> Tuple[Dict[Tuple[int, int], List[str]], Dict[Tuple[int, int], List[str]]]:
        reads: Dict[Tuple[int, int], Set[str]] = defaultdict(set)
        declared: Dict[Tuple[int, int], Set[str]] = defaultdict(set)
        for unit, unit_reads, _, _ in facts:
            for (file, line), (root, steps), type_name, how in unit_reads:
                targets = self.resolve(unit, root, steps)
                if targets:
                    self.into_tags += 1
                for definition, offset, view in targets:
                    if offset < 0 or offset >= definition.size:
                        continue
                    leaves = self.leaves(unit, type_name, offset)
                    if leaves is None:
                        continue
                    if definition.name == self.trace:
                        self.traced.append(f"{file}:{line}: {type_name} at {offset:#x} ({how} through {view})")
                    for leaf in leaves:
                        if leaf.size < 2 or leaf.kind == "bytes":
                            continue
                        for run_offset, _, name, _ in self.held(definition, leaf):
                            raw = any(step[0] == "elements" and clean(step[1]) in ("byte", "char", "unsigned char")
                                      for step in steps)
                            reads[(id(definition), run_offset)].add(
                                f"{file}:{line}: {leaf.kind} of {leaf.size} at {leaf.offset:#x} ({how} through "
                                f"{view}{', offset arithmetic' if raw else ''})")
        # the views: every unit's structures of the names (and the bound ones)
        names: Dict[str, List[Tuple[Definition, int]]] = defaultdict(list)
        for name, definitions in self.by_name.items():
            for definition in definitions:
                names[name].extend((member, 0) for member in self.family(definition))
        for name, embedded in self.embedded.items():
            names[name].extend(embedded)
        for name, bound in self.bound.items():
            names[name].extend(bound.values())
        seen = set()
        for unit, types in self.executable.unit_types.items():
            if "tag_schema" in unit:
                continue
            for name, candidates in names.items():
                die = types.get(f"struct {name}")
                if die is None or "DW_AT_byte_size" not in die.attributes:
                    continue
                for definition, base in candidates:
                    if not self.agrees(definition, base, die):
                        continue
                    leaves: List[Leaf] = []
                    self.executable._flatten(die, base, f"struct {name}", leaves)
                    for leaf in leaves:
                        if leaf.size < 2 or leaf.kind == "bytes":
                            continue
                        for run_offset, _, _, _ in self.held(definition, leaf):
                            key = (id(definition), run_offset, name, leaf.offset, leaf.size, leaf.kind)
                            if key in seen:
                                continue
                            seen.add(key)
                            declared[(id(definition), run_offset)].add(
                                f"struct {name} ({unit}) declares {leaf.name.split('.', 1)[-1]}: "
                                f"{leaf.kind} of {leaf.size} at {leaf.offset:#x}")
        return ({key: sorted(value) for key, value in reads.items()},
                {key: sorted(value) for key, value in declared.items()})


def main() -> int:
    global _converter
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("maps", nargs="+", type=Path)
    parser.add_argument("--elf", type=Path, default=Path("build/wii/halo.elf"))
    parser.add_argument("--root", type=Path, default=ROOT,
                        help="the checkout the executable was built from (its build.ninja and sources)")
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 1))
    parser.add_argument("--verbose", action="store_true", help="also list the placeholders not read")
    parser.add_argument("--trace", metavar="DEFINITION", help="list every read found into one definition")
    args = parser.parse_args()
    started = time.monotonic()
    elf = args.elf if args.elf.is_absolute() else (Path.cwd() / args.elf)
    _converter = Converter(elf, 0x90100000)
    checker = Checker(_converter)
    context = multiprocessing.get_context("fork")

    # the maps (each converted in memory)
    maps = [path.resolve() for path in args.maps]
    with context.Pool(min(args.jobs, 8, len(maps))) as pool:
        for found in pool.imap_unordered(map_bytes, maps):
            for key, mask in found.items():
                checker.data[key] = checker.data.get(key, 0) | mask
    held = [(definition, offset, size, name) for definition in checker.definitions
            for offset, size, name in checker.runs[id(definition)] if checker.data.get((id(definition), offset))]
    print(f"maps: {len(maps)}, placeholders and gaps holding data: {len(held)} in "
          f"{len({id(definition) for definition, *_ in held})} definitions ({time.monotonic() - started:.0f} s)")

    # the code
    # (a unit the executable has no debug information of links no code: the tag definitions' files)
    commands = unit_commands(elf, args.root)
    units = [(unit, command, str(args.root.resolve())) for unit, command in commands
             if unit in _converter.executable.unit_types and "tag_schema" not in unit]
    facts = []
    failed = []
    with context.Pool(args.jobs) as pool:
        for unit, reads, bindings, error in pool.imap_unordered(unit_facts, units):
            facts.append((unit, reads, bindings, error))
            if error:
                failed.append(f"{unit}: {error.strip().splitlines()[-1] if error.strip() else 'no output'}")
    bound = checker.bind(facts)
    print(f"units: {len(facts)} (of {len(commands)}; the others link no code or are the schema's), "
          f"reads through pointers to structures: {sum(len(f[1]) for f in facts)}, "
          f"types bound to tags by the code: {bound} ({time.monotonic() - started:.0f} s)")
    for line in failed:
        print(f"  not parsed: {line}")

    checker.trace = args.trace
    reads, declared = checker.check(facts)
    print(f"reads into tags: {checker.into_tags}")
    for line in sorted(set(checker.traced)):
        print(f"  {line}")
    flagged = 0
    for definition, offset, size, name in held:
        key = (id(definition), offset)
        mask = checker.data[key].to_bytes(size, "big")
        data = [offset + index for index, byte in enumerate(mask) if byte]
        what = f"{name} ({offset:#x}-{offset + size - 1:#x}, data at {data[0]:#x}-{data[-1]:#x})"
        if key in reads or key in declared:
            flagged += 1
            print(f"read: {what}")
            for line in reads.get(key, []):
                print(f"  {line}")
            for line in declared.get(key, []):
                print(f"  {line}")
        elif args.verbose:
            print(f"not read: {what}")
    print(f"{flagged} of {len(held)} placeholders holding data are read by the game "
          f"({time.monotonic() - started:.0f} s)")
    return 1 if flagged or failed else 0


if __name__ == "__main__":
    sys.exit(main())
