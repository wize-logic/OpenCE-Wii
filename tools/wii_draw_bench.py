#!/usr/bin/env python3
"""The Wii's Direct3D device drawing on the host: instructions a draw takes, and what it sends GX.

port/wii/tests/draw_bench.c includes port/wii/src/d3d8_gx.c, compiled as the Wii build compiles
it (clang for powerpc-eabi, tools/wii_build.py's flags), over a stand-in of GX
(port/wii/tests/host/gx_stand_in.c), and draws synthetic scenes: a structure's lightmapped
sections, a rigid model, a skinned and lit character. It is linked against the host's
powerpc-linux C library and run under qemu-ppc.

  count     the instructions of each draw (qemu's trace, an instruction at a time, between
            the bench's region marks), by scene: the first frame's draws and the later ones',
            per draw and per index, and where the later ones' go (by function)
  record    each draw's digest of what GX would draw (the vertices as GX reads them, through
            its arrays and matrices, and the state): --save FILE keeps them, --compare FILE
            checks another build of the device draws the same
  programs  the game's vertex programs, their declarations and dependencies
  subdivide a sphere map read across big triangles (d3d8_gx.c's subdivide_triangle): a flat
            sea's triangles, their pieces against each point's own direction; exits 1 where
            they are off or do not cover the triangle

usage: tools/wii_draw_bench.py count [--scene all] [--frames 4]
       tools/wii_draw_bench.py record --save build/wii/bench/before.txt
       tools/wii_draw_bench.py record --compare build/wii/bench/before.txt
       tools/wii_draw_bench.py subdivide

Needs devkitPPC (/opt/devkitpro, or DEVKITPRO), WiiLibs (WIILIBS, else ../WiiLibs), clang,
powerpc-linux-gnu-gcc and qemu-ppc, and a configured `ninja wii` (its
build/wii/platform_msvc_semantics.h).
"""

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools.wii_build import PLATFORM_FLAGS, wii_abi_flags  # noqa: E402

WORK = ROOT / "build" / "wii" / "bench"
WII = ROOT / "port" / "wii"
LINUX_SOURCES = ROOT / "port" / "linux" / "src"
MICROCODE = ROOT / "source" / "rasterizer" / "xbox" / "rasterizer_xbox_vertex_shaders_data.inc"


def wiilibs_dir() -> Path:
    for candidate in (os.environ.get("WIILIBS"), ROOT / "WiiLibs", ROOT.parent / "WiiLibs"):
        if candidate and (Path(candidate) / "wl").is_dir():
            return Path(candidate)
    sys.exit("wii_draw_bench: no WiiLibs (WIILIBS, ./WiiLibs or ../WiiLibs)")


def recording_header(devkitpro: Path, out: Path) -> None:
    """libogc's <ogc/gx.h> with each vertex write a call of the stand-in's wg_record_*"""
    text = (devkitpro / "libogc" / "include" / "ogc" / "gx.h").read_text(encoding="utf-8")
    text = re.sub(r'#include "(lwp|gx_struct|gu)\.h"', r"#include <ogc/\1.h>", text)
    text = re.sub(r"wgPipe->([A-Z0-9]+) = (.*);", r"wg_record_\1(\2);", text)
    declarations = "".join(f"void wg_record_{kind}({kind.lower()} value);\n"
                           for kind in ("U8", "S8", "U16", "S16", "U32", "S32", "F32"))
    text = text.replace("extern WGPipe* const wgPipe;", "extern WGPipe* const wgPipe;\n" + declarations)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")


def user_mode_headers(devkitpro: Path, include: Path) -> None:
    """libogc's processor intrinsics without the supervisor's instructions (PPCSync's interrupt
    lock and HID0), which qemu's user mode refuses: each becomes an instruction of no effect"""
    for name in ("intrinsics.h", "cache.h"):
        text = (devkitpro / "libogc" / "include" / "tuxedo" / "ppc" / name).read_text(encoding="utf-8")
        text = text.replace('#include "../types.h"', "#include <tuxedo/types.h>")
        text = text.replace('#include "spr.h"', "#include <tuxedo/ppc/spr.h>")
        text = text.replace('#include "intrinsics.h"', "#include <tuxedo/ppc/intrinsics.h>")
        text = text.replace('"mfmsr %0"', '"li %0, 0"').replace('"mtmsr %0"', '"nop"')
        text = text.replace('"mfspr %0, %1"', '"li %0, 0"').replace('"mtspr %0, %1"', '"nop"')
        out = include / "tuxedo" / "ppc" / name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")
    # (and the headers that reach them by a relative path)
    for name in ("ogc/cache.h", "ogc/irq.h", "ogc/machine/processor.h"):
        text = (devkitpro / "libogc" / "include" / name).read_text(encoding="utf-8")
        text = re.sub(r'#include "(?:\.\./)+tuxedo/([^"]+)"', r"#include <tuxedo/\1>", text)
        out = include / name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")


def run(command, **kwargs):
    result = subprocess.run(command, **kwargs)
    if result.returncode:
        sys.exit(f"wii_draw_bench: failed: {' '.join(map(str, command))}")
    return result


def build(record: bool, device: str = None) -> Path:
    devkitpro = Path(os.environ.get("DEVKITPRO") or "/opt/devkitpro")
    wiilibs = wiilibs_dir()
    semantics = ROOT / "build" / "wii" / "platform_msvc_semantics.h"
    if not semantics.is_file():
        sys.exit("wii_draw_bench: no build/wii/platform_msvc_semantics.h (configure and run `ninja wii` first)")
    kind = "record" if record else "count"
    # (the device's sources: port/wii/src, or another copy of them, to compare with)
    src = Path(device).resolve() if device else WII / "src"
    out_dir = WORK / (kind + ("-device" if device else ""))
    out_dir.mkdir(parents=True, exist_ok=True)
    # the vertex programs compiled ahead of time, as tools/wii_nv2a_check.py makes them
    codegen = WORK / "nv2a_codegen"
    programs = WORK / "nv2a_programs.c"
    run(["cc", "-std=gnu11", "-O1", "-ffp-contract=off", "-DNV2A_CPU_NO_NATIVES", f"-I{WII / 'tests' / 'host'}",
         f"-I{src}", f"-I{wiilibs}", "-o", str(codegen),
         str(src.parent / "tools" / "nv2a_codegen.c" if device else WII / "tools" / "nv2a_codegen.c"),
         str(wiilibs / "wl" / "gfx" / "gfx_sqrt.c"), "-lm"])
    run([str(codegen), str(MICROCODE), str(programs)])
    user_mode_headers(devkitpro, out_dir / "include")
    includes = [f"-I{out_dir / 'include'}"]
    defines = []
    if record:
        recording_header(devkitpro, out_dir / "include" / "ogc" / "gx.h")
        defines.append("-DGX_STAND_IN_RECORD")
    flags = [*" ".join(wii_abi_flags(devkitpro)).split(), *PLATFORM_FLAGS, "-Wno-unused-but-set-variable",
             *defines, *includes,
             "-include", str(WII / "include" / "halo_wii_prefix.h"), "-include", str(semantics),
             f"-I{src}", f"-I{LINUX_SOURCES}", f"-I{WII / 'include'}",
             f"-I{ROOT / 'port' / 'linux' / 'include'}", f"-I{wiilibs}", f"-I{ROOT / 'source'}",
             f"-I{ROOT / 'source' / 'cseries'}", "-idirafter", str(ROOT / "port" / "include" / "xdk")]
    plain = [*" ".join(wii_abi_flags(devkitpro)).split(), "-std=gnu11", *defines, *includes, f"-I{wiilibs}",
             f"-I{src}", f"-I{WII / 'tests' / 'host'}"]
    units = [
        (WII / "tests" / "draw_bench.c", flags),
        (WII / "tests" / "host" / "gx_stand_in.c", plain),
        (src / "gx_psh.c", flags),
        (src / "gx_psh_cache.c", flags),
        (src / "gx_lighting.c", flags),
        (src / "nv2a_cpu.c", flags),
        (programs, flags),
        (LINUX_SOURCES / "d3d8_state.c", flags),
        (LINUX_SOURCES / "xbox_texture_formats.c", flags),
        (wiilibs / "wl" / "platform" / "tev.c", plain),
        (wiilibs / "wl" / "base" / "hash.c", plain),
        (wiilibs / "wl" / "gfx" / "gfx_sqrt.c", plain),
        (wiilibs / "wl" / "platform" / "wii" / "tev_wii.c", plain),
    ]
    objects = []
    for source, unit_flags in units:
        obj = out_dir / (source.stem + ".o")
        run(["clang", *unit_flags, "-c", str(source), "-o", str(obj)])
        objects.append(obj)
    host = out_dir / "bench_host.o"
    run(["powerpc-linux-gnu-gcc", "-O2", "-c", str(WII / "tests" / "host" / "bench_host.c"), "-o", str(host)])
    binary = out_dir / "draw_bench"
    run(["powerpc-linux-gnu-gcc", "-static", "-o", str(binary), str(host), *map(str, objects), "-lm"])
    return binary


def bench_arguments(args) -> list:
    return [args.scene, str(args.frames)]


def count(args) -> int:
    binary = build(record=False, device=args.device)
    with tempfile.TemporaryDirectory() as temporary:
        fifo = Path(temporary) / "trace"
        os.mkfifo(fifo)
        process = subprocess.Popen(["qemu-ppc", "-one-insn-per-tb", "-d", "nochain,exec", "-D", str(fifo), str(binary),
                                    *bench_arguments(args)], stdout=subprocess.PIPE, text=True)
        labels = []
        reader = threading.Thread(target=lambda: labels.extend(line.split(None, 1)[1].strip()
                                                               for line in process.stdout if line.startswith("region ")))
        reader.start()
        regions = []
        inside = None
        symbol_pattern = re.compile(r"\] (\S+)\s*$")
        pc_pattern = re.compile(r"\[[0-9a-f]+/([0-9a-f]+)/")
        lines = collections.Counter()
        callers = collections.Counter()
        stack = ["draw_streams"]
        with open(fifo, "r", encoding="utf-8", errors="replace") as trace:
            for line in trace:
                match = symbol_pattern.search(line)
                symbol = match.group(1) if match else "?"
                if symbol == "bench_region_begin":
                    inside = collections.Counter()
                    stack = ["draw_streams"]
                    continue
                if symbol == "bench_region_end":
                    if inside is not None:
                        regions.append(inside)
                    inside = None
                    continue
                if inside is not None:
                    inside[symbol] += 1
                    if args.callers:
                        # (calls and returns guessed from the symbol's changes: a symbol on the
                        # stack is a return to it, any other a call)
                        if symbol != stack[-1]:
                            if symbol in stack:
                                del stack[len(stack) - stack[::-1].index(symbol):]
                            else:
                                stack.append(symbol)
                        if args.callers in stack:
                            at = len(stack) - 1 - stack[::-1].index(args.callers)
                            callers[stack[at - 1] if at else "?"] += 1
                    if args.lines and symbol == args.lines:
                        pc = pc_pattern.search(line)
                        if pc:
                            lines[int(pc.group(1), 16)] += 1
        process.wait()
        reader.join()
    if args.lines:
        # the function's hottest source lines, over every region
        addresses = sorted(lines)
        located = run(["powerpc-linux-gnu-addr2line", "-e", str(binary), *[hex(a) for a in addresses]],
                      stdout=subprocess.PIPE, text=True).stdout.split()
        by_line = collections.Counter()
        for address, place in zip(addresses, located):
            by_line[place.rsplit("/", 1)[-1]] += lines[address]
        total = sum(by_line.values())
        print(f"{args.lines}: {total} instructions")
        for place, instructions in by_line.most_common(40):
            print(f"    {instructions:10d}  {place}")
    if args.callers:
        print(f"{args.callers}, with what it calls, by its caller:")
        for caller, instructions in callers.most_common(20):
            print(f"    {instructions:10d}  {caller}")
    if len(labels) != len(regions):
        print(f"wii_draw_bench: {len(labels)} regions named, {len(regions)} counted")
    # (scene, first frame or later) -> draws, indices, instructions, by function
    groups = collections.OrderedDict()
    for label, counter in zip(labels, regions):
        fields = dict(part.split("=", 1) for part in label.split() if "=" in part)
        scene = label.split()[0]
        key = (scene, "first frame" if fields.get("frame") == "0" else "later frames")
        group = groups.setdefault(key, {"draws": 0, "indices": 0, "total": 0, "gx": 0, "functions": collections.Counter()})
        group["draws"] += 1
        group["indices"] += int(fields.get("indices", 0))
        for symbol, instructions in counter.items():
            if symbol.startswith("GX_") or symbol.startswith("wg_"):
                group["gx"] += instructions
            else:
                group["total"] += instructions
            group["functions"][symbol] += instructions
    for (scene, frames), group in groups.items():
        draws = group["draws"]
        print(f"{scene}, {frames}: {draws} draws, {group['indices'] / draws:.0f} indices a draw: "
              f"{group['total'] / draws:.0f} instructions a draw ({group['total'] / max(group['indices'], 1):.1f} an "
              f"index), and {group['gx'] / draws:.0f} in GX's stand-in")
        if frames == "later frames" or args.verbose:
            for symbol, instructions in group["functions"].most_common(args.top):
                print(f"    {instructions / draws:10.0f}  {symbol}")
    return 0


def record(args) -> int:
    binary = build(record=True, device=args.device)
    output = run(["qemu-ppc", str(binary), *bench_arguments(args), "record"], stdout=subprocess.PIPE,
                 text=True).stdout
    lines = [line for line in output.splitlines() if line.startswith("draw ")]
    print(f"{len(lines)} draws recorded")
    if args.save:
        Path(args.save).parent.mkdir(parents=True, exist_ok=True)
        Path(args.save).write_text("\n".join(lines) + "\n", encoding="utf-8")
    if args.compare:
        before = Path(args.compare).read_text(encoding="utf-8").splitlines()
        differ = [(a, b) for a, b in zip(before, lines) if a != b]
        if len(before) != len(lines):
            print(f"{len(before)} draws before, {len(lines)} now")
        for a, b in differ[:20]:
            print(f"  before: {a}\n  now:    {b}")
        print(f"{len(differ)} of {min(len(before), len(lines))} draws differ")
        return 1 if differ or len(before) != len(lines) else 0
    return 0


def programs(args) -> int:
    binary = build(record=False)
    run(["qemu-ppc", str(binary), "programs"])
    return 0


def subdivide(args) -> int:
    binary = build(record=False, device=args.device)
    return subprocess.run(["qemu-ppc", str(binary), "subdivide"]).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="mode", required=True)
    for name in ("count", "record", "programs", "subdivide"):
        command = sub.add_parser(name)
        command.add_argument("--scene", default="all", help="environment, rigid, skinned, unlit, fogged or all")
        command.add_argument("--frames", type=int, default=4)
        command.add_argument("--device", help="another copy of port/wii/src (with tools/ beside it) to build instead")
        if name == "count":
            command.add_argument("--top", type=int, default=12, help="functions listed for the later frames")
            command.add_argument("--verbose", action="store_true", help="the first frame's functions too")
            command.add_argument("--lines", help="the hottest source lines of this function")
            command.add_argument("--callers", help="this function's instructions (and its callees') by its caller")
        if name == "record":
            command.add_argument("--save", help="keep the digests in this file")
            command.add_argument("--compare", help="compare with the digests this file kept")
    args = parser.parse_args()
    os.chdir(ROOT)
    return {"count": count, "record": record, "programs": programs, "subdivide": subdivide}[args.mode](args)


if __name__ == "__main__":
    sys.exit(main())
