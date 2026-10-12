"""Ninja rules for the Wii build (``ninja wii``).

It compiles the game sources with clang for 32-bit big-endian PowerPC (the
Wii's Broadway, a PowerPC 750CL) against devkitPPC's newlib and libogc, adds
the shared platform files that port/wii/port.json keeps and the Wii's own
(port/wii/src), links an ELF with devkitPPC's toolchain and converts it to
build/wii/halo.dol for the Homebrew Channel. WiiLibs (../WiiLibs, or
configure.py --wiilibs) is built with its own CMake and linked in. See
port/wii/README.md for the design.
"""

import json
import os
import re
from pathlib import Path
from typing import Any, Dict, List

from .linux_build import (GAME_FLAGS, MUSL_MATH_DIR, OPTIMISATION, TOML_DIR, ZLIB_DEFINES, ZLIB_DIR, ZLIB_SOURCES,
                          XDK_INCLUDE, compile_launcher, game_defines_and_includes, game_sources,
                          musl_math_sources, xdk_headers)
from .ninja_syntax import Writer

PORT_DIR = Path("port/wii")
PORT_CONFIG = PORT_DIR / "port.json"
LINUX_DIR = Path("port/linux")
LINUX_CONFIG = LINUX_DIR / "port.json"
LINK_SCRIPT = Path("tools/wii_link.py")

# WiiLibs' libraries the port links, in link order (WiiLibs/README.md)
WIILIBS_LIBRARIES = ("wl/platform/libwl_platform.a", "wl/audio/libwl_audio.a", "wl/audio/libwl_audio_core.a",
                     "wl/gxtex/libwl_gxtex.a", "wl/gfx/libwl_gfx.a", "wl/codec/libwl_codec.a", "wl/base/libwl_base.a")

# Flags shared by the game and the XDK-facing half of the platform layer:
# the MSVC/Xbox ABI the source was written against (as the Linux build's
# LINUX_ABI_FLAGS), on the Wii's processor:
#  - 16-bit wchar_t, __declspec, __int64, tentative definitions shared
#    between units,
#  - small structures and unions returned in registers, as Win32 does
#    (-msvr4-struct-return, PowerPC's spelling of -freg-struct-return:
#    hs_runtime.c calls union-returning converters through pointers typed as
#    returning long),
#  - no optimisations that assume the absence of MSVC-tolerated UB,
#  - no frame pointers, unlike the x86 builds: PowerPC's frames are chained
#    whether or not r31 holds one (each frame's first word is the caller's,
#    the EABI's back chain), which is what WiiLibs' backtraces and crash
#    report follow; a frame pointer only cost every function r31 and
#    every leaf function a frame (3% of the game's matrix and vector
#    functions' instructions, 4% of their code),
#  - no fused multiply-adds (PowerPC has fmadd): every port simulates the
#    same game from the same inputs (port/include/halo_math.h),
#  - thread-local variables through libgcc's emulation (__emutls_get_address,
#    over pthread keys): the PowerPC ABI addresses them from r2, which the
#    EABI gives the small data area and libogc no thread's storage,
#  - plain char signed, as MSVC's (PowerPC's is unsigned: a char holding
#    NONE would read as 255),
#  - libogc's Wii (HW_RVL) declarations.
# -malign-double is not needed: the PowerPC EABI already aligns doubles and
# 64-bit integers to 8 bytes, as MSVC does.
def wii_abi_flags(devkitpro: Path) -> List[str]:
    return [
        "--target=powerpc-unknown-eabi",
        "-mcpu=750",
        "-fms-extensions",
        "-fshort-wchar",
        "-fsigned-char",
        "-fcommon",
        "-fno-pic",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-fno-delete-null-pointer-checks",
        "-msvr4-struct-return",
        "-fomit-frame-pointer",
        "-ffp-contract=off",
        "-femulated-tls",
        "-ffunction-sections",
        "-fdata-sections",
        OPTIMISATION,
        "-g",
        # every type in the debug information, those only sizeof names (the tag schema's)
        # too: the map converter reads the tags' layouts from it (tools/wii_tag_layout.py)
        "-fno-eliminate-unused-debug-types",
        "-DGEKKO",
        "-DHW_RVL",
        "-D__wii__",
        # not the host's /usr/include (some clangs add it for this target: glibc's limits.h
        # then fails); clang's own headers stay, and devkitPPC's newlib follows them
        "-nostdlibinc",
        f"-isystem {devkitpro / 'libogc' / 'include'}",
        f"-isystem {devkitpro / 'devkitPPC' / 'powerpc-eabi' / 'include'}",
        # newlib's wide string functions assume its own wchar_t; keep clang
        # from turning loops into calls to them (as the Linux build)
        *(f"-fno-builtin-{name}" for name in (
            "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
            "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
            "wmemmove", "wmemset",
        )),
    ]


# Platform files whose floating point only reaches the speakers, compiled with
# fused multiply-adds (-ffp-contract=fast; every other unit has them off, so
# that the Wii's game matches x86's bit for bit). The sound mixer: its low
# pass is a multiply and an add a tap, 48 taps a voice a frame, and was most
# of b30's processor time on the Wii (WiiLibs' profiler); fused, its
# instructions under qemu-ppc for a scene of 24 voices fall 14%, and its
# output moves by 3.6e-7 at most (-129 dBFS: the four-lane sum against tap by
# tap moves it by 4.2e-7). The lockstep check's x86 side mixes nothing (SDL's
# dummy audio), and the game never reads the mix back.
CONTRACTED_SOURCES = {"dsound_sdl.c"}

PLATFORM_FLAGS = [
    "-std=gnu11",
    "-D_GNU_SOURCE",
    "-DHALO_LINUX_PLATFORM_LAYER",
    "-Wall",
    "-Wno-unused-function",
    "-Wno-unknown-pragmas",
    "-Wno-microsoft-anon-tag",
    "-Wno-pragma-pack",
    "-Wno-ignored-attributes",
    "-Wno-duplicate-decl-specifier",
    "-Wno-missing-braces",
    "-Wno-unused-variable",
    "-Wno-ignored-pragmas",
]


def _load(path: Path) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def devkitpro_dir(sln: Any) -> Path:
    """devkitPro (configure.py --devkitpro, else DEVKITPRO, else /opt/devkitpro)"""
    explicit = getattr(sln, "devkitpro", None)
    return Path(explicit or os.environ.get("DEVKITPRO") or "/opt/devkitpro")


def wiilibs_dir(sln: Any) -> Path:
    """WiiLibs' checkout (configure.py --wiilibs, else ../WiiLibs)"""
    return Path(getattr(sln, "wiilibs", None) or "../WiiLibs")


def heap_overflow_wraps(wiilibs: Path) -> List[str]:
    """The names WiiLibs' heap overflow wraps (WL_HEAP_OVERFLOW_WRAPS in its cmake/WiiLibs.cmake), so that
    the list is WiiLibs' own and cannot drift from it"""
    try:
        text = (wiilibs / "cmake" / "WiiLibs.cmake").read_text(encoding="utf-8")
        match = re.search(r"set\(WL_HEAP_OVERFLOW_WRAPS\s+([^)]*)\)", text)
        names = match.group(1).split() if match else []
    except OSError:
        names = []
    return names or ["malloc", "calloc", "realloc", "free", "memalign", "aligned_alloc", "malloc_usable_size",
                     "_malloc_r", "_calloc_r", "_realloc_r", "_free_r", "_memalign_r", "_malloc_usable_size_r"]


def wii_configure_inputs() -> List[Path]:
    """Files whose change must re-run configure.py."""
    if not PORT_CONFIG.is_file():
        return [Path(__file__)]
    return [PORT_CONFIG, Path(__file__), PORT_DIR / "src"]


def generate_wii_build(n: Writer, sln: Any) -> None:
    if not PORT_CONFIG.is_file() or not LINUX_CONFIG.is_file():
        return
    devkitpro = devkitpro_dir(sln)
    if not (devkitpro / "devkitPPC" / "bin" / "powerpc-eabi-gcc").is_file():
        # (no devkitPPC here: `ninja wii` is left out, as the Windows build is
        # without SDL)
        return
    config = _load(PORT_CONFIG)
    linux_config = _load(LINUX_CONFIG)
    build_dir: Path = sln.build_dir / "wii"
    obj_dir = build_dir / "obj"
    elf = build_dir / "halo.elf"
    dol = build_dir / "halo.dol"
    tools = devkitpro / "devkitPPC" / "bin"
    prefix_header = PORT_DIR / "include" / "halo_wii_prefix.h"
    semantics_header = build_dir / "halo_msvc_semantics.h"
    platform_semantics_header = build_dir / "platform_msvc_semantics.h"
    wiilibs = wiilibs_dir(sln)
    wiilibs_build = build_dir / "wiilibs"
    wiilibs_outputs = [wiilibs_build / library for library in WIILIBS_LIBRARIES]

    n.comment("Wii build (ninja wii)")
    n.variable("wii_cc", "clang")
    n.rule(
        name="wii_msvc_semantics",
        command="$python tools/linux_msvc_semantics.py --output $out $scan",
        description="WII MSVC SEMANTICS $out",
        restat=True,
    )
    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    n.build(
        outputs=semantics_header,
        rule="wii_msvc_semantics",
        implicit=[Path("tools/linux_msvc_semantics.py"), *xdk_headers(), *game_headers],
        variables={"scan": f"--all-inlines --tags source --inlines source --inlines {XDK_INCLUDE}"},
    )
    n.build(
        outputs=platform_semantics_header,
        rule="wii_msvc_semantics",
        implicit=[Path("tools/linux_msvc_semantics.py"), *xdk_headers()],
        variables={"scan": f"--inlines {XDK_INCLUDE}"},
    )
    n.rule(
        name="wii_cc",
        command=f"{compile_launcher(sln)}$wii_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="WII CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    # WiiLibs, with its own CMake and devkitPro's toolchain file
    n.rule(
        name="wii_wiilibs",
        command=(f"cmake -S {_quote(wiilibs)} -B {_quote(wiilibs_build)} -G Ninja "
                 f"-DCMAKE_TOOLCHAIN_FILE={devkitpro / 'cmake' / 'Wii.cmake'} -DCMAKE_BUILD_TYPE=Release "
                 f"-DWL_BUILD_TESTS=OFF > {build_dir}/wiilibs-configure.log && "
                 f"ninja -C {_quote(wiilibs_build)} > {build_dir}/wiilibs-build.log"),
        description="WII WIILIBS",
        pool="console",
        restat=True,
    )
    n.build(outputs=wiilibs_outputs, rule="wii_wiilibs",
            implicit=[*sorted(wiilibs.glob("wl/**/*.c")), *sorted(wiilibs.glob("wl/**/*.h")),
                      *sorted(wiilibs.glob("**/CMakeLists.txt"))])

    abi = " ".join(wii_abi_flags(devkitpro) + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []))
    sdk_flags = f"-idirafter {XDK_INCLUDE}"
    # the Wii's own headers first, then those it shares with the Linux build
    port_includes = f"-I{PORT_DIR / 'include'} -I{LINUX_DIR / 'include'}"
    objects: List[Path] = []

    def add_object(source: Path, cflags: str) -> None:
        obj = obj_dir / source.with_suffix(".o")
        objects.append(obj)
        n.build(
            outputs=obj,
            rule="wii_cc",
            inputs=source,
            implicit=[*xdk_headers(), prefix_header, semantics_header, platform_semantics_header],
            variables={"cflags": cflags},
        )

    game_cflags = " ".join([
        abi,
        " ".join(GAME_FLAGS),
        f"-include {prefix_header}",
        f"-include {semantics_header}",
        port_includes,
        f"-iquote {Path(linux_config['game_sources'])}",
        game_defines_and_includes(linux_config),
        sdk_flags,
    ])
    # WII_BOUNDS_CHECK=source/ai,source/physics (at configure time): those folders' units trap
    # at an index past a sized array's end (clang's array-bounds check, in trap mode: no
    # runtime); the crash report names the place. For finding a write into the stack
    bounds_checked = [part for part in os.environ.get("WII_BOUNDS_CHECK", "").split(",") if part]

    def checked(source: Path) -> str:
        if any(str(source).startswith(part) for part in bounds_checked):
            return game_cflags + " -fsanitize=array-bounds -fsanitize-trap=array-bounds"
        return game_cflags

    for source in game_sources(linux_config):
        add_object(source, checked(source))
    for source in sorted(Path(linux_config["game_sources"]).glob("*.c")):
        add_object(source, checked(source))

    linux_platform_dir = Path(linux_config["platform_sources"])
    wii_platform_dir = Path(config["platform_sources"])
    platform_cflags = " ".join([
        abi,
        " ".join(PLATFORM_FLAGS),
        f"-include {prefix_header}",
        f"-include {platform_semantics_header}",
        f"-I{wii_platform_dir}",
        f"-I{linux_platform_dir}",
        port_includes,
        f"-I{ZLIB_DIR}",
        f"-I{TOML_DIR}",
        f"-I{wiilibs}",
        "-Isource -Isource/cseries",
        sdk_flags,
    ])
    # Platform files named posix_*.c (and the Wii's wii_posix.c) talk to
    # newlib only: built without the XDK headers and the MSVC environment, so
    # newlib's structures (struct stat) have their own layout, as the Linux
    # build's posix_*.c have glibc's
    posix_cflags = " ".join([*wii_abi_flags(devkitpro), "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_WII",
                             f"-I{linux_platform_dir}", f"-I{wiilibs}", "-Wall"])

    def platform_flags(source: Path) -> str:
        if source.name in CONTRACTED_SOURCES:
            return f"{platform_cflags} -ffp-contract=fast"
        return posix_cflags if source.name.startswith("posix_") or source.name == "wii_posix.c" else platform_cflags

    for name in config["shared_platform_sources"]:
        add_object(linux_platform_dir / name, platform_flags(linux_platform_dir / name))
    for source in sorted(wii_platform_dir.glob("*.c")):
        add_object(source, platform_flags(source))
    # the game's vertex programs compiled ahead of time: a host tool decodes
    # its microcode with nv2a_cpu.c's decoder and writes each program as C
    # (port/wii/tools/nv2a_codegen.c)
    codegen_source = PORT_DIR / "tools" / "nv2a_codegen.c"
    codegen = build_dir / "tools" / "nv2a_codegen"
    microcode = Path("source/rasterizer/xbox/rasterizer_xbox_vertex_shaders_data.inc")
    programs = build_dir / "gen" / "nv2a_programs.c"
    n.rule(name="wii_host_cc",
           command=f"cc -O1 -std=gnu11 -DNV2A_CPU_NO_NATIVES -I{PORT_DIR / 'tests' / 'host'} -I{wii_platform_dir} "
                   "-o $out $in -lm",
           description="WII HOST CC $out")
    n.build(outputs=codegen, rule="wii_host_cc", inputs=codegen_source,
            implicit=[wii_platform_dir / "nv2a_cpu.c", wii_platform_dir / "nv2a_cpu.h",
                      wii_platform_dir / "nv2a_cpu_native.h", PORT_DIR / "tests" / "host" / "platform.h"])
    n.rule(name="wii_nv2a_codegen", command="$in $microcode $out > /dev/null", description="WII NV2A PROGRAMS $out")
    n.build(outputs=programs, rule="wii_nv2a_codegen", inputs=codegen, implicit=[microcode],
            variables={"microcode": str(microcode)})
    add_object(programs, platform_cflags)
    # WiiLibs' Xbox texture conversions (wl/codec/xbox_tex.c) as a host library
    # the map converter loads (tools/wii_gx_textures.py; without it, its numpy
    # conversions make the same bytes). Left out for a WiiLibs without them
    host_tools: List[Path] = []
    texture_sources = [wiilibs / "wl" / "codec" / name for name in ("xbox_tex.c", "gx_tex.c")]
    if all(source.is_file() for source in texture_sources):
        texture_library = build_dir / "tools" / "libwl_xbox_tex.so"
        n.rule(name="wii_host_library",
               command=f"cc -O2 -std=gnu11 -ffp-contract=off -shared -fPIC -I{_quote(wiilibs)} -o $out $in",
               description="WII HOST LIBRARY $out")
        n.build(outputs=texture_library, rule="wii_host_library", inputs=texture_sources,
                implicit=[wiilibs / "wl" / "codec" / "xbox_tex.h", wiilibs / "wl" / "codec" / "gx_tex.h"])
        host_tools.append(texture_library)
    # the settings file's parser (port/third_party/tomlc17)
    add_object(TOML_DIR / "tomlc17.c", " ".join([abi, "-std=gnu11", "-w"]))
    # the port's zlib, and the game's sin, pow and the rest, the same on
    # every port (port/include/halo_math.h)
    for name in ZLIB_SOURCES:
        add_object(ZLIB_DIR / name, " ".join([abi, "-std=gnu11", *ZLIB_DEFINES, "-w"]))
    for source in musl_math_sources():
        add_object(source, " ".join([abi, "-std=gnu11", "-w", f"-I{MUSL_MATH_DIR}/include",
                                     f"-include {MUSL_MATH_DIR}/include/libm.h"]))

    # the link: devkitPPC's gcc as the driver (its crt0, linker script and
    # newlib), libogc's Wii libraries, WiiLibs. The start-up code calls
    # port/wii/src/wii_boot.c's __wrap_main, which starts the console and
    # then calls the game's main (source/shell/shell_xbox.c); not a
    # constructor, as clang puts those in .init_array, which devkitPPC's
    # start-up code never runs. There must be none in clang's units.
    linker = tools / "powerpc-eabi-gcc"
    ldflags = " ".join(["-mrvl", "-mcpu=750", "-meabi", "-mhard-float", "-g", "-Wl,--gc-sections", "-Wl,--wrap=main",
                        # (every way out logged, with who took it: WiiLibs' wii/trace_wii.c)
                        "-Wl,--wrap=exit", "-Wl,--wrap=abort", "-Wl,--wrap=_exit", "-Wl,--wrap=SYS_ResetSystem",
                        "-Wl,--wrap=sched_yield",
                        # (sqrtf without newlib's loop of a bit a step: port/wii/src/wii_math.c)
                        "-Wl,--wrap=sqrtf",
                        # (the game's heap, MEM1's malloc with MEM2's behind it: WiiLibs' wii/heap_wrap_wii.c,
                        # enabled in port/wii/src/wii_boot.c; -u pulls the wrappers out of the archive even when
                        # only libc's own objects call malloc)
                        *(f"-Wl,--wrap={name}" for name in heap_overflow_wraps(wiilibs)), "-Wl,-u,__wrap_malloc",
                        f"-Wl,-Map,{build_dir / 'halo.map'}", f"-L{devkitpro / 'libogc' / 'lib' / 'wii'}"])
    libs = " ".join([*(str(library) for library in wiilibs_outputs),
                     *(f"-l{library}" for library in config.get("libraries", []))])
    stub_cflags = " ".join(wii_abi_flags(devkitpro))
    n.rule(
        name="wii_link",
        command=(f"$python {LINK_SCRIPT} --wiilibs {_quote(wiilibs)} --linker {linker} --compiler $wii_cc "
                 f"--cflags '$stub_cflags' --ldflags '$ldflags' --libs '$libs' "
                 f"--stubs {build_dir / 'wii_missing.c'} --report {build_dir / 'missing_symbols.txt'} "
                 f"--output $out @$out.rsp"),
        description="WII LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=elf, rule="wii_link", inputs=objects,
            implicit=[LINK_SCRIPT, wiilibs / "tools" / "link_stubs.py", *wiilibs_outputs],
            implicit_outputs=[build_dir / "missing_symbols.txt"],
            variables={"ldflags": ldflags, "libs": libs, "stub_cflags": stub_cflags})
    n.rule(name="wii_dol", command=f"{devkitpro / 'tools' / 'bin' / 'elf2dol'} $in $out",
           description="WII DOL $out")
    n.build(outputs=dol, rule="wii_dol", inputs=elf)
    # (clang's constructors would be silently dropped: see the link's comment;
    # and native thread-local data would be addressed from r2, which is not a
    # thread's: see wii_abi_flags)
    n.rule(name="wii_no_constructors",
           command=f"! {tools / 'powerpc-eabi-readelf'} -S $in | grep -qE 'init_array|[.]tdata|[.]tbss' && touch $out",
           description="WII CHECK no .init_array or native thread-local data in $in")
    n.build(outputs=build_dir / "no_constructors.stamp", rule="wii_no_constructors", inputs=elf)
    # (no fused multiply-adds in clang's objects, whatever -ffp-contract=off
    # was told: the lockstep check compares the Wii's game with x86's; but
    # those of CONTRACTED_SOURCES)
    fp_check = Path("tools/wii_fp_check.py")
    n.rule(name="wii_fp_check",
           command=f"$python {fp_check} --objdump {tools / 'powerpc-eabi-objdump'} --stamp $out @$out.rsp",
           description="WII CHECK no fused multiply-adds in the game's objects",
           rspfile="$out.rsp",
           rspfile_content="$in_newline")
    checked_objects = [obj for obj in objects if obj.with_suffix(".c").name not in CONTRACTED_SOURCES]
    n.build(outputs=build_dir / "no_fused_multiply_adds.stamp", rule="wii_fp_check", inputs=checked_objects,
            implicit=[fp_check])
    n.build(outputs="wii", rule="phony",
            inputs=[dol, build_dir / "no_constructors.stamp", build_dir / "no_fused_multiply_adds.stamp", *host_tools])
    # the Homebrew Channel's zip (apps/halo: boot.dol, meta.xml, icon.png; no game data)
    # (made by WiiLibs' tools/wii_package.py, which tools/wii_package.py calls)
    n.rule(name="wii_package",
           command=f"$python tools/wii_package.py --wiilibs {_quote(wiilibs)} --dol $in --output $out",
           description="WII PACKAGE $out")
    n.build(outputs=build_dir / "halo-wii.zip", rule="wii_package", inputs=dol,
            implicit=[Path("tools/wii_package.py"), PORT_DIR / "package_description.txt",
                      wiilibs / "tools" / "wii_package.py", build_dir / "no_constructors.stamp",
                      build_dir / "no_fused_multiply_adds.stamp"])
    n.build(outputs="wii-package", rule="phony", inputs=[build_dir / "halo-wii.zip"])
    n.newline()
