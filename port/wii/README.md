# Halo: Combat Evolved for the Wii

This port is in bring-up. The game compiles, links and boots in Dolphin: it claims its memory,
starts WiiLibs, mounts the SD card on IOS 58 and enters the game's `main`. It stops at the first
platform function the port does not provide yet, which is currently `Direct3DCreate8`.

## Building

You need devkitPro with devkitPPC and libogc (`/opt/devkitpro`, or `DEVKITPRO`), clang with the
PowerPC target, and a checkout of [WiiLibs](../../../WiiLibs) beside this repository (or
`configure.py --wiilibs DIR`).

```sh
python3 configure.py
ninja wii                    # build/wii/halo.dol
tools/wii_dolphin.py --until "does not provide"   # boot it in Dolphin from WSL
```

`tools/wii_dolphin.py` runs WiiLibs' Dolphin runner (`tools/dolphin_run.py` there) on the build.
It uses the Windows Dolphin (`WL_DOLPHIN`, else `Documents\Emulators\Dolphin-x64`) with a user
folder of its own, so your own Dolphin settings stay as they are. `build/wii/sd` is synced in as
the SD card. It saves a capture of the render window to `build/wii/dolphin.png` and prints the
game's log (WiiLibs' sink, Dolphin's OSREPORT).

## Game data

The game reads its data from `apps/halo` on the SD card, or on a USB drive when the card has no
`maps` folder there (`platform_data_root`, `port/linux/src/xbox_files.c`). Saves go to
`apps/halo/saves`. The maps must be the Xbox's (cache version 5), converted for the Wii. Halo PC's
and Custom Edition's maps don't run. The converter is not written yet.

## How the port operates

- **Compiler.** The game is compiled with clang `--target=powerpc-unknown-eabi -mcpu=750` against
  devkitPPC's newlib and libogc, then linked by devkitPPC's gcc (`tools/wii_build.py`).
  devkitPPC's own gcc cannot compile the game's MSVC dialect, while clang already compiles it for
  the other ports.
- **Flags.** The ABI flags reproduce the MSVC/Xbox environment, as the Linux build's do:
  - `-msvr4-struct-return` is PowerPC's spelling of `-freg-struct-return`
  - `-ffp-contract=off` because PowerPC has fused multiply-adds
- **Prefix header.** `include/halo_wii_prefix.h` is force-included and wraps the Linux one. Its
  only change is a 16-bit `wint_t` ahead of newlib's.
- **Start-up.** The link wraps `main`: `src/wii_boot.c`'s `__wrap_main` claims the fixed memory,
  starts WiiLibs (system buttons, log, heaps, IOS 58, SD and USB) and a text console, then calls
  the game's `main` (`source/shell/shell_xbox.c`). Clang puts constructors in `.init_array`,
  which devkitPPC's start-up code never runs, so the port has none. The build checks that.
- **Platform layer.** `port.json` lists the shared platform files (`port/linux/src`) the Wii
  keeps, and `src/` holds its own. `tools/wii_link.py` stubs every symbol still missing with a
  function that stops the game and names the symbol. The list is
  `build/wii/missing_symbols.txt`, which is what the port still lacks.
- **Memory.** The Wii's cached memory is at virtual 0x80000000 + P, as the Xbox's is, so the
  game's and Direct3D's physical/virtual arithmetic holds. `src/wii_memory.c` claims two fixed
  ranges from WiiLibs (`wl_platform_mem_claim_fixed`) before anything else runs:

  | Range | Holds |
  | --- | --- |
  | MEM1 `0x813C0000`–`0x81800000` | the game state (4 MB CPU, 256 KB GPU) |
  | MEM2 `0x90100000`–`0x92F00000` | the tag cache at its bottom (22 MB, which converted maps are linked to), then the 16 MB texture cache, the 4 MB sound cache and Direct3D's resources, top-down |

  WiiLibs' heaps (`src/wii_boot.c`) get what is left of each region. The game's pools are the
  Xbox's sizes (`HALO_WII` in `port/linux/include/halo_port_capacity.h`), so the Wii can't join
  the other builds' distributed games.
- **Floating point.** `_controlfp`/`_statusfp` map the MSVC control word onto the FPSCR
  (`port/linux/src/msvc_crt.c`).

## Map conversion (planned)

The Xbox maps are little-endian memory images whose pointers point into `0x803A6000`. The Wii
build's tag structures are laid out exactly as the i386 build's: all 161 that the tag schema and
validator use match, field for field, in the two builds' DWARF. None of them use C bitfields. So
converting a map is a byteswap of every field, which the DWARF types say how to swap, plus a
move of every pointer by one constant. The walk follows the existing tag schema
(`port/linux/game/tag_schema*.c`). Textures (DXT to CMPR), vertices and sounds are converted on
their own.

## What is next

1. A Direct3D 8 device for the Wii (`d3d8_gx.c`), drawing nothing at first, so the game can go
   past start-up. Resources already come from the shared `d3d8_resources.c` and
   `xbox_texture_formats.c`.
2. The map converter, as above, with the maps validated by `tag_validate` after conversion.
3. The rest of the platform layer: input (XInput over WiiLibs' controllers), sound output (the
   shared mixer into WiiLibs' audio stream) and networking (libogc's sockets).
4. GX drawing: the register combiners translated to TEV stages, and the 67 vertex shaders
   mapped to GX transforms one by one.
