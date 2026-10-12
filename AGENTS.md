# Notes for agents

This repository ports the Halo: Combat Evolved decompilation (Xbox build
01.01.14.2342, `cachebeta.exe`) to Linux, Windows and Android. The game's C
sources are the decompilation; the port adds a platform layer that
implements the Xbox APIs the game calls, and features the Xbox game did not
have (a new netcode, internet play, the PC version's menus, Custom Edition
maps, touch controls). The project is published as OpenCE.

Start with [README.md](README.md). The other documents are listed at the
end.

## Layout

| Path | Contents |
| --- | --- |
| `source/` | The game: 466 C files of the decompilation, compiled as they are with few changes (each marked, below) |
| `port/linux/src/` | The platform layer the three ports share: Direct3D 8 on OpenGL (`d3d8_gl.c`), DirectSound and XInput on SDL3, files, threads, memory, sockets, settings (`port_config.c`), internet play (`p2p*.c`) |
| `port/linux/game/` | The port's game-side code, compiled as the game's sources are: the distributed netcode (`network_*.c`), co-op, the menus (`menu_tags.c`, `menu_functions.c`), Custom Edition maps (`custom_edition_*.c`, `cache_file_formats.c`), the tag validator (`tag_validate.c`, `tag_schema_*.c`), touch, FOV and more |
| `port/linux/include/` | The port's headers: the limits (`halo_port_limits.h`, with `HALO_PORT_NETWORK_VERSION`), capacities (`halo_port_capacity.h`), the prefix header, `halo_linux_source_fixups.h` (port functions the game's sources call) |
| `port/include/xdk/` | Stand-ins for the Xbox SDK's headers (declarations only) |
| `port/windows/` | The Windows build's own files (`win32_*.c`, headers) |
| `port/wii/` | The Wii port (branch `wii-port`): its platform files (`src/`), the Direct3D device on GX (`d3d8_gx.c`), the prefix header, the shared files it keeps (`port.json`), host checks (`tests/`), the vertex program compiler (`tools/nv2a_codegen.c`) |
| `port/android/` | The Android app: the guest runtime, the host library (`host/`), the Java app (`app/`), host imports (`host_imports.list`) |
| `port/assets/` | What the builds embed or ship: high-res HUD (`hud/`), fonts, titles, menus (`menus/`), icons, network brokers |
| `port/third_party/` | Vendored libraries, each with a README naming its upstream, version and checksum |
| `port/tools/` | Stand-alone tools (`cache_file_report.c`) |
| `tools/` | Build scripts (`linux_build.py`, `windows_build.py`, `android_build.py`, `ci_build.py`), generators (`ce_menus.py`, `port_settings.py`, `hud_assets.py`, `title_assets.py`, `embed_assets.py`, `xdk_headers.py`), the Wii's (`wii_build.py`, `wii_link.py`, `wii_package.py`, `wii_convert_maps.py`, `wii_tag_layout.py`, `wii_gx_textures.py`, `wii_dolphin.py`, `lockstep_compare.py`, and the host checks `wii_gx_psh_check.py`, `wii_nv2a_check.py`, and on converted maps `wii_animation_check.py`, `wii_data_check.py`, `wii_placeholder_check.py`) and tests |
| `docs/` | Longer design notes (`custom_edition_caches.md`) |
| `pgo/` | Profile-guided optimisation profiles |
| `assets/` | Game data for local runs (gitignored: `maps/`, `custom_maps/`) |
| `.github/workflows/` | CI: builds every platform on each push, publishes releases from `main` |

## Building

```sh
python configure.py            # debug build; add --release, --portable, --profile, --pgo=off, --lto=off
ninja linux                    # build/linux/halo
ninja android                  # the Android guest image and native libraries
ninja android_apk              # the APK (Gradle)
ninja windows                  # on Windows only
ninja wii                      # build/wii/halo.dol (devkitPPC and ../WiiLibs; see port/wii/README.md)
ninja wii-package              # build/wii/halo-wii.zip for the Homebrew Channel
python tools/ci_build.py linux release   # what CI builds
```

- `configure.py` writes `build.ninja`; `configure_args` at its top records
  the options it was run with. Restore them after trying other options.
- The Linux and Windows builds are 32-bit x86 with clang, in MSVC's ABI
  (`-fms-extensions -fshort-wchar -malign-double -fcommon`), C89 (`gnu89`):
  declarations come before statements.
- Every native build compiles without fused multiply-add and with the
  vendored musl maths (`halo_math.h`), so that every machine of a network
  game computes the same results. Do not call the system's maths functions.
- Android: the game is an ILP32 AArch64 guest (`arm64_32`) in a 64-bit app.
  Guest code can call only the host imports declared in
  `port/android/host_imports.list` and `port/android/guest/runtime/guest_host.h`,
  and only the SDL functions `guest_sdl.c` bridges. Run `ninja android`
  after changing anything it compiles: `port/linux/src`, `port/linux/game`,
  `port/android`, `source/`, the build scripts.

### Defines

| Define | Set by | Meaning |
| --- | --- | --- |
| `HALO_ANDROID` | Android | The app: its display, input, files and lifecycle |
| `HALO_GLES` | Android | The OpenGL ES renderer |
| `HALO_ARM64_GUEST` | Android | The guest's ABI (ILP32 AArch64) |
| `HALO_WINDOWS` | Windows (`halo_windows_prefix.h`) | The Windows build |
| `HALO_WII` | Wii (`halo_wii_prefix.h`, `tools/wii_build.py`) | The Wii build: big-endian PowerPC, GX, the Xbox's capacities |
| `HALO_RELEASE` | `--release` | No assertions checked |
| `HALO_PROFILE` | `--profile` | The profiling build; a normal build must be unchanged by it |

`HALO_LINUX` and `HALO_CUSTOM_EDITION` are **not** defined by any build:
code guarded by them compiles to nothing. Custom Edition support is a
setting (`game.custom_edition`), checked at run time with
`custom_edition_cache_tags_loaded()`.

## Testing

```sh
python -m pytest -q tools/harness tools/test_touch_menu.py   # the engine's functions in a fake world
python -m pytest -q tools/test_linux_port.py                # the build, the maps' checks (needs assets/maps)
python -m pytest -q tools/test_cache_file_formats.py tools/test_bmp_files.py tools/test_profile.py
python tools/test_touch_layout.py                           # the Android touch layout (JDK 17+)
python tools/test_light_storage.py
python tools/test_death_timing.py
```

`tools/harness/README.md` explains how to add an asset-free test: the code
under test is taken from the sources, not copied.

Running the game without a person at it:

- `debug.network_test` (`HALO_NETWORK_TEST=host:<map>` or `join`) hosts or
  joins a game without the menus and logs each player's state every second;
  several copies play on one machine on different loopback addresses
  (`HALO_NET_ADDRESS`, `HALO_NET_BROADCAST`). `NETCODE.md` describes the
  test settings.
- `debug.exit_after`, `debug.hidden_window`, `debug.null_renderer`,
  `debug.menu_open` (start on one menu screen), `debug.telnet_console`.
- `debug.test_input = "script:<file>"` plays controller 1 from a file of
  steps through the menus and a level, as a person would, logging each
  screen and step (`port/linux/src/test_input.c`; `port/wii/tests/playthrough.txt`,
  with `display.menus = "xbox"`).
- Use a virtual display (Xvfb, with `SDL_VIDEO_DRIVER=x11`) for windowed
  runs, never the desktop of the person you work for, and stop only the
  processes you started.

## Rules of the project

### The game's sources

- Change `source/` as little as possible, and mark each change from the
  original with a comment that starts `port:`. Write like the surrounding
  code: its naming, its comment density, and the project's plain,
  descriptive comment style.
- A port function the game's sources call is declared in
  `halo_linux_source_fixups.h` or included with a note naming its file
  (`#include "view_fov.h" /* port: port/linux/game/view_fov.c */`).
- Notes such as `BUG (original, preserved for exact matching)` mark the
  original's bugs. Fixing one is allowed; say so in a `port:` comment.

### The Xbox SDK

`port/include/xdk` was written without the SDK's headers, from the January
build's debug information and public documentation. Never read the SDK's
headers or anything derived from them, never copy from them, and do not
read `xbox/include`. To add a name, follow `port/include/xdk/README.md`.

### Networking

- Raise `HALO_PORT_NETWORK_VERSION` (`port/linux/include/halo_port_limits.h`)
  with any change to what the machines send each other, or to anything they
  must agree on, and say why in the commit. Machines of different versions
  do not play together.
- Only the host decides; clients predict their own players. Read
  `port/linux/NETCODE.md` before changing the netcode.
- Anything another machine sends is untrusted: check sizes, indices and
  rates, as the existing handlers do. `debug.network_corrupt` tests this.

### Menus

- The menus must match Halo Custom Edition's. Their XML (`port/assets/menus/ce/`)
  is generated by `tools/ce_menus.py` from the PC version's tags; the
  settings screens come from `tools/port_settings.py`. Do not edit the XML
  by hand: change the generator and run it (the command is in
  `port/assets/menus/README.md`, "Writing them again"), then check that
  `git status` shows only the screens meant to change.
- Functions the menus call but the port has not wired are listed in
  `port/assets/menus/UNWIRED.md`.

### Art and credit

- Replacement HUD, title and menu art is made of hand-drawn redraws.
- Prefer an existing redraw (the ui-svg-handmade set), then a new redraw. A
  Custom Edition picture that cannot reasonably be redrawn (a 3D render, a
  screenshot, a logo) may be used only if it is listed in
  `port/assets/menus/NON_HANDDRAWN.md`.
- `AUTHORS.md` credits art only.
- Fonts: Overpass and OpenCE (Newtown, respaced) only; see
  `port/assets/fonts/README.md`. Never ship the maps' commercial typefaces.

### Custom Edition maps

Custom Edition maps live in the data root's `custom_maps` folder with
`bitmaps.map`, `sounds.map` and `loc.map`; their level names are
`custom_maps\<name>`. OpenSauce features are not supported and must not be
added: a map that needs them is refused, and one that only carries
OpenSauce's header runs as stock Custom Edition runs it. Map files are
untrusted input: read them through bounds-checked readers. See
`docs/custom_edition_caches.md`.

### Settings

A setting is an entry in `port/linux/src/port_config.c`, with a default, an
environment variable and a comment that `config.toml` gets. Document it in
the settings table of `port/linux/README.md` (and the Android README for an
Android-only one). A departure from the stock game's look or behaviour
should be a setting, off or stock by default, unless the project decides
otherwise.

### Third-party code and licences

The repository is CC0. Vendored code goes in `port/third_party/<name>`,
copied unchanged, with a README naming the upstream, the version or commit,
the checksum and what uses it. GPL code (OpenSauce, Reclaimer) may be read
as documentation of file formats, never copied.

### Commits and pull requests

- Commit subjects are plain sentences saying what changed; the body says
  why and what was checked.
- A pull request is merged with a merge commit (`Merge pull request #N from
  <owner>/<branch>`), and follow-up changes go in separate commits whose
  subject ends with `(#N)`.
- A commit taken from a fork keeps its author and is made with
  `git cherry-pick -x`.

## The Wii port (branch `wii-port`)

`port/wii/` ports the game to the Nintendo Wii (PowerPC, big-endian, GX)
on WiiLibs (`../WiiLibs`, a sibling checkout). Read
[port/wii/README.md](port/wii/README.md) first: how it builds (`ninja wii`,
clang for `powerpc-unknown-eabi` against devkitPPC), how the maps are
converted, how it draws, and the lockstep check.

- The port and WiiLibs give and take. What the port works out that any Wii
  program could use (a fix, a format, a workaround for libogc) goes into
  WiiLibs, with its tests; whatever WiiLibs already does, the port takes
  from it rather than keeping its own copy. Before writing platform code,
  look in WiiLibs first; after writing it, ask whether it belongs there.

- `HALO_WII` marks the Wii's code in shared files. The Wii keeps the Xbox's
  capacities (`halo_port_capacity.h`), so it does not join the other
  builds' network games.
- The game data is the player's own Xbox maps
  (`C:\Users\bcolb\Documents\Halo\maps`, seen from WSL as
  `/mnt/c/Users/bcolb/Documents/Halo/maps`), converted with
  `tools/wii_convert_maps.py` into `build/wii/sd/apps/halo/maps`. Never
  commit or upload game data or converted maps.
- Host checks that need no emulator: `python3 tools/wii_gx_psh_check.py`
  (the TEV translation against the combiners, built with WiiLibs' TEV
  programs: it finds WiiLibs in `WIILIBS`, `./WiiLibs` or `../WiiLibs`) and
  `python3 tools/wii_nv2a_check.py` (the generated vertex programs against
  the interpreter), both in the CI's `wii` job. `tools/wii_animation_check.py`
  (every animation of the Xbox's maps against the converted ones) and
  `tools/wii_data_check.py` (the other data blocks and the bitmaps) need the
  maps, so they are not in the CI.
- Dolphin (`tools/wii_dolphin.py`, WiiLibs' `tools/dolphin_run.py`) opens a
  window on the desktop of the person you work for. Run it only at a
  checkpoint, say so first, keep one run at a time, give the runner's own
  `--seconds` a margin below any outer `timeout` (a killed runner leaves its
  Dolphin running and the next run fails on its locked log), and stop only
  the Dolphin started with this runner's user folder
  (`halo-wii-dolphin`).
- The x86 Linux build for the lockstep check is built and run headless in
  the CI's `archlinux` image with Docker (Snap's Docker sees only folders
  under the home folder; hand the files back with `chown` when done).
- Commit on `wii-port` only (WiiLibs: `main`);
  push only when asked. Commits carry no AI attribution: no
  `Co-Authored-By`, session link or "generated with" line.

## Specialized agents

Six roles for the Wii port. Each is given to an agent (Claude Code's
`Agent` tool, a general-purpose agent whose prompt starts with the role
below) together with a concrete task, at the model and effort its role's
table row gives (the tool's `model` and `effort` parameters). An agent that edits files works in
its own git worktree, commits there on a branch named for its task, and
reports what it changed and how it checked it; the main session reviews
and merges. Agents do not launch Dolphin: they ask the main session for a
checkpoint run, naming what it should show.

| Role | Model | Effort | Why (from each role's first run) |
| --- | --- | --- | --- |
| map-converter | opus | high | Binary formats where one wrong width corrupts data silently: it found a section-sizing bug in compressed animations by reading the game's decoder, and wrote a check of every animation in 24 maps |
| endian-auditor | opus | high | Whole-codebase reasoning about layouts and widths, cross-checked against the debug information; a missed or false finding is costly |
| gx-renderer | opus | high | Performance work that must stay bit-identical: it halved the processor's instructions per vertex and proved the output unchanged under qemu |
| lockstep-runner | opus | high | Mostly waiting on Docker and Dolphin, but its value is tracing a divergence to its cause across systems (identifier salts to a needle's wander and a tree's frame) |
| wiilibs-steward | sonnet | high | Bounded library work with clear conventions (an API, tests on three targets, CMake); its scoping was good without needing more |
| ci-steward | sonnet | high | Procedure (running the CI jobs, checking documents against code) where care, not depth, finds the faults |

### Running them

- Keep six agents running at once. When one finishes, the main session
  reviews its report (and merges its branch once its checks pass, after a
  Dolphin checkpoint if it changes what is drawn or played), records what
  needs a checkpoint, and starts the next task at once.
- No two running agents edit the same files; editing agents work in their
  own worktrees.
- The next task is chosen as "Choosing the work" says, for whichever role
  it fits; a role may run twice at once on unrelated tasks.

### Choosing the work

Without being told, pick the most valuable work that does not need a real
Wii. The aim is a game a person can sit down and play: everything below
serves that, and work that brings it nearer comes first. In order:
1. **Playable from start to finish.** What stops or spoils a person
   playing, found by playing it in Dolphin as a person would (a scripted
   controller, `debug.test_input`, not only the bot):
   - the menus: the title, campaign and difficulty, the pause menu,
     settings, saving and loading a checkpoint, quitting to the menu;
   - the controls: the Classic Controller, GameCube pad and Wiimote with
     Nunchuk mapped as the README says, aiming, the deadzones;
   - every level from its start to its end, cutscenes included, with no
     halt, hang, missing script trigger or memory run out over a long
     session (the memory report's peaks);
   - sound: music, dialogue and effects heard (the audio report);
   - **the frame rate: 30 frames a second (33 ms a frame) on every level**,
     the Xbox's own rate. The frame's time split (`a frame takes`) for each
     level shows where it goes; cut the largest part first, and count the
     cut as the log's work (the processor's vertices, stages, textures
     uploaded, the game's own update) as well as the milliseconds, which
     Dolphin measures in the console's time but only approximately.
2. **Correctness the lockstep check can show.** The first tick at which a
   level's x86 and Wii logs differ, on every level (a10 to d40, then the
   multiplayer maps): find the cause and fix it. A big-endian fault breaks
   play on the console and nothing else finds it as surely.
3. **What is drawn wrong or not at all**, shown by a Dolphin screenshot or
   A/B: text, the HUD, menus, then the README's "What is next" (bumped
   specular, bumped reflections), and anything a checkpoint run shows broken.
4. **Faults found by reading.** The endian-auditor's sweeps of code not yet
   covered (its reports list what was checked), and the map-converter's
   checks of data not yet verified value by value.
5. **Health**: the CI's jobs, upstream merges, documents, WiiLibs taking
   what the port worked out.

Work that needs the real console is not done by agents; it is listed for
the person, with what to look for, in port/wii/README.md: timings, the
processor's alignment exceptions, frsqrte's accuracy, the SD card's speed,
the controllers. Prefer a task whose result a host check, the lockstep
check or a Dolphin A/B can confirm; say in the report how it was confirmed.

### map-converter

Owns the map conversion: `tools/wii_convert_maps.py`,
`tools/wii_tag_layout.py`, `tools/wii_gx_textures.py`.
- The converter swaps every tag field by the layouts the Wii build's debug
  information gives (`build/wii/halo.elf`), relocates pointers, rewrites
  bitmaps in GX's formats, and converts the data blocks whose contents are
  not plain fields (vertices, animations, recorded animations, script
  syntax, cluster visibility).
- Its checks: `tools/wii_animation_check.py` (every animation, value by
  value, against the game's own reading); every map converts (`--output`
  into a scratch folder, never the repository); the notes it prints (fields left as they are that hold
  data, data not reached by the walk); a converted structure read back
  against the game's own reading of it (`tools/wii_animation_check.py`,
  `tools/wii_data_check.py`: nothing may differ on the 24 maps); and
  `tools/wii_placeholder_check.py`: the game reads none of the bytes left
  as they are that hold data (by its declarations and clang's AST of the
  build; it must report nothing on the 24 maps).
- A field the game reads through another structure than its tag's (as the
  contrail's shader) is listed in `Schema.EMBEDDED`.

### endian-auditor

Read-only. Finds big-endian hazards in `source/` and `port/linux/game/`:
values written at one width and read at another (a narrow value in a
32-bit cell, `halo_cells.h`), unions of different widths, byte-order
assumptions, C bitfields over data from maps or files, mismatched
prototypes across files. Reports each finding with its file and line, the
expression, why it differs on a big-endian machine, and how likely it is
to matter in play; confirms with the lockstep check's results where it
can. Fixes are made by the main session or a `gx-renderer` or
`map-converter` agent.

### gx-renderer

Owns the Wii's Direct3D device: `port/wii/src/d3d8_gx.c`, `gx_psh.c`,
`nv2a_cpu.c`, `nv2a_cpu.h`, `nv2a_cpu_native.h`,
`port/wii/tools/nv2a_codegen.c`.
- Drawing must match the processor's path: a change to GX's transform is
  checked by an A/B of the same moment with `debug.gpu_transform = false`
  (a checkpoint run the main session makes).
- Speed is measured as the processor's vertices (`vertices (N run)` and
  `vertex programs run most` in the log) as well as Dolphin's milliseconds,
  which do not track real hardware.
- Builds with `ninja wii`; runs the host checks after any change to the
  translation or the vertex programs.

### lockstep-runner

Owns the lockstep check (`debug.fixed_ticks`, `debug.lockstep_log`,
`debug.lockstep_detail`, `tools/lockstep_compare.py`; port/wii/README.md,
"The lockstep check").
- Builds and runs the x86 Linux side headless in Docker, as that section
  says, for any level and number of ticks; the Wii's side is a checkpoint
  run the main session makes with the same settings.
- Compares the logs, names the first tick that differs, has both sides
  list their objects at that tick, and traces the difference to its cause
  in the code (a log line added for the purpose is removed once found).
- Keeps a table in its report of each level checked: the ticks that agree
  and the first that does not.

### wiilibs-steward

Owns the give-and-take with WiiLibs (`/home/bcolb/GitHub/WiiLibs`, branch
`main`): what the port works out that any Wii
program could use goes into WiiLibs, with its tests, and the port then
takes it from there.
- Works in its own git worktree of WiiLibs, on a branch named for its task,
  off `main`; follows WiiLibs' own conventions
  and runs its tests (host `ctest`, the big-endian `ppc-be` build under
  qemu, and the Wii compile check; `ctest -L dolphin` only through a
  checkpoint run of the main session).
- Reports what it moved, the tests that cover it, and the port's change
  that would take it from WiiLibs.

### ci-steward

Owns what keeps the branch healthy: `.github/workflows/build.yml`, the
build scripts' CI paths (`tools/ci_build.py`, `tools/wii_build.py`,
`tools/wii_package.py`), merges from upstream, and the documents.
- Runs the CI's jobs locally in their own images with Docker (the `wii`
  job in `devkitpro/devkitppc`, the `linux` job in `archlinux`) on the
  branch's head, from a clean export (`git archive`), and reports what
  fails and why.
- Checks that the READMEs and AGENTS.md say what the code does (settings,
  commands, file names), and lists what is stale.
- Changes go in its own worktree and branch; a merge from upstream is the
  main session's.

## Documents

| Document | About |
| --- | --- |
| [README.md](README.md) | Downloads, game data, building, build options, profiling builds |
| [port/linux/README.md](port/linux/README.md) | The Linux build, controls, menus, every setting, updates, frame rate, field of view, system link, internet play, voice chat, the server browser, security, map checks, how the port operates, the game source changes |
| [port/linux/NETCODE.md](port/linux/NETCODE.md) | The distributed netcode, network versions, joining a game in progress, transport, testing |
| [port/windows/README.md](port/windows/README.md) | The Windows build, its headers, inline functions, crash reports |
| [port/android/README.md](port/android/README.md) | The Android build, the guest and host, touch controls, Android settings, finding problems |
| [port/include/xdk/README.md](port/include/xdk/README.md) | How the SDK declarations were written and are checked |
| [docs/custom_edition_caches.md](docs/custom_edition_caches.md) | Loading and converting Custom Edition maps, and the evidence for each layout |
| [port/assets/menus/README.md](port/assets/menus/README.md) | The menu files' format and how to write them again |
| [port/assets/menus/NON_HANDDRAWN.md](port/assets/menus/NON_HANDDRAWN.md) | Menu pictures that are not redraws |
| [port/assets/menus/UNWIRED.md](port/assets/menus/UNWIRED.md) | Menu functions that do nothing yet |
| [port/assets/fonts/README.md](port/assets/fonts/README.md) | The fonts and their licences |
| [tools/harness/README.md](tools/harness/README.md) | Asset-free regression tests |
| `port/third_party/*/README.md` | Each vendored library |
| [AUTHORS.md](AUTHORS.md) | Art credits |
| [port/wii/README.md](port/wii/README.md) | The Wii port: building, map conversion, drawing, the lockstep check |
