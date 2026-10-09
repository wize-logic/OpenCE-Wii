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
#  - frame pointers (the stack walker follows the frame chain),
#  - no fused multiply-adds (PowerPC has fmadd): every port simulates the
#    same game from the same inputs (port/include/halo_math.h),
#  - libogc's Wii (HW_RVL) declarations.
# -malign-double is not needed: the PowerPC EABI already aligns doubles and
# 64-bit integers to 8 bytes, as MSVC does.
def wii_abi_flags(devkitpro: Path) -> List[str]:
    return [
        "--target=powerpc-unknown-eabi",
        "-mcpu=750",
        "-fms-extensions",
        "-fshort-wchar",
        "-fcommon",
        "-fno-pic",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-fno-delete-null-pointer-checks",
        "-msvr4-struct-return",
        "-fno-omit-frame-pointer",
        "-ffp-contract=off",
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
    for source in game_sources(linux_config):
        add_object(source, game_cflags)
    for source in sorted(Path(linux_config["game_sources"]).glob("*.c")):
        add_object(source, game_cflags)

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
                             f"-I{linux_platform_dir}", "-Wall"])

    def platform_flags(source: Path) -> str:
        return posix_cflags if source.name.startswith("posix_") or source.name == "wii_posix.c" else platform_cflags

    for name in config["shared_platform_sources"]:
        add_object(linux_platform_dir / name, platform_flags(linux_platform_dir / name))
    for source in sorted(wii_platform_dir.glob("*.c")):
        add_object(source, platform_flags(source))
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
                        f"-Wl,-Map,{build_dir / 'halo.map'}", f"-L{devkitpro / 'libogc' / 'lib' / 'wii'}"])
    libs = " ".join([*(str(library) for library in wiilibs_outputs),
                     *(f"-l{library}" for library in config.get("libraries", []))])
    stub_cflags = " ".join(wii_abi_flags(devkitpro))
    n.rule(
        name="wii_link",
        command=(f"$python {LINK_SCRIPT} --linker {linker} --compiler $wii_cc "
                 f"--cflags '$stub_cflags' --ldflags '$ldflags' --libs '$libs' "
                 f"--stubs {build_dir / 'wii_missing.c'} --report {build_dir / 'missing_symbols.txt'} "
                 f"--output $out @$out.rsp"),
        description="WII LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=elf, rule="wii_link", inputs=objects, implicit=[LINK_SCRIPT, *wiilibs_outputs],
            implicit_outputs=[build_dir / "missing_symbols.txt"],
            variables={"ldflags": ldflags, "libs": libs, "stub_cflags": stub_cflags})
    n.rule(name="wii_dol", command=f"{devkitpro / 'tools' / 'bin' / 'elf2dol'} $in $out",
           description="WII DOL $out")
    n.build(outputs=dol, rule="wii_dol", inputs=elf)
    # (clang's constructors would be silently dropped: see the link's comment)
    n.rule(name="wii_no_constructors",
           command=f"! {tools / 'powerpc-eabi-readelf'} -S $in | grep -q init_array && touch $out",
           description="WII CHECK no .init_array in $in")
    n.build(outputs=build_dir / "no_constructors.stamp", rule="wii_no_constructors", inputs=elf)
    n.build(outputs="wii", rule="phony", inputs=[dol, build_dir / "no_constructors.stamp"])
    n.newline()
