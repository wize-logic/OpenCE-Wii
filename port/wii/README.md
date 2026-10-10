# Halo: Combat Evolved for the Wii

This port is in bring-up. In Dolphin, the game boots from the SD card and loads its converted maps.
- **The main menu** is drawn as on the Xbox (the Pillar of Autumn passing the planet, the logo, the
  menu), with its music, at 30 frames a second (emulated time).
- **Blood Gulch** loads and is drawn (`debug.network_test = "solo:bloodgulch"`): the canyon, its
  terrain with lightmaps and detail maps, the sky, fog, the HUD and the first-person weapon. The
  scripted player (`debug.test_input = "bot:1"`) moves, jumps and throws grenades in it.
- **The campaign** starts: `debug.network_test = "solo:levels\\a10\\a10"` loads the Pillar of
  Autumn in about 11 seconds, plays its opening (the cryo bay, the look-training prompts), and
  survives a cinematic's skip, which reverts to the saved game. Ten minutes of it ran without a
  fault.
- **Every campaign level** loads and plays with the scripted player, in Dolphin, without a
  fault: a10, a30, a50, b30, b40, c10, c20, c40, d20 and d40 (a tour:
  `debug.network_test = "solo:levels\\a30\\a30,levels\\b30\\b30"` with
  `debug.network_test_tour` seconds of game time in each). The outdoor levels run at about two
  frames a second in Dolphin: their skinned characters' vertices are the CPU's. On the Wii,
  `display.object_shadows` is off by default (each object's shadow draws it again, a pass of
  skinned vertices) and `display.model_detail` is 0.5.
- **Loading**: the menu is up within a second of the game's start.
- **Every map converts**: the 24 Xbox maps (campaign and multiplayer) go through
  `tools/wii_convert_maps.py` with nothing left unconverted.

The port hasn't run on a Wii yet, and the levels haven't been played through.

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
tools/wii_nv2a_check.py                     # the compiled vertex programs against the interpreter
ninja wii-package                           # build/wii/halo-wii.zip for the Homebrew Channel
```

`ninja wii-package` zips `apps/halo` as the Homebrew Channel reads it: `boot.dol`, `meta.xml`
and an icon drawn by `tools/wii_package.py`. It holds no game data. The `wii` job in
`.github/workflows/build.yml` runs both host checks, builds the zip in devkitPro's devkitPPC
image (with WiiLibs checked out beside the repository) and keeps it as an artifact.

`tools/wii_dolphin.py` runs WiiLibs' Dolphin runner (`tools/dolphin_run.py` there) on the build:

- It uses the Windows Dolphin (`WL_DOLPHIN`, else `Documents\Emulators\Dolphin-x64`) with a user
  folder of its own, so your own Dolphin settings stay as they are.
- `build/wii/sd` is synced in as the SD card.
- It prints the game's log (WiiLibs' sink, Dolphin's OSREPORT) and saves a capture of the render
  window to `build/wii/dolphin.png`.
- `--backtrace build/wii/halo.elf` attaches gdb through Dolphin's stub and prints every thread's
  stack when the run ends.
- It runs Dolphin on Vulkan, single-core by default (`--dual-core` for speed). Dolphin's
  Direct3D 11 backend has exited without a word on the game's longer TEV programs. Earlier
  "exits" in dual-core mode were another program's test runner killing every Dolphin; WiiLibs'
  runner now acts only on its own (its `--name` user folder).
- The game's log (`src/d3d8_gx.c`, not the runner) reports how long a frame takes in emulated time
  every 60 frames, split into draws, textures, pixel shaders, the end of the frame and the game,
  with the draws' split (GX's transform, the processor's, immediate) and the processor's
  vertices (fetch, program, store, emit). A frame of more than half a second (loading, mostly)
  gets a split of its own.

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
  game's main priority (WiiLibs' `WL_THREAD_PRIO_MAIN`, LWP 64: `WII_GAME_MAIN_PRIO` in
  `src/wii_posix.c`), moved by its Win32 priority, and a 128 KB stack. The game's main runs on a
  thread of its own with a 1 MB stack, started by WiiLibs' `wl_platform_run_on_stack`
  (`src/wii_boot.c`): libogc's main thread has 128 KB, below which lie its audio DSP structures,
  and a deep AI path search in b30 went past it (the game's stack reaches about 290 KB; the memory
  report says how deep, from `wl_platform_stack_used`, which measures the pattern WiiLibs fills the
  stack with). devkitPPC's `sched_yield` is a stub, so the link wraps it (`-Wl,--wrap=sched_yield`,
  `tools/wii_build.py`) to WiiLibs' `__wrap_sched_yield`, libogc's yield: the game's waits spin on
  `SwitchToThread` for its I/O thread, which otherwise never ran (the menu took 45 seconds to
  appear, a10 100 seconds to load).
- **Big-endian fixes.** The game core is the same code as on the other ports, with a few changes
  for big-endian:
  - the script engine's value cells (`port/linux/include/halo_cells.h`)
  - the halves of `LARGE_INTEGER`
  - the recorded animations' bitfield header byte (rewritten by the converter)
  - the compressed animations' floats, read a byte at a time (the 750 can trap on a misaligned
    float load)
- **Floating point.** `_controlfp`/`_statusfp` map the MSVC control word onto the FPSCR
  (`port/linux/src/msvc_crt.c`).
- **Movies.** Bink movies are skipped (`source/bink/bink_playback.c`).
- **Sound.** The shared DirectSound mixer (`port/linux/src/dsound_sdl.c`) fills WiiLibs' audio
  stream (AESND, 32 kHz). Every 240 frames the log reports blocks mixed and played, underruns,
  the time a block takes to mix, and the loudest sample. In the menu: no underruns, the music
  near full scale, about 0.6 ms to mix a block.

## Map conversion

`tools/wii_convert_maps.py` reads the tag layouts from `build/wii/halo.elf` (the DWARF types and
the linked tag schema, `tools/wii_tag_layout.py`). The Wii build's tag structures are laid out
exactly as the i386 build's, and none use C bitfields. A map is converted as follows:

- **Tags.** Every field of every tag is byteswapped, its pointers into the tag cache moved from
  0x803A6000 to 0x90100000. The schema's structures stand byte arrays (`reserved...`,
  `unused...`) for fields its validator doesn't check. Where the game's own declarations of the
  tag's structure name fields there, those are swapped too (`fill_placeholders` in
  `tools/wii_tag_layout.py`); the sky's fog colours are an example. The data blocks are converted
  by what they hold:
  - script syntax nodes
  - animations' frames
  - recorded animations' event streams
  - the BSPs' vertices and the vertex and index buffers
  - the clusters' visible sets, bit vectors the game reads a long at a time
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
  | P8 (bump maps' normals) | CI8, the palette loaded as a TLUT |
  | a cube map (any of these) | a sphere map in RGB565 or RGB5A3, with a trailer the device finds |

  Mip levels are kept while GX's tiles fit in the Xbox's bytes. A cube map's sphere map uses the
  mirror ball's mapping, sampled from the six faces' first levels; only the direction straight
  behind has no place on it. 3D textures stay as they are, and the device decodes them.
- **Pixel offsets.** The Xbox's maps keep a stale tag-file offset in each bitmap group's pixel
  data, which the texture cache would add to every bitmap's own offset. The converter sets it to
  0.

The converter reports what it doesn't reach for each map.

`tools/wii_animation_check.py --converted DIR map.map...` reads every animation of the Xbox's maps
and of the converted ones as `source/models/model_animations.c` does (the frame info, the default
data and frames, and the compressed data's header, node headers, keyframe frame indices,
keyframes and defaults) and compares the values element by element.

`tools/wii_data_check.py --converted DIR map.map...` does the same for the other data blocks, each
read as the game reads it: the script syntax nodes (a constant's value by its type's size from the
cell's low-order end, a call's expression index whole), the recorded animations' streams (the
unit control, the events with their bitfield headers), the clusters' visible sets (a long at a
time), the BSP materials' vertices (as the processor reads them), every vertex buffer by its
shader declaration's elements, the model parts' indices (which must name their part's vertices),
the sounds (all of the maps' samples are Xbox ADPCM, whose decoder reads its headers a byte at a
time; they are left as they are), and the bitmaps: each texture the device binds as GX's, decoded
texel by texel from the Xbox's swizzled or DXT texels and from GX's tiles, and each cube map's
sphere map against the cube map in the direction `sphere_coordinates` gives each texel. On the 24
maps nothing differs.

## Drawing

`src/d3d8_gx.c` is a Direct3D 8 device on GX. It keeps the game's own rasterizer and its vertex
and pixel shaders.

- **Vertex shaders.** GX has no programmable vertex stage. A program GX's fixed transform can
  compute goes to it; the others run on the CPU, vertex by vertex.
  - `nv2a_cpu_analyze` walks each program with every value a set of the inputs it depends on and
    whether it is still affine in them (the generated programs carry the result as a comment).
  - An affine program is, for one draw's constants, a matrix. Its columns come from running it for
    no input and for each input component alone. GX then reads the game's vertex buffers by index:
    the position through the position matrix, each texture coordinate the TEV stages read through
    a texture matrix (on the position, two floats or normalized shorts), the fog through a matrix
    into its ramp, and constant colours as the channels' material colour.
  - The others stay on the CPU: skinned models (a weight times a matrix), lit ones (a reciprocal
    square root), and colours from the vertices (Direct3D's ARGB is a byte order GX doesn't read).
    Even so, any draw whose position is affine goes through the same position matrix, so a
    surface's passes on either path reach the same depths, as their equal-depth tests need.
  - A skinned model's draw is keyed by what its vertices' node indices and weights make
    (`node_keys`). A vertex of weight 1 or 0 hangs off one node, whatever its other index: the
    game's programs give it what that node alone gives (`tests/nv2a_native_check.c` checks each
    program). A blended vertex is its two nodes and its weight. Up to nine values a draw get a
    position matrix each, which GX picks per vertex by its matrix index. The other vertices (of a
    value with fewer than six of the draw's indices, or past the nine) are the CPU's: their
    position is sent in the same draw, through the identity and the same projection, and every
    pass of the model makes the same choice, as it depends on the vertices alone.
  - In the model parts of eleven maps (the campaign and Blood Gulch), GX now computes the position
    of 89 to 96% of the vertices, where it computed 49 to 80% (a draw of more than nine values was
    all the CPU's): b30 60% to 93%, c10 49% to 91%. 2 to 14% of them blend two nodes.
  - Where GX computes the position, the CPU's program for the rest of the vertex skips it: about
    70 instructions a vertex in the lit skinned programs, 235 in those that compute one texture
    coordinate. A matrix's columns run the program for the position alone (440 instructions in
    place of 670 for `cfc6f3a4`), a model's other nodes without the depth's fit, and the constants
    a program reads are hashed once while none is set, where each node's matrix hashed them again
    (about 10,000 instructions for a skinned program, which reads all 192).
  - In a10, 4.0 of 6.5 million vertices a minute go to GX. The CPU runs 1.6 million programs where
    it ran 7.9 million, and a frame takes about 160 ms in Dolphin (it took 200 to 450).
    `debug.gpu_transform = false` keeps every program on the CPU.
  - An indexed draw on the CPU runs each vertex's program once, however many indices name it.
  - A draw on the CPU plans its vertex elements once: only the registers its program reads, each
    type with its own case, bytes and packed normals converted by table. Each vertex then computes
    only what the stages read: the program skips instructions that reach no output sent
    (`nv2a_cpu_run_live`), and the texture scale, sphere map and fog table apply only where set.
  - At build time, `tools/nv2a_codegen.c` decodes the game's 67 programs with the interpreter's own
    decoder (`src/nv2a_cpu.c`) and writes each one as a C function (about 180 KB of code). Each takes the
    outputs a draw reads, and runs an instruction only if it reaches one of them (found backwards
    from the end, a register's component at a time). A program is found by a hash of its words.
  - Square roots and ARL's floor give the libraries' results without their calls
    (`nv2a_sqrtf`: frsqrte refined in double, rounded, then corrected by the midpoints' exact
    squares). A skinned vertex takes about 1,600 instructions where it took 3,400.
  - Any other program runs on the interpreter.
  - In the menu, the draws take 10 ms a frame compiled, against 58 ms interpreted.

  GX then draws the results:
  - A program that divides by its clip-space w is sent as (x·w, y·w, −w) through a perspective
    projection. GX then clips at the eye and interpolates textures with perspective.
  - The depth is fitted per draw as a + b/w from a few of its vertices. This is exact for the
    game's projections.
  - Draws whose w doesn't vary, such as the interface's, are drawn in screen space.
- **Pixel shaders.** The register combiners are translated to TEV stages (`src/gx_psh.c`).
  - Combiner registers are renamed as they are written, and dead values are dropped.
  - Inputs a stage can't reach (a second texture or rasterized colour, a second constant, a
    doubly inverted product) are first computed into a free register, or into the operation's
    own destination.
  - Colour and alpha share stages where their textures agree.
  - Dot products of a texture, rasterized colour or constant with another take three stages
    (fewer when the constant has zero channels). Swap tables put one channel in all three, as do
    the konst selections for constants.
  - Signed negation becomes TEV's subtraction. The other signed mappings are clamped to [0, 1],
    as TEV's registers are, except in a dot product.
  - A dot product of signed values (expanded or half-biased: a bump map against the
    normalization cube map's direction, or against a constant) is made exactly, a channel at a
    time, by TEV's lerp with its bias of -1/2 and its scale: (x - 1/2)(y - 1/2) is
    (lerp(1 - x, x, y) - 1/2) / 2, and (x - 1/2)y is lerp(1/2, x, y) - 1/2.
    - The sum stays signed between its stages (unclamped: TEV keeps -4 to 4 for a d input); the
      last clamps it, so a negative one is 0.
    - In the colour half it takes two stages a channel (1 - x and x as one stage's colour and
      alpha, then the term). Where the alpha half reads its blue (the lightmap's bump term, a
      reflection's Fresnel), it takes three in the alpha half, and the colour reads that
      register's alpha, as it does products of it.
    - A dot product of a register's colour is still taken as 1 (no swap reaches its channels).
  - (1 - x)p + xq is TEV's lerp, one stage.
  - Constants read as 0 or 1 need no register. Those needed in every channel take the four konst
    registers first, and any beyond four are set in TEV registers kept for them.
  - The fog register reads the vertex's fog factor, sent as a fifth texture coordinate into a
    ramp texture whose intensity is its coordinate, and `D3DRS_FOGCOLOR` as a constant.
  - A program that doesn't fit GX's 16 stages and 4 registers is translated again with these
    simplified, and each approximation says why. One that still doesn't fit draws its first
    texture times its diffuse colour.
  - `tools/wii_gx_psh_check.py` simulates the TEV stages against the combiners, for random
    programs and the game's.
- **Culling.** `D3DRS_CULLMODE` names the screen winding to discard. Both vertex paths keep the
  screen's winding. GX's own convention is measured once at start-up: the device draws a
  clockwise triangle with `GX_CULL_FRONT` and reads the pixel back.
- **Render targets.** A draw into another surface takes over the EFB:
  - The back buffer's colour and depth are parked in MEM2.
  - The target is drawn at the EFB's origin and copied into its own memory in GX's format, where
    its texture is bound from.
  - The back buffer returns through a quad that writes the parked colour, and the parked depth
    through a Z texture.
  - A clear once the frame has begun is a quad.
- **Textures.** A map's bitmap is bound straight from the texture cache in its GX format. Any
  other texture is decoded to RGBA8 into a 1.5 MB cache, such as the glyph cache, which is
  re-decoded when its pixels change. A cube map is bound as its sphere map, and each vertex's
  direction is mapped onto it (on the CPU): the normalization cube map's too, whose texel is the
  direction a bump map's dot product takes.
- **Depth.** A program's position matrix and its depth's a + b/w come from its constants alone,
  from two points along w's gradient. Both vertex paths take them, so a surface's passes meet
  their equal-depth tests whichever path each pass took.
- **Frame buffer.** The EFB has an alpha channel (RGBA6_Z24), because the game blends by
  destination alpha.
- **Debugging.** The desktop ports' `debug.gpu_debug_flat` and `debug.gpu_debug_texture0` (in
  `config.toml`) show the diffuse colour, or the first texture, alone. One frame's draws are
  described in the log (`DRAW_LOG_FRAME`).

## The lockstep check

The same session, played by the x86 Linux build and by the Wii in Dolphin, compared tick by tick:
the big-endian game core against the little-endian one, with the maps the Wii's converter wrote.

- Both builds run a solo network test with `debug.fixed_ticks = true` (a game tick each frame, and
  the scripted player's clock the game's ticks, so both see the same input at the same tick) and
  `debug.lockstep_log = 10`. Every 10 ticks they log a hash of every object's position, velocity,
  facing, vitality and parent, and the game's random seed (`port/linux/game/network_test.c`).
- The Linux build runs headless (`debug.null_renderer`, SDL's dummy video and audio drivers) in the
  CI's Arch Linux image, with the Xbox maps. Both builds use musl's maths functions, so the floats
  agree to the bit.
- `tools/lockstep_compare.py linux.log wii.log` names the first tick that differs. With
  `debug.lockstep_detail = <tick>` both list every object's values (and its nodes' matrices,
  hashed) at that tick, and the tool names the objects that differ.

It found `list_get`'s element index read from the wrong end of its cell, which made a10's opening
teleport player 0 to player 1's flag. With that fixed, a10's opening (the cryo bay, the cinematic and
its skip, the scripted player) agrees at every tick of 2,600.

## What only a real Wii can show

Dolphin runs the game, but these need the console:
- **Speed.** Dolphin's emulated timing does not follow this code's cost (a
  short function can measure several times its instruction count); a
  frame's time, and where it goes (the log's "a frame's draws" lines), on
  the console.
- **Alignment exceptions.** The PowerPC 750 can trap a floating-point load
  from an address that is not a multiple of 4; Dolphin does not. The
  compressed animations' floats are read a byte at a time for this; any
  other crash report ("CRASH: alignment exception") names the place.
- **The square root's estimate.** The vertex programs' square root starts
  from `frsqrte`, checked on the host for estimates as coarse as the 750's
  documented 1/32; Broadway's own is not documented.
- **The SD card and USB drive's speed** (a level loads in about 11 seconds
  in Dolphin) and **the controllers** (the Classic Controller, the
  GameCube pad, the Wiimote and Nunchuk mappings in `xinput_wii.c`).
- **Memory.** MEM1 and MEM2 as the console's IOS leaves them, with the
  Homebrew Channel's loader (the memory report in the log).

## What is next

1. Speed. A skinned model's vertices go to GX through its matrix palette by node, or by blend
   where a blend has enough vertices; the CPU computes the position of the rest (the blended
   ones, 2 to 14%), and `display.model_detail` (0.5 on the Wii) chooses simpler models sooner.
   A pass whose stages read a cube map's direction or a texture coordinate from the nodes is
   still the CPU's, vertex by vertex, less the position. Next: those passes' cost (the fetch, the
   program, the sphere map and the emission per vertex), and a profile on real hardware, where
   Dolphin's timing may not hold.
2. The rest of the drawing. Bump mapping's dot products of textures and constants are exact
   (the lightmap's bump term, a light's diffuse, the flat cube map's reflection). Still taken as
   1: the bumped specular's (dot products of registers, signed sums of products), the
   self-illuminated lightmap's (more than 16 stages), a light's alpha test's (a register's), and
   the bumped reflections' dependent lookups (indirect texturing where it maps). Sampling the
   cube maps put the environment's lit pass (`10c30c53`, whose cube map direction comes from the
   tangent frame) back on the CPU: a10's frame takes about 200 ms in Dolphin (150 without them).
3. Playing on. The campaign beyond a10's opening, and the lockstep check over longer sessions and
   more levels. Big-endian faults found so far:
   - plain char's sign (fixed with `-fsigned-char`);
   - tag fields the schema left as placeholders, one of them a contrail's shader that the game
     reads through another structure;
   - the clusters' visible sets;
   - a recorded animation's event header (bitfields);
   - the script engine's narrow values in 32-bit cells (`HALO_CELL_AT`);
   - a material type read as a long's low word;
   - the network message size and an AI vocalization type;
   - `list_get`'s element index (the lockstep check found it);
   - a data array's first salt, copied from the array's name as a short (`source/memory/data.c`),
     which changed every identifier and what the game computes from them (a needle's wander, a
     tree's frame; the lockstep check found it in b30);
   - an object's damage flags, a word read as a byte (`actor_perception.c`,
     `device_machines.c`), and `device_group_get`'s argument (`hs.c`): a dead target read as
     alive, a corpse opening doors;
   - the compressed animations' sections, swapped by the layout the decoder reads
     (`tools/wii_animation_check.py` compares every animation).
4. Real hardware, from the Homebrew Channel (`ninja wii-package`).
5. CI. The workflow's `wii` job passes when run locally in the devkitpro/devkitppc image. On
   GitHub it builds against WiiLibs' `dolphin-tests-and-fixed-claims` branch, which needs this
   port's WiiLibs changes pushed first.
