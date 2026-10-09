# Halo: Combat Evolved for the Wii

This port is in bring-up. In Dolphin, the game boots from the SD card, loads its converted maps and
runs its main menu. The menu is drawn with its textures, its 3D background and its music, at a few
frames a second. It hasn't been tried on a Wii yet, and no level has been played.

## Building

You need:

- devkitPro with devkitPPC and libogc (`/opt/devkitpro`, or `DEVKITPRO`)
- clang with the PowerPC target
- Python 3 with pyelftools and numpy (for the map converter)
- a checkout of [WiiLibs](../../../WiiLibs) beside this repository (or `configure.py --wiilibs DIR`)

```sh
python3 configure.py
ninja wii                                   # build/wii/halo.dol and halo.elf
tools/wii_convert_maps.py XBOX/maps/ui.map XBOX/maps/bloodgulch.map   # into build/wii/sd/apps/halo/maps
tools/wii_dolphin.py --seconds 300 --until "frame 360:"            # run it in Dolphin from WSL
tools/wii_gx_psh_check.py                   # the combiner translation, checked on the host
```

`tools/wii_dolphin.py` runs WiiLibs' Dolphin runner (`tools/dolphin_run.py` there) on the build:

- It uses the Windows Dolphin (`WL_DOLPHIN`, else `Documents\Emulators\Dolphin-x64`) with a user
  folder of its own, so your own Dolphin settings stay as they are.
- `build/wii/sd` is synced in as the SD card.
- It prints the game's log (WiiLibs' sink, Dolphin's OSREPORT) and saves a capture of the render
  window to `build/wii/dolphin.png`.
- `--backtrace build/wii/halo.elf` attaches gdb through Dolphin's stub and prints every thread's
  stack when the run ends.

## Game data

The game reads its data from `apps/halo` on the SD card, or from a USB drive when the card has no
`maps` folder there (`platform_data_root`, `port/linux/src/xbox_files.c`). Saves go to
`apps/halo/saves`, and the settings to `apps/halo/config.toml`.

The maps must be the Xbox's (cache version 5), converted by `tools/wii_convert_maps.py`. Halo PC's
and Custom Edition's maps don't run. A converted map is uncompressed, about the size the Xbox
inflated it to, and the Wii reads it where it is.

## How the port operates

- **Compiler.** The game is compiled with clang `--target=powerpc-unknown-eabi -mcpu=750` against
  devkitPPC's newlib and libogc, then linked by devkitPPC's gcc (`tools/wii_build.py`).
  devkitPPC's own gcc cannot compile the game's MSVC dialect, while clang already compiles it for
  the other ports.
- **Flags.** The ABI flags reproduce the MSVC/Xbox environment, as the Linux build's do:
  - `-msvr4-struct-return` is PowerPC's spelling of `-freg-struct-return`
  - `-ffp-contract=off` because PowerPC has fused multiply-adds
  - `-femulated-tls`: native thread-local storage would use r2, which the EABI gives the small
    data area. WiiLibs provides `__emutls_get_address` for each libogc thread.
- **Prefix header.** `include/halo_wii_prefix.h` is force-included and wraps the Linux one. Its
  only change is a 16-bit `wint_t` ahead of newlib's.
- **Start-up.** The link wraps `main`. `src/wii_boot.c`'s `__wrap_main` does the following, then
  calls the game's `main` (`source/shell/shell_xbox.c`):
  - claims the fixed memory
  - starts WiiLibs: system buttons, log, crash report, heaps, IOS 58, SD and USB
  - starts the network in the background

  Clang puts constructors in `.init_array`, which devkitPPC's start-up code never runs, so the
  port has none. The build checks that.
- **Platform layer.** `port.json` lists the shared platform files (`port/linux/src`) the Wii
  keeps. These include the Xbox kernel's threads and events, the files, Direct3D's state and
  resources, the DirectSound mixer and the Winsock layer. `src/` holds the Wii's own:

  | File | What it does |
  | --- | --- |
  | `wii_platform.c` | log, messages and quitting |
  | `wii_posix.c` | POSIX threads, files and time over newlib and libogc |
  | `xinput_wii.c` | XInput over the Wii Remote with Nunchuk or Classic Controller, and the GameCube controller |
  | `posix_net_wii.c` | sockets over IOS, for system link |
  | `d3d8_gx.c` | the Direct3D device |
  | `wii_extras.c` | the desktop additions the Wii goes without |

  `tools/wii_link.py` stubs any symbol still missing with a function that stops the game and names
  it (`build/wii/missing_symbols.txt`).
- **Memory.** The Wii's cached memory is at virtual 0x80000000 + P, as the Xbox's is, so the
  game's and Direct3D's physical/virtual arithmetic holds. `src/wii_memory.c` claims two fixed
  ranges from WiiLibs (`wl_platform_mem_claim_fixed`) before anything else runs:

  | Range | Holds |
  | --- | --- |
  | MEM1 `0x813C0000`–`0x81800000` | the game state (4 MB CPU, 256 KB GPU) |
  | MEM2 `0x90100000`–`0x92D00000` | the tag cache at its bottom (22 MB, which converted maps are linked to), then the 12 MB texture cache, the 4 MB sound cache and Direct3D's resources |

  WiiLibs' heaps (`src/wii_boot.c`) are in MEM2. The game's own heap is newlib's, in what MEM1
  leaves (about 4 MB). `src/wii_malloc.c` wraps the allocators, newlib's internal ones included,
  so that what MEM1 cannot hold comes from a 5 MB heap in MEM2. The game's pools are the Xbox's
  sizes (`HALO_WII` in `port/linux/include/halo_port_capacity.h`), so the Wii can't join the
  other builds' distributed games.
- **Threads.** libogc's threads are strictly prioritised. Each of the game's threads gets the
  main thread's priority, moved by its Win32 priority, and a 128 KB stack.
- **Big-endian fixes.** The game core is the same code as on the other ports, with a few changes
  for big-endian:
  - the script engine's value cells (`port/linux/include/halo_cells.h`)
  - the halves of `LARGE_INTEGER`
  - the recorded animations' bitfield header byte (rewritten by the converter)
- **Floating point.** `_controlfp`/`_statusfp` map the MSVC control word onto the FPSCR
  (`port/linux/src/msvc_crt.c`).
- **Movies.** Bink movies are skipped (`source/bink/bink_playback.c`).

## Map conversion

`tools/wii_convert_maps.py` reads the tag layouts from `build/wii/halo.elf` (the DWARF types and
the linked tag schema, `tools/wii_tag_layout.py`). The Wii build's tag structures are laid out
exactly as the i386 build's, and none use C bitfields. A map is converted as follows:

- **Tags.** Every field of every tag is byteswapped, its pointers into the tag cache moved from
  0x803A6000 to 0x90100000. The data blocks are converted by what they hold:
  - script syntax nodes
  - animations' frames
  - recorded animations' event streams
  - the BSPs' vertices and the vertex and index buffers
- **Header.** The header gets the Wii's version (`CACHE_FILE_VERSION_WII`, 0x57490005), so the
  game refuses an unconverted map.
- **Bitmaps.** Bitmaps are rewritten in place, in the same bytes, in GX's formats
  (`tools/wii_gx_textures.py`):

  | Xbox | GX |
  | --- | --- |
  | DXT1 | CMPR, the same blocks in GX's order |
  | DXT3, DXT5 | CMPR colour, then I4 alpha |
  | A8R8G8B8, X8R8G8B8 | RGBA8 |
  | R5G6B5 | RGB565 |
  | A1R5G5B5, A4R4G4B4 | RGB5A3 |
  | L8, A8, AL8 | I8 |
  | A8L8 | IA8 |

  Mip levels are kept while GX's tiles fit in the Xbox's bytes. Cube maps, 3D textures and
  palettized bitmaps stay as they are, and the device decodes them.
- **Pixel offsets.** The Xbox's maps keep a stale tag-file offset in each bitmap group's pixel
  data, which the texture cache would add to every bitmap's own offset. The converter sets it to
  0.

The converter reports what it doesn't reach for each map.

## Drawing

`src/d3d8_gx.c` is a Direct3D 8 device on GX. It keeps the game's own rasterizer and its vertex
and pixel shaders.

- **Vertex shaders.** GX has no programmable vertex stage, so the game's vertex programs run on
  the CPU (`src/nv2a_cpu.c`, an NV2A interpreter), vertex by vertex. GX then draws the results:
  - A program that divides by its clip-space w is sent as (x·w, y·w, −w) through a perspective
    projection. GX then clips at the eye and interpolates textures with perspective.
  - The depth is fitted per draw as a + b/w from a few of its vertices. This is exact for the
    game's projections.
  - Draws whose w doesn't vary, such as the interface's, are drawn in screen space.
- **Pixel shaders.** The register combiners are translated to TEV stages (`src/gx_psh.c`).
  - Combiner registers are renamed as they are written, and dead values are dropped.
  - Inputs a stage can't reach (a second texture or rasterized colour, a second constant, a
    doubly inverted product) are first computed into a free register.
  - Colour and alpha share stages where their textures agree.
  - GX's inputs are unsigned 8-bit, so signed mappings are clamped. Dot products (bump mapping)
    read as 1, and fog isn't applied yet.
  - `tools/wii_gx_psh_check.py` simulates the TEV stages against the combiners, for random
    programs and the game's.
- **Textures.** A map's bitmap is bound straight from the texture cache in its GX format. Any
  other texture is decoded to RGBA8 into a 1.5 MB cache, such as the glyph cache, which is
  re-decoded when its pixels change.
- **Frame buffer.** The EFB has an alpha channel (RGBA6_Z24), because the game blends by
  destination alpha.
- **Debugging.** The desktop ports' `debug.gpu_debug_flat` and `debug.gpu_debug_texture0` (in
  `config.toml`) show the diffuse colour, or the first texture, alone. One frame's draws are
  described in the log (`DRAW_LOG_FRAME`).

## What is next

1. The rest of the drawing:
   - render targets (EFB copies to textures) and clears in the middle of a frame
   - culling
   - fog
   - cube maps
   - the Z-buffer's precision with the fitted depth
2. Speed: the vertex programs run on the CPU and the menu runs at a few frames a second. Next
   are display lists for the static geometry, paired singles, and GX's own transform for the
   simplest programs. The target is 30 frames a second.
3. Playing a level (Blood Gulch, then a10), and the game core's endianness checked under qemu
   against the x86 build.
4. Real hardware, then packaging for the Homebrew Channel (`meta.xml`, an icon) and a CI job.
