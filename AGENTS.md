# Notes for agents

This repository ports the Halo: Combat Evolved decompilation (Xbox build
01.01.14.2342, `cachebeta.exe`) to Linux, Windows, Android and the web. The game's C
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
| `port/linux/game/` | The port's game-side code, compiled as the game's sources are: the distributed netcode (`network_*.c`), co-op, map sharing (`map_share*.c`), the menus (`menu_tags.c`, `menu_functions.c`), Custom Edition maps (`custom_edition_*.c`, `cache_file_formats.c`), the tag validator (`tag_validate.c`, `tag_schema_*.c`), touch, FOV and more |
| `port/linux/include/` | The port's headers: the limits (`halo_port_limits.h`, with `HALO_PORT_NETWORK_VERSION`), capacities (`halo_port_capacity.h`), the prefix header, `halo_linux_source_fixups.h` (port functions the game's sources call) |
| `port/include/xdk/` | Stand-ins for the Xbox SDK's headers (declarations only) |
| `port/windows/` | The Windows build's own files (`win32_*.c`, headers) |
| `port/wii/` | The Wii port (branch `wii-port`): its platform files (`src/`), the Direct3D device on GX (`d3d8_gx.c`), the prefix header, the shared files it keeps (`port.json`), host checks (`tests/`), the vertex program compiler (`tools/nv2a_codegen.c`) |
| `port/android/` | The Android app: the guest runtime, the host library (`host/`), the Java app (`app/`), host imports (`host_imports.list`) |
| `port/web/` | The web build: its runtime (`src/`: entry, sockets inside the page, memory watch), the page and its service worker (`site/`), the SDL port, tests |
| `port/assets/` | What the builds embed or ship: high-res HUD (`hud/`), fonts, titles, menus (`menus/`), icons, network brokers |
| `port/third_party/` | Vendored libraries, each with a README naming its upstream, version and checksum |
| `port/tools/` | Stand-alone tools (`cache_file_report.c`) |
| `tools/` | Build scripts (`linux_build.py`, `windows_build.py`, `android_build.py`, `web_build.py`, `ci_build.py`, `web_serve.py`), generators (`ce_menus.py`, `port_settings.py`, `hud_assets.py`, `title_assets.py`, `embed_assets.py`, `xdk_headers.py`), the Wii's (`wii_build.py`, `wii_link.py`, `wii_package.py`, `wii_convert_maps.py`, `wii_tag_layout.py`, `wii_gx_textures.py`, `wii_dolphin.py`, `lockstep_compare.py`, and the host checks `wii_gx_psh_check.py`, `wii_nv2a_check.py`, and on converted maps `wii_animation_check.py`, `wii_data_check.py`, `wii_placeholder_check.py`) and tests |
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
ninja web                      # build/web/site (Emscripten: port/web/README.md)
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
- Web: the game is `wasm32` (also ILP32), run on a Web Worker; calls must
  match their callee's type exactly (the link fails on a mismatch), and
  WebGL 2 is ES 3.0. Run `ninja web` after changing what it compiles (as
  for Android, and `port/web`). Refer to [port/web/README.md](port/web/README.md).

### Defines

| Define | Set by | Meaning |
| --- | --- | --- |
| `HALO_ANDROID` | Android | The app: its display, input, files and lifecycle |
| `HALO_GLES` | Android, web | The OpenGL ES renderer |
| `HALO_ARM64_GUEST` | Android | The guest's ABI (ILP32 AArch64) |
| `HALO_WEB` | Web | The browser: its frames, storage, sockets and WebGL 2's limits (with `HALO_GLES`, not `HALO_ANDROID`) |
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
python -m pytest -q tools/test_web_build.py                 # the web build's graph, sockets, menus
node --test "port/web/tests/*.test.js"                      # the web page's importer and service worker
node port/web/tests/webrtc_native.mjs chromium firefox      # the native builds' WebRTC with browsers (Playwright)
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
- Internet play is the same code in every build, the web's too: a browser's
  tunnel is a WebRTC data channel (`port/web/src/web_p2p.c`), which the
  native builds take on their tunnel's socket (`p2p_webrtc.c`, `posix_dtls.c`).
  Check a change with both (`port/web/tests/webrtc_native.mjs`, and with the
  maps `port/web/tests/internet.mjs`).
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
- Commit on `wii-port` only (WiiLibs: `Xbox`);
  push only when asked. Commits carry no AI attribution: no
  `Co-Authored-By`, session link or "generated with" line.

## Specialized agents

Three roles for the Wii port, divided by the files each owns so that all
three can run at once without meeting. Each is given to an agent (Claude
Code's `Agent` tool, a general-purpose agent whose prompt starts with the
role below) with one concrete task, at its row's model and effort (the
tool's `model` and `effort` parameters).

| Role | Model | Effort | Owns |
| --- | --- | --- | --- |
| renderer | opus | high | The Direct3D device on GX and the frame rate: `port/wii/src/d3d8_gx.c`, `gx_psh*.c`, `gx_lighting.c`, `nv2a_cpu*`, `port/wii/tools/nv2a_codegen.c`, `tools/wii_gx_textures.py` |
| game | opus | high | The game right on a big-endian machine: the map converter and its checks (`tools/wii_convert_maps.py`, `wii_tag_layout.py`, `wii_*_check.py`), the lockstep check (`tools/lockstep_compare.py`), the playthrough scripts (`port/wii/tests/*.txt`), and the `port:` fixes in `source/` and `port/linux/game/` |
| platform | sonnet | high | Everything around the game: WiiLibs (its own worktree, branch off `Xbox`), the Wii's other platform files (`port/wii/src/wii_*.c`, `xinput_wii.c`, `posix_net_wii.c`), the CI and build scripts (`.github/workflows/build.yml`, `tools/ci_build.py`, `tools/wii_build.py`, `tools/wii_package.py`), and the documents |

A shared file outside these (`port/linux/src/*`, `port_config.c`) belongs,
for one task, to the agent the task's prompt gives it to. Each agent
describes its own change in the README section it concerns; the platform
agent keeps the documents as a whole true to the code.

### Running them

- Three at once, one per role. When one finishes, the main session reviews
  its report, merges its branch once its checks pass (after a Dolphin
  checkpoint if it changes what is drawn or played), and gives that role
  its next task at once.
- The prompt is short: the role's paragraph below, the task, what "done"
  looks like, and how to confirm it. The report is short too: what
  changed, how it was checked, and what checkpoint run it needs (at most
  20 lines).
- An agent that edits works in its own git worktree and commits there on a
  branch named for its task. Agents do not launch Dolphin: they ask the
  main session for a checkpoint run, naming what it should show. Merges
  from upstream are the main session's.

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
4. **Faults found by reading.** The game agent's endian sweeps of code not
   yet covered (its reports list what was checked), and its checks of
   converted data not yet verified value by value.
5. **Health**: the CI's jobs, upstream merges, documents, WiiLibs taking
   what the port worked out.

Work that needs the real console is not done by agents; it is listed for
the person, with what to look for, in port/wii/README.md: timings, the
processor's alignment exceptions, frsqrte's accuracy, the SD card's speed,
the controllers. Prefer a task whose result a host check, the lockstep
check or a Dolphin A/B can confirm; say in the report how it was confirmed.

### renderer

Draws what the Xbox drew, at 30 frames a second.
- A change to GX's transform is checked by an A/B of the same moment with
  `debug.gpu_transform = false` (a checkpoint run).
- Speed is counted as the log's work (the processor's vertices, `vertex
  programs run most`, draws, bytes flushed) and qemu instruction counts, as
  well as Dolphin's milliseconds, which do not track the console.
- Builds with `ninja wii`. Runs `tools/wii_gx_psh_check.py` and
  `tools/wii_nv2a_check.py` after any change to the translation or the
  vertex programs.
- A render-only shortcut (fused multiply-adds, estimates) never reaches
  game state.

### game

Makes the Wii's game the x86 game, tick for tick, from start to end.
- The converter swaps every tag field by the layouts `build/wii/halo.elf`
  gives, relocates pointers, and converts the data blocks that are not
  plain fields. Its checks (`wii_animation_check.py`, `wii_data_check.py`,
  `wii_placeholder_check.py`) report nothing on the 24 maps. Output goes to
  a scratch folder, never the repository.
- The lockstep check: it builds and runs the x86 side headless in Docker
  (port/wii/README.md, "The lockstep check"), and the Wii side is a
  checkpoint run. It names the first tick that differs and traces it to its
  cause. Its report keeps a table of levels: the ticks that agree, the
  first that does not.
- Playthroughs: scripts (`debug.test_input`) that play the menus and every
  level from start to end as a person would.
- Between tasks, read-only sweeps for big-endian hazards: widths, unions,
  byte order, bitfields over file data, mismatched prototypes. Each finding
  gives its file and line and why it matters.

### platform

Makes it a Wii program a person can use, and keeps the branch healthy.
- WiiLibs: what the port works out that any Wii program could use moves
  there, with its tests (host `ctest`, `ppc-be` under qemu, the Wii compile
  check). The port then takes it from WiiLibs.
- The Wii's platform: the controllers, the system buttons, HOME, saving,
  video modes, start-up.
- Health: the CI's jobs run locally in their images from a clean export
  (`git archive`), and the READMEs and this file are checked against the
  code.

## Documents

| Document | About |
| --- | --- |
| [README.md](README.md) | Downloads, game data, building, build options, profiling builds |
| [port/linux/README.md](port/linux/README.md) | The Linux build, controls, menus, every setting, updates, frame rate, field of view, system link, internet play, voice chat, the server browser, map sharing, security, map checks, how the port operates, the game source changes |
| [port/linux/NETCODE.md](port/linux/NETCODE.md) | The distributed netcode, network versions, joining a game in progress, map sharing, transport, testing |
| [port/windows/README.md](port/windows/README.md) | The Windows build, its headers, inline functions, crash reports |
| [port/android/README.md](port/android/README.md) | The Android build, the guest and host, touch controls, Android settings, finding problems |
| [port/web/README.md](port/web/README.md) | The web build, the page, the game data in the browser, limits, how it operates, hosting on GitHub Pages |
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
