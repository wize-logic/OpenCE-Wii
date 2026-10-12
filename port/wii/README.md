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
  `debug.network_test_tour` seconds of game time in each). In Dolphin a10's frame takes about
  116 ms and b30's about 580 ms (the frame's split in the log; the aim is 33 ms, 30 frames a
  second; measured on 10 October, and not timed since the draw-cost work of the 12th, which
  cut the processor's work per draw). On the Wii, `display.bumped_highlights` is off by default (bumped specular and bumped
  reflections: their passes' vertices run on the processor, about two fifths of a10's frame),
  `display.object_shadows` is off by default (each object's shadow draws it again, a pass of
  skinned vertices), `display.model_detail` is 0.5 and `display.environment_reflections` is
  off (two of the environment's five passes over its shiny surfaces). `display.bumped_lightmaps`
  is on, as elsewhere: its cube map's direction is computed once a vertex and kept. "What the frame draws" below has the
  counts.
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
ninja wii                                   # build/wii/halo.dol and halo.elf (and the converter's texture library)
tools/wii_convert_maps.py XBOX/maps/ui.map XBOX/maps/bloodgulch.map   # into build/wii/sd/apps/halo/maps
tools/wii_dolphin.py --seconds 300 --until "frame 360:"            # run it in Dolphin from WSL
tools/wii_gx_psh_check.py                   # the combiner translation, checked on the host (WIILIBS=DIR)
tools/wii_nv2a_check.py                     # the compiled vertex programs against the interpreter
tools/wii_input_check.py                    # the controllers' mapping to the Xbox gamepad (WIILIBS=DIR)
tools/wii_gx_texel_check.py                 # the processor's texel reads of the GX textures (WIILIBS=DIR)
tools/wii_animation_check.py --converted DIR XBOX/maps/*.map   # the converted maps against the Xbox's (map conversion, below)
tools/wii_data_check.py --converted DIR XBOX/maps/*.map
tools/wii_placeholder_check.py XBOX/maps/*.map    # the game reads none of the tag bytes left as they are
ninja wii-package                           # build/wii/halo-wii.zip for the Homebrew Channel
```

`ninja wii-package` zips `apps/halo` as the Homebrew Channel reads it: `boot.dol`, `meta.xml`
and an icon drawn by the packager. It holds no game data. `tools/wii_package.py` is a thin
wrapper: it calls WiiLibs' `tools/wii_package.py` (the checkout the build uses, `--wiilibs`) with
the port's name, the version (the repository's description), the icon's lettering and the long
description, which is in `package_description.txt` beside this file. The `wii` job in
`.github/workflows/build.yml` runs the four host checks (`wii_gx_psh_check.py`, `wii_nv2a_check.py`,
`wii_input_check.py`, `wii_gx_texel_check.py`) and the profile tool's test
(`tools/test_wii_profile.py`), builds the zip in devkitPro's devkitPPC image (with WiiLibs checked out
into `./WiiLibs`) and keeps it as an artifact.

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
  with the draws' split (GX's transform, the processor's, immediate), GX's transform's setup by
  part (what the draw depends on, the matrices, the lights and sphere maps, the kept outputs,
  GX's state; in tenths of a millisecond, with the draws in hardware a frame), the processor's
  vertices (fetch, program, store, emit), the streams' flushes (made and kept), the end of the frame's (GX finishing the frame's
  draws; the copy to the display and the wait for its retrace) and the textures' counts (found
  by their mark or looked up, made, hashed, decoded, flushed, TMEM emptied). A frame of more than
  half a second (loading, mostly) gets a split of its own.

## Game data

The game reads its data from `apps/halo` on the SD card, or from a USB drive when the card has no
`maps` folder there (`platform_data_root`, `port/linux/src/xbox_files.c`). Saves go to
`apps/halo/saves`, and the settings to `apps/halo/config.toml`.

With no `maps` folder in either place the game does not start: WiiLibs' stop screen says so on
the console's text screen, where to put the converted maps, and whether the SD card and the USB
drive were found, and waits for HOME (or START on a GameCube controller) to return to the loader
(`wii_stop_no_maps`, `src/wii_platform.c`; called from `platform_data_root`).

The maps must be the Xbox's (cache version 5), converted by `tools/wii_convert_maps.py`. Halo PC's
and Custom Edition's maps don't run. A converted map is uncompressed, about the size the Xbox
inflated it to, and the Wii reads it where it is.

### Saves

A save survives a reset, a power-off or a pulled card. Each file the game opens for writing under
`apps/halo/saves` (the profiles, the saved games, each profile's `savegame.bin`, the checkpoint the
game keeps for Save and Quit) is written beside itself as `<file>.wtmp`, and takes the file's place
when the game closes it (`xbox_files.c`, "saves that survive a cut"): the data is synced, then the
copy is renamed over the old file. libfat cannot rename over a file, so there the old one goes
aside as `<file>.wold` for the moment between the two renames. A cut at any call leaves the old file
or the new one, whole.

- A file the game writes whole from its start (a profile; the checkpoint) is not copied into its
  copy first; one written in part (a patch in the middle, an append, a resize) is.
- The next start sweeps the save folders (`shadow_sweep`): a `.wold` is put back where its file is
  missing (the cut came between the two renames) and goes where it is not, and a `.wtmp` goes. The
  game's directory listings never show either. A file with a copy that is still open when the
  console leaves is not replaced: the old one stays.
- Not shadowed: `z:\savegame.bin` opened unbuffered and kept open (the game state's revert buffer,
  of no use after the game ends), and files outside `saves` (`d:\core`).
- A full card, a read-only card, a card that is gone: the call that fails tells the game with the
  error it knows (`ERROR_DISK_FULL`, `ERROR_ACCESS_DENIED`, not found), as on the Xbox, and the
  game's handlers run (`failed to write player profile to file`, `failed to write to persistent
  storage`, where the game deletes the checkpoint it was writing). The old file stays, whatever the
  failure, no copy is left, and the log says `save: <file> ...` and why. A write that takes no bytes
  and sets no error (libfat does that with no room left) is a full disk, not a call to repeat.
- Writes still go through the gate the shutdown uses (`wii_files_quiesce`): the ones under way
  finish, the ones that would start wait; so does the replacing at close.

Checked on the host: `python -m pytest -q tools/harness/tests/test_atomic_saves.py` runs the
game's own sequences (a profile written whole, the checkpoint with its header last, a patch, an
append, a file made, emptied and written, grown) on a real directory with the card modelled, cut
before each call that changes it (and half-way through each write), on a card that renames over a
file and on FAT's, then sweeps and checks that the file is the old one or the new one, no copy
is left, and that a copy renamed over a file was synced after its last change; and the same
sequences on a full card (a write that errs, one that takes no bytes), a read-only card, a card that
is gone and a rename that fails. Fourteen faults put into the code are each caught. Needs a
checkpoint on a real card (item 6 of "What only a real Wii can show"): power off during a save, and a card pulled.

### Files the game makes again, and the start-up

The game writes its default player profiles and default game variants
(`z:\saved\player_profiles\default_profile\`, `z:\saved\playlists\default_playlist\`: about
thirty files) at every start, and the menus take no input until the start-up thread has done it
(`filesystem_initialization_thread_proc`, `source/interface/ui_widget.c`; on the Xbox the intro
movie covers it, and the Wii has no Bink). It empties the folder, makes each file (`file_create`,
an empty file), opens it and writes one block into it. With the saves' copy-and-rename above that
was, for one variant on FAT, 11 calls that change the card (2 opens, 1 write, 2 syncs, 4 renames, 2
unlinks) on the first start and 12 on every start after; over thirty files that is the 4.5 s the
first Dolphin run showed. These files lose nothing to a cut (the next start writes them again), so
(`xbox_files.c`, "files made again"):

- They are not written beside themselves and not synced.
- `DeleteFileA` leaves them where they are, and a file made again keeps its contents until the
  game writes something else into it; a file the game emptied and then wrote is cut to what was
  written when it closes.
- A file is opened for reading until a write differs from what it holds: a write of the same bytes
  is not made, and the descriptor that can write is opened only for a write that differs.
- The first start writes each variant once (2 calls: the file made, one write), and every start
  after writes nothing to the card for them (0 calls). `python3 -m pytest
  tools/harness/tests/test_atomic_saves.py` has the cases (`regen-first`, `regen-again`,
  `regen-changed`, with the sequence of calls counted: `atomic_saves profile:regen-count` prints the
  numbers above) and four faults that must be caught.

Smaller changes with the same aim: a copy that nothing was written to (a file made empty) is
renamed without a sync; a file opened for reading and writing that the game then only reads (the
listing of the saves opens every one to check its sum) is opened for reading, and gets its copy
at the first write; `SetFileAttributesA` does not write the read-only bit when it is already as
asked (the game makes a file ordinary before it deletes it); and a path that is there as it is
written is translated with one `stat` where it had a look into every directory along it
(`translate_as_written`: each look opens a directory of the card and reads all its entries, once
for every part of the path, at every open; the answer is the same, which `tools/harness/tests/test_path_translation.py` checks
against the look at every part, and it counts the directories looked into).

What was measured, and what was not: the call counts above are exact (the host test counts the
calls that change a modelled card). The time is for a checkpoint run: the log now says where the
card's time goes. About every two seconds while files are used, and when a call takes 25 ms or
more (the first 64), `files:` lines appear:

```
files: 410 ms in 187 calls: paths 12 ms (95) open 120 ms (30) directories 35 ms (31) writes 210 ms (30) ...
files: writes took 83 ms (sd:/apps/halo/saves/z/saved/playlists/default_playlist/03/blam.lst)
```

(`paths` the translation of Xbox paths, `open` the opening of a file with its copy, `directories`
mkdir and the opening of a listing, `attributes` stat, `writes`, `syncs`, `closes`, `replacing` the
copy's rename over its file, `deleting`). The start-up's cost is the total on the line after the
last default variant.

### The loading screen

The game's loading screen is its progress bar (`source/interface/progress_bar.c`: a blue swirl
made from noise textures and the 320 by 240 `loading.tga` that has the word LOADING in it, with a
chant behind it). It is drawn by `render_frame_pregame` only while `game_precache_new_map`'s loop
runs, and that loop runs as long as `cache_files_precache_map_status` says the map is still being
copied to the Xbox's hard drive, which was most of a level's load there. On the Wii:

- The precache is done at once (`cache_files_precache_map_begin` opens the map where it is, and
  `cache_files_precache_map_status` says success with progress 1.0 at its first call), so the loop
  runs once: one frame, with progress 1.0. `progress_bar_display` takes the first progress it is
  given as its starting point and draws `(progress - start) / (1 - start)`, which is 0/0 here.
  Then `progress_bar_end`, and the whole load (`game_load`, `game_initialize_for_new_map`: tags,
  structure, objects, sounds) runs on the main thread with no frame presented, so the TV holds the
  last picture presented, still, for the whole load (the log's one long frame, "frame N took 1243
  ms"). It is not the animation, and not black on purpose.
- `maps/loading.tga` is not on the SD card: the disc's maps folder has it, the player's copy of the
  maps does not (24 files, no `loading.tga`), and the converter copies none. The word LOADING is
  then missing from the picture (the texture is made and left as it was).

What the animation needs, which is not the platform's: (1) the progress bar kept active and
`map_load_in_progress` set until `main_new_map` has finished, not until the precache says done;
(2) a pump, `loading_pump()`, that draws only the progress bar (not the widgets, whose tags the
load is replacing) and presents, at most every 33 ms, called from where the main thread waits for
the card (`cache_file_block_until_not_busy`, `cached_map_block_on_async_request` and the
`SleepEx(0, TRUE)` loops of `cache_files_windows.c`: the cache thread reads while the main thread
spins, which is where the console spends most of a level's load) and between the calls of
`game_initialize_for_new_map`; (3) a progress that rises with the load (the bytes read of the map
over a size the load is expected to read, rounded off as 1 - 1/(1 + bytes/8 MB)); (4)
`loading.tga` from the disc in the maps folder and copied by the converter; (5) a checkpoint
that shows `progress_bar_render`'s draws (render targets copied to textures, immediate vertices)
come out on the GX device. None of this is done; it is listed for the game and renderer.

### Reading the maps

A level load reads the map through `ReadFile` and `ReadFileEx` with an offset; the Xbox's overlapped
reads finish where they are issued here (there is no read in flight to wait for), so there is no
asynchronous read to add. Two things are the platform's:

- A handle opened for reading only with `FILE_FLAG_OVERLAPPED` (the maps) keeps its place: a read
  that starts where the last one ended is a `read` and nothing else (WiiLibs'
  `WlPlacedReader`, `wl/platform/posix.h`). Before, each was WiiLibs' `pread`, which is a seek to
  the offset, the read and a seek back to where the handle stood; on
  FAT a seek walks the file's chain of clusters from the cluster it stands at (or from the start,
  when it goes backwards), and the one back to the handle's old place made every read walk the
  chain from the start. The handle's own position is put back when something that uses it comes
  (`SetFilePointer`, a read without an offset).
- The reads are counted: about every two seconds, `files: reads in 2000 ms: N, K KB in M ms
  (largest L KB; S under 4 KB, B into a buffer not on 32 bytes, U from an offset not on 512, X
  seeks)`, and a file that was read much says, as it closes, `files: <path> closed after N reads, K
  KB in M ms`. The three counts say whether the reads reach the card's driver as it likes them:
  libogc's SD driver reads straight into a buffer that is on 32 bytes (the DMA's alignment), and
  is believed to go by a copy for one that is not (the checkpoint's counts will say whether
  that matters); libfat hands it the buffer shifted by the offset's place in its sector, so what
  counts is the address less the offset, modulo 32.

The a10 and b30 loads in the earlier Dolphin runs (`build/wii/ck_*.log`) are, in the emulated
clock the log's times are in: the menu's map 0.7 s and then 253 reads, 12 MB in 0.69 s; a level
123 to 125 reads, 33 to 36 MB in 0.38 s of a 1.2 s frame whose other 0.8 s is the game's own work
(tags fixed up, the structure, the first objects); a saved game's load 120 reads, 36 MB in 0.40 s.
Dolphin reads the host's disk, which is faster than an SD card by an order of magnitude, so on
the console the same 36 MB are what the load waits for (at 5 to 10 MB/s, 4 to 7 s), and the reads'
number and size are what to cut there. (A load that takes 11 s of wall-clock time against 1.2 s in
the log's clock would mean Dolphin runs the load at a fraction of real time; the checkpoint run
will tell which it was, from the `files:` lines and the times on them.)

## How the port operates

- **Compiler.** The game is compiled with clang `--target=powerpc-unknown-eabi -mcpu=750` against
  devkitPPC's newlib and libogc, then linked by devkitPPC's gcc (`tools/wii_build.py`).
  devkitPPC's own gcc cannot compile the game's MSVC dialect, while clang already compiles it for
  the other ports.
- **Flags.** The ABI flags reproduce the MSVC/Xbox environment, as the Linux build's do:
  - `-msvr4-struct-return` is PowerPC's spelling of `-freg-struct-return`
  - `-ffp-contract=off` because PowerPC has fused multiply-adds. Clang has let them through in a
    function it does not optimise (`#pragma optimize("", off)`: `ai_handle_spatial_effect`,
    `source/ai/ai.c`, now with `#pragma clang fp contract(off)`), so `ninja wii` reads the game's
    objects for them (`tools/wii_fp_check.py`, a thin wrapper that calls WiiLibs'
    `tools/fma_check.py`). The sound mixer (`dsound_sdl.c`) alone has them on
    (`CONTRACTED_SOURCES`, `tools/wii_build.py`): its output only reaches the speakers, and they take
    14% off its instructions
  - `-fomit-frame-pointer`, unlike the x86 builds: PowerPC's frames are chained through each
    frame's first word whether or not r31 holds a frame pointer, and that chain is what WiiLibs'
    backtraces follow. Without one the game's code is 2.3% smaller
  - `-femulated-tls`: native thread-local storage would use r2, which the EABI gives the small
    data area. WiiLibs provides `__emutls_get_address` for each libogc thread.
- **Prefix header.** `include/halo_wii_prefix.h` is force-included and wraps the Linux one. Its
  only change is a 16-bit `wint_t` ahead of newlib's.
- **Start-up.** The link wraps `main`. `src/wii_boot.c`'s `__wrap_main` does the following, then
  calls the game's `main` (`source/shell/shell_xbox.c`):
  - claims the fixed memory
  - starts WiiLibs: system buttons, log, crash report, the text console (`wl_platform_console_start`),
    heaps, then the buttons' watcher thread (`src/wii_system.c`), IOS 58, SD and USB, and then the
    log's file on the card: `sd:/apps/halo/logs/log-NN.log` when that folder exists (the first call
    of `wl_platform_log_init` precedes the mounts and gives the ring and the debug port only; the
    second opens the file)
  - turns on the game's heap in MEM2 (the overflow of MEM1's malloc)
  - starts the network in the background

  Clang puts constructors in `.init_array`, which devkitPPC's start-up code never runs, so the
  port has none. The build checks that.
- **Platform layer.** `port.json` lists the shared platform files (`port/linux/src`) the Wii
  keeps. These include the Xbox kernel's threads and events, the files, Direct3D's state and
  resources, the DirectSound mixer and the Winsock layer. `src/` holds the Wii's own:

  | File | What it does |
  | --- | --- |
  | `wii_platform.c` | log, messages (the stop screen when the maps are missing) and quitting |
  | `wii_system.c` | the reset and power buttons, and leaving the game properly: a watcher thread, the shutdown hooks |
  | `wii_posix.c` | names for the shared files over WiiLibs: where the game's folder is (`wl_platform_fs_find_app_dir`), random bytes (`wl_platform_random_bytes`), thread priorities (`wl_posix_thread_set_relative_priority`), the heap report |
  | `wii_boot.c` | what runs before the game's `main`: the memory, WiiLibs, the log's file, the game's thread |
  | `wii_memory.c` | the fixed memory ranges the game is linked to (MEM1's game state, MEM2's window) |
  | `wii_video.c` | the TV's mode from the console's settings, the 4:3 picture on a 16:9 screen, 30 frames a second at 50 Hz |
  | `wii_profile.c` | arms WiiLibs' sampling profiler and hardware counters when `profile.txt` or `hwcount.txt` is in the game's folder |
  | `xinput_wii.c` | XInput over the Wii Remote with Nunchuk or Classic Controller, and the GameCube controller; the pointer's aim |
  | `xinput_wii_map.c` | what each button and stick of those controllers is as the Xbox gamepad (checked on the host) |
  | `wii_home.c` | the HOME button's menu and the Wii's notices as a state machine apart from the game (checked on the host) |
  | `wii_home_game.c` | that machine joined to the game (its pause, WiiLibs' sound fade and exit) and the overlay it draws over the frame |
  | `posix_net_wii.c` | sockets over WiiLibs' network layer, for system link |
  | `d3d8_gx.c` | the Direct3D device on GX, with `gx_psh.c` and `gx_psh_cache.c` (pixel shaders to TEV), `gx_lighting.c` (lights and sphere maps for GX), `nv2a_cpu.c` (the vertex programs on the processor) and `wii_bitmap_texel.c` (the processor's reads of GX's textures) |
  | `wii_extras.c` | the desktop additions the Wii goes without |

  `tools/wii_link.py` (WiiLibs' `tools/link_stubs.py`) stubs any symbol still missing with a
  function that stops the game and names it (`build/wii/missing_symbols.txt`; the stop is WiiLibs'
  `wl_platform_missing_symbol`, whose screen waits for HOME or START to return to the loader).
- **Memory.** The Wii's cached memory is at virtual 0x80000000 + P, as the Xbox's is, so the
  game's and Direct3D's physical/virtual arithmetic holds. Every byte of both regions, and the
  memory report in the log, is in "Memory" below. The game's pools are the Xbox's sizes
  (`HALO_WII` in `port/linux/include/halo_port_capacity.h`), so the Wii can't join the other
  builds' distributed games.
- **Ways out.** The link wraps `exit`, `_exit`, `abort` and `SYS_ResetSystem` (WiiLibs'
  `wii/trace_wii.c`): each logs "exit(0) from 80001234 ..." (tag `exit`; the return addresses up
  the frame chain, for `addr2line` on `build/wii/halo.elf`) before it goes on, so a game that
  ends says how and from where. A failed assertion (`display_assert` and `cseries_assert` in
  `source/cseries/cseries.c`, under `HALO_WII`) logs "assert from ..." the same way, through
  `wl_platform_log_backtrace`.
- **Reset, Power and quitting.** WiiLibs latches the console's reset and power buttons and a Wii
  Remote's power button, and `wii_system_start` (`src/wii_system.c`) starts WiiLibs' watcher
  thread (`wl_platform_system_watch_start`: every 16 ms, 16 KB of stack, above the game's threads
  in priority), so they work while a map loads for tens of seconds as in play. Reset returns to
  the loader (the Homebrew Channel); power, or the Remote's power button, powers the console off.
  Both, and the menus' Quit (`platform_request_quit`) and the stop screen's HOME, go through
  WiiLibs' `wl_platform_exit`, which winds the game down in this order before it leaves:
  1. the shutdown hooks, newest first: the rumble service is stopped (xinput_wii.c: it cannot
     switch a motor on again), then the files (xbox_files.c, `wii_files_quiesce`): the game's
     writes under way finish (the wait is at most three seconds), the ones that would start wait,
     and the files open for writing are synced. A save is several writes with the header written
     last (`game_state_xbox.c`), and a file under `saves` replaces the old one only when it is
     closed whole (Game data, "Saves"), so a cut leaves the old save or the new, and no single
     write is cut;
  2. the audio output is stopped (`wl_platform_audio_stop`) and every Remote's and GameCube
     controller's motor (`wl_platform_input_stop_rumble`);
  3. the log is flushed and closed, and the SD card and the USB drive are unmounted, so that
     libfat's caches are written out;
  4. `exit(0)` (the loader) or `STM_ShutdownToStandby` (power off).

  The log says "leaving (loader)" or "leaving (power off)" (tag `system`) when it starts. A second
  way out, from another thread, waits for the first. The game's own `debug.txt` and other C
  `FILE`s are not covered (the port keeps its log in WiiLibs').
- **Start-up failures.** A failure before the game runs (the fixed memory, WiiLibs' heaps, the
  game's heap, its thread, missing maps) stops with WiiLibs' stop screen: the message on the
  console's text screen and in the log, and a wait for HOME, START, reset or power
  (`platform_show_message` stays a log line: its callers' messages are about multiplayer, which
  the Wii does not play, and none ends the game).
- **Network.** `src/posix_net_wii.c` is the platform layer's sockets (`posix.h`) over WiiLibs'
  `wl/platform/net.h`; the Winsock layer above it is the Linux build's `xnet.c`, whose byte-order
  functions are the identity on this big-endian machine. The network comes up in the background
  (`wl_net_start` from `wii_network_start`), and every call fails with `WSAENETDOWN` until it is
  up. WiiLibs' results become Winsock errors: `AGAIN` 10035, `INPROGRESS` 10036, `ALREADY` 10037,
  `NOTSOCK` 10038, `MSGSIZE` 10040, `ADDRINUSE` 10048, `ADDRNOTAVAIL` 10049, `DOWN` 10050,
  `UNREACH` 10051, `RESET` 10054, `NOMEM` 10055, `ISCONN` 10056, `NOTCONN` 10057, `TIMEDOUT`
  10060, `REFUSED` 10061, `ACCESS` 10013, `ARG` 10022; any other is IOS's errno plus 10000.
  `select` is `wl_net_poll` (64 sockets at most), `SO_TYPE` is `wl_net_socket_proto` and
  `SO_ERROR` is `wl_net_connect_result`. IOS cannot say how many bytes wait (its ioctl answers
  only `FIONBIO`), so `ioctlsocket(FIONREAD)` fails with `WSAEINVAL`, and it has no
  `getpeername`. Options other than `SO_REUSEADDR`, `SO_BROADCAST`, `SO_SNDBUF`, `SO_RCVBUF` and
  `TCP_NODELAY` are accepted and ignored.
- **Positioned reads.** `pread`, `pwrite`, `pause` and `sysconf`, which newlib declares and does
  not define, are WiiLibs' weak definitions (`wl/platform/posix.h`, `wii/posix_wii.c`).
- **Threads.** libogc's threads are strictly prioritised. Each of the game's threads gets the
  game's main priority (WiiLibs' `WL_THREAD_PRIO_MAIN`, LWP 64: `WL_POSIX_PRIO_MAIN`), moved by
  its Win32 priority (`wii_thread_priority` in `src/wii_posix.c`, over WiiLibs'
  `wl_posix_thread_set_relative_priority`), and a 128 KB stack. The game's main runs on a
  thread of its own with a 512 KB stack, started by WiiLibs' `wl_platform_run_on_stack`
  (`src/wii_boot.c`): libogc's main thread has 128 KB, below which lie its audio DSP structures,
  and a deep AI path search in b30 went past it (the game's stack reaches about 290 KB; the memory
  report says how deep, from `wl_platform_stack_used`, which measures the pattern WiiLibs fills the
  stack with). devkitPPC's `sched_yield` is a stub, so the link wraps it (`-Wl,--wrap=sched_yield`,
  `tools/wii_build.py`) to WiiLibs' `__wrap_sched_yield`, libogc's yield: the game's waits spin on
  `SwitchToThread` for its I/O thread, which otherwise never ran (the menu took 45 seconds to
  appear, a10 100 seconds to load).
- **Big-endian fixes.** The game core is the same code as on the other ports, with a few changes
  for big-endian (each marked `port:` in `source/`; the faults found so far are listed under
  "What is next"):
  - the script engine's value cells (`port/linux/include/halo_cells.h`)
  - the halves of `LARGE_INTEGER`
  - the recorded animations' bitfield header byte (rewritten by the converter)
  - the compressed animations' floats, read a byte at a time (the 750 can trap on a misaligned
    float load), and the script stack's values, each started at a multiple of 4 for the same
    reason
  - values written at one width and read at another: a profile count passed as a `word *` to a
    function that writes a word, a chicago shader's `extra_flags` (the tag's long), a fog's
    `flags` (the tag's word, read as a byte), the server's read of the pregame start request (a
    short on the wire, decoded into a long's first two bytes), and the textures the game writes
    itself (the device decodes them from a copy in the Xbox's byte order)
  - network byte order: `htonl`, `ntohl`, `htons` and `ntohs` (`port/linux/src/xnet.c`) do nothing
    on a big-endian machine, and the transport's address and port conversions
    (`source/bungie_net/network/transport_endpoint_winsock.c`, `transport_endpoint_set_winsock.c`)
    use them where they swapped bytes by hand, which is right only on a little-endian one
  - little-endian file values read byte by byte: the TGA header's width and height
    (`source/interface/progress_bar.c`), a tag's four-character name (`string_to_tag` and
    `tag_to_string`, `source/cseries/cseries.c`, the first character the high byte) and the PCM
    samples of the sounds the shared mixer plays (`port/linux/src/dsound_sdl.c`)
- **Floating point.** `_controlfp`/`_statusfp` map the MSVC control word onto the FPSCR
  (`port/linux/src/msvc_crt.c`). The 750 has no square root instruction, and newlib's `sqrtf` works
  a bit at a time (about 290 instructions); the link wraps it (`-Wl,--wrap=sqrtf`) with WiiLibs'
  (`wl/gfx/gfx_sqrt_wrap.c`), which refines `frsqrte`'s estimate and gives the same correctly
  rounded root in about 60. WiiLibs' tests (`tests/gfx/test_gfx_sqrt.c`) compare it with the
  host's `sqrtf` bit for bit, from estimates as coarse as the 750's (`WL_TEST_EXHAUSTIVE=1`: every
  float); `nv2a_cpu_native.h`'s `nv2a_sqrtf` is the same function (`wl_sqrtf_exact`), and
  `tools/wii_nv2a_check.py` builds it from WiiLibs. The game calls it for every vector's length
  (`square_root`).
- **Movies.** Bink movies are skipped (`source/bink/bink_playback.c`).
- **Sound.** The shared DirectSound mixer (`port/linux/src/dsound_sdl.c`) fills WiiLibs' audio
  stream (AESND, 44.1 kHz). Every 240 frames the log reports blocks mixed and played, underruns,
  the time a block takes to mix, and the loudest sample. In the menu: no underruns, the music
  near full scale, about 0.6 ms to mix a block (measured at 32 kHz, with the other platforms'
  mixing).
  - The other platforms resample every voice to the output's rate with a 48-tap windowed sinc.
    That took two thirds of the processor in b30 (the first sampling profile, `mix`): counted
    under qemu-ppc, a 22 kHz voice cost 530 instructions an output frame at 32 kHz and a 44 kHz
    one 3,900, its low pass stretched tap by tap. The Wii mixes each voice at its own rate
    instead ("the Wii's mixing" in `dsound_sdl.c`): the game's 22 kHz voices on a 22,050 Hz bus
    and its 44 kHz ones on the 44,100 Hz output, each frame as it is; one half-band low pass
    (32 taps) doubles the low bus for all of them; a voice the game plays at another frequency
    (a sound's random pitch, an engine) is interpolated onto the output by a cubic
    (Catmull-Rom). The gains, pans, low passes and room sends are the other platforms'.
  - The figures (`python tools/harness/tests/test_mixer_wii.py metrics`): a 22 kHz voice flat
    within 0.01 dB to 9.5 kHz with the images 70 dB down, 1.8 dB down at 10.5 kHz; a 44 kHz
    voice exact; a pitched one 0.2 dB down at a fifth of its rate and 2 dB at two fifths, the
    cubic's images 33 dB down at a fifth, above 16 kHz for a voice played near its rate.
  - Counted (instructions an output frame, the scene of `mixer_wii.c`'s "bench 12 2 2 8": 12 mono
    22 kHz voices in the world, two of them pitched, two stereo 44 kHz and two stereo 22 kHz
    voices, 8 out of earshot, loud enough to be limited, the reverb off): 861 with ADPCM (38
    million instructions a second, 5.2% of the processor), 656 with PCM, against 15,100 at 32 kHz
    (480 million, two thirds of the processor, as in the profile) with every voice resampled and
    the packets decoded beforehand. Of the 861: 240 decoding ADPCM, 160 making the samples floats,
    300 the voices' gains, filters and interpolation, 100 the limiter, 70 the upsampler. A voice
    out of earshot costs about 1, the mix with nothing playing 50.
  - **The reverb** (`audio.reverb`, on) is the other builds' network (taps, diffusers, a
    feedback delay network) from the same I3DL2 properties, so a room keeps the decay time, the
    reflections and the level the game set. Run as they do (22,050 Hz, eight lines) it added 476
    to that scene, half as much again, and `audio.reverb` was off on the Wii. It now runs at
    11,025 Hz on four lines ("The Wii's reverb" in `dsound_sdl.c`): the room send is decimated by
    a 1 2 1 low pass and the reverberation interpolated back up linearly, so the high frequencies
    of the reverberation end near 5 kHz; while the room holds, a frame uses its one set of
    parameters (the crossfade between two rooms, over 186 ms, is the other loop); and a voice
    that sends to the reverb has a loop of its own (the general one made a sending voice cost 12
    an output frame). It adds 113 with ADPCM and 112 with PCM (`bench 12 2 2 8` with flags 3 and
    1, against 2 and 0; 0.7% of the processor), and its state is 64 KB, not 276.
    `python tools/harness/tests/test_mixer_wii.py reverb-figures` prints, for decay times from 0.3
    to 8 s, the time an impulse takes to fall 60 dB (within 7% of the time asked for; the eight
    lines at 22,050 Hz: within 12%) and the energy out for low noise in (0.6 to 0.8, the eight
    lines 0.7 to 0.9, the room level being 0 mB). The harness checks the decay time within 15% and
    the level; the steady loop against the crossfading one (same output); a change of room (the
    fade ends, nothing unlike a number); silence; and the voice's loop against its sums in double
    precision.
  - **Sound setting.** The console's mono, stereo or surround (SYSCONF `IPL.SND`, WiiLibs'
    `wl_platform_sound_mode`) is read when the audio starts (the log says `audio: playing in
    mono|stereo`). Mono plays the mean of the two channels in both, at the end of the mix after
    the limiter (`mix_mono`: 10 instructions an output frame, 1% of the scene's, when it is on);
    surround is played as stereo, with no Pro Logic encoding. `audio.speakers` (Wii only)
    overrides it: `"console"` (default), `"stereo"` or `"mono"`.

## Memory

The Wii has 24 MB of MEM1 (fast; the program is loaded there) and 64 MB of MEM2, of which IOS 58
keeps the top (above `0x933E0000`) and libogc a little at the bottom. `src/wii_memory.c` claims
two fixed ranges from WiiLibs (`wl_platform_mem_claim_fixed`) before anything else runs, the game
state's in MEM1 and the window in MEM2; WiiLibs' heaps (`src/wii_boot.c`) take what is left of
MEM2, and newlib's malloc what is left of MEM1.

The tables give every range in KB, before (the build whose b30 run ran out: MEM1's heap full
before the first frame, the MEM2 heap's 122 failed allocations, decoded textures and the
profiler's ring refused) and now. The executable's part is `llvm-readelf -S build/wii/halo.elf`;
its largest symbols are `llvm-nm -S --size-sort -r build/wii/halo.elf`.

**MEM1** (`0x80000000`–`0x81800000`, 24576 KB):

| Range | Holds | Before | Now |
| --- | --- | ---: | ---: |
| `0x80000000`–`0x80003F00` | the system's: exception vectors, the loader's and the OS's globals | 16 | 16 |
| from `0x80003F00` | the code (`.init`, `.text`) | 3677 | 3614 |
| then | read-only data and data: `.rodata` 380, `.eh_frame` 73 (unwind tables nothing reads), `.data` 502 | 965 | 967 |
| then | zeroed data (`.sbss`, `.bss`): below | 12571 | 7896 |
| to `0x81340000` | libogc's arena: newlib's heap (the "MEM1 heap": all the game mallocs first, WiiLibs' two video framebuffers among it, 600 KB each at 480 lines and 718 at 574) and libogc's own (about 64 KB) | 2996 | 7220 |
| `0x81340000`–`0x813C0000` | the GX command FIFO (WiiLibs' `WL_TAG_GXFIFO`, 524,256 bytes in a 512 KB heap): the GP reads it from MEM1, where libogc's examples put it; on the console a FIFO in MEM2 is not supported. It was 1 MB in MEM2 | 0 | 512 |
| `0x813C0000`–`0x81800000` | the game state: 4 MB for the CPU's part (the Xbox's 3,165,260 bytes of pools and the port's additions), 256 KB for the GPU's | 4352 | 4352 |

What changed in MEM1:

| What | Before | Now | How |
| --- | ---: | ---: | --- |
| the tag validator's claims (`tag_validate_claims`, a bit a byte of the tag cache) | 2944 | 0 | They cover only the region being checked, in the tag cache's free bytes: the tags' above the tags, a bsp's between the tags and the bsp (bsps load at the tag cache's top), else malloc'd while the bsp is checked (b30's is 790 KB, a10's largest 1020: the heap's, not MEM1's for good) |
| the game's profiler's frame history (`profile_globals`) | 1103 | 40 | 8 frames, not 256 (`source/cseries/profile.c`; the profiler is the Xbox debug build's) |
| the game's stack (`game_stack`) | 1024 | 512 | the deepest seen is about 290 KB |
| the window's page tables (`window_protection`, `window_blocks`) | 96 | 51 | 16 bits a page |
| the console's framebuffer (in the MEM1 heap) | 600 | 0 | the bring-up console writes into one of WiiLibs' two |
| the game state revert's copy (malloc'd, 4352: never had) | | | read into the game state, as the Xbox did (`source/saved games/game_state_xbox.c`) |
| the sounds' packets, decoded (malloc'd as the game queued them: 8 to 32 KB each, up to 4 a voice) | | | read where the sound cache keeps them as they play, an ADPCM block of 64 frames at a time into the voice (`port/linux/src/dsound_sdl.c`; the same samples) |

The largest zeroed data now: `render` 692 (the game's render globals), `ai_debug` 534 (the AI's
debug state, whose layout is asserted), `update_client_globals` 513 and `update_server_globals`
128 (`source/game/player_queues_new.c`), `late_joiner_players` 512
(`source/networking/network_server_manager.c`), `game_stack` 512, the distributed netcode's
damage history (`port/linux/game/network_damage.c`: `damage_trails` 384, `damage_history` 192,
`damage_explosions` 161) and batches (`network_distributed.c`, `distributed_batches` 151),
`widget_walk` 256 (the
validator's walk of the menus' widgets, `tag_schema_effects.c`, sized for 65536 tags),
`local_lens_flare_occlusion_test_results2` 256, libogc's Bluetooth buffers
(`memb_mem_pool_pbufs`) 200, `custom_edition_maps_globals` 200 (the Custom Edition map list,
though the Wii runs none), libogc's main thread's stack (`s_mainStack`, which only waits once
the game's thread starts) 128.

**MEM2** (`0x90000000`–`0x933E0000` as IOS 58 leaves it, 53120 KB):

| Range | Holds | Before | Now |
| --- | --- | ---: | ---: |
| `0x90000000` to libogc's MEM2 arena | libogc's IOS buffers (the memory report's window line says where the arena began) | a few | a few |
| from there to `0x90100000` | before, no one's; now the window's lowest pages, which only its top-down blocks reach | about 1000 | in the window |
| `0x90100000`–`0x91700000` | the tag cache (22528 KB), at the window's bottom: a map's tags (at most 16.2 MB, b40) and the structure bsp loaded at its top (at most 8.6 MB, c20) | 22528 | 22528 |
| top-down from the window's top | the texture cache (12288 KB, the game's own cache of the map's bitmaps, converted to GX's formats), the sound cache (4096 KB), Direct3D's resources (vertex and index buffers, textures the game makes, render targets: b30 1.4 to 3.8 MB) and what is free | 22528 | 23040 and the pages below |
| window, in all | `0x90100000`–`0x92D00000`, now from libogc's arena to `0x92D80000` | 45056 | 45568 and about 1000 |
| above the window | WiiLibs' heaps: the game's heap (the MEM1 heap's overflow) 5120 then 6144 (it took the GX FIFO's 1024 when that moved to MEM1), thread stacks 256 then 64 (40 in use: the audio thread's 24 and the buttons' watcher's 16), the audio output's buffers 256 then 64 (4 in use), asynchronous reads 256 then 64 (none in use); the rest WiiLibs' reserve (192) | 7040 | 6528 |

So the game's malloc has the MEM1 heap and the game's heap together: about 8 MB before (2.9 and
5.1), about 13 MB now (7.05, of which the two framebuffers take 1.2, and 6.0 less the 64 KB of
the accounting's tables). Moving the GX FIFO into MEM1 took 512 KB from the first and gave the
second 1024: 512 KB more in all. MEM1's table still adds up to its 24576 KB (16 + 3614 + 967 +
7896 + 7220 + 512 + 4352).

**The GX FIFO** is `WL_TAG_GXFIFO` in MEM1 (`src/wii_boot.c`: the tag's region and 512 KB budget,
and `memory.claim[WL_MEM1] = WL_MEM_CLAIM_NEEDED`, so that WiiLibs takes only the FIFO's 512 KB
from the top of libogc's MEM1 arena, below the game state, and leaves the rest to malloc).
`wl_gx_init` sizes the FIFO by the heap (`wl_gx_fifo_bytes`). WiiLibs samples the FIFO's fill at
each frame's end and the log says each new peak by 64 KB steps ("FIFO fill peaked at N of M
bytes", tag `GX`) and at shutdown; the Direct3D device logs the FIFO overflows (`GX FIFO
overflows so far`). A frame that fills it makes the processor wait, so a peak near 512 KB, or
overflows, are the sign to look at.

**How malloc works.** The game's heap is newlib's, in what MEM1 leaves. WiiLibs' heap overflow
(`wl/platform/heap_overflow.h`, its `wii/heap_wrap_wii.c`) wraps the allocators, newlib's
internal ones included, so that what MEM1 cannot hold comes from a 6 MB heap in MEM2
(`WII_MEMORY_TAG_GAME`). The link wraps the names in WiiLibs' `WL_HEAP_OVERFLOW_WRAPS`
(`cmake/WiiLibs.cmake`, which `tools/wii_build.py` reads) and pulls the wrappers in with
`-u __wrap_malloc`; `__wrap_main` turns the overflow on with `wl_heap_overflow_enable` once
WiiLibs' heaps are up. Its blocks carry a 32-byte header, so they are 32-byte aligned; the log
says so at the first one, and "malloc(N) failed, from ..." (addresses for `addr2line` on
`build/wii/halo.elf`) when both heaps are full. `GlobalMemoryStatus` (WiiLibs' `sysconf`,
`wii/posix_wii.c`) counts MEM1's free bytes and the overflow heap's free budget.

**The memory report.** Every 240 frames the log has:
- `memory: MEM1 heap N KB used, N KB free` (newlib's `mallinfo`);
- `memory: decoded textures N KB of 1536 in N, N given up for others (N KB)`: the textures the
  device decodes into GX's RGBA8 in the heap (those the converter could not leave in a GX format
  in the texture cache, and those the game makes and writes). They are kept to 1.5 MB, least
  recently drawn going first; when the heap has no room for one, decoded textures are given up
  (and freed at once, once GX has drawn what it was given) until it has;
- `memory: the game's stack at most N KB deep of 512`;
- `memory: MEM2 window ... at ADDRESS, N KB: N KB used, N KB free (N KB together)`: the largest
  free run is what the biggest new resource can have;
- WiiLibs' heaps, one `[mem]` line each (`game` is the overflow heap: its use, peak and failures);
- `[heap] by call site` (WiiLibs' `heap_sites.h`, turned on in `src/wii_boot.c` with
  `wl_heap_overflow_track_sites`): the blocks of 1 KB or more in both heaps by the two return
  addresses above the allocator, the 16 sites holding the most (`N KiB in N blocks (peak N KiB, N
  made) from ADDRESS ADDRESS`, for `powerpc-eabi-addr2line -f -e build/wii/halo.elf`), and the
  smaller blocks by size.

## Map conversion

`tools/wii_convert_maps.py` reads the tag layouts from `build/wii/halo.elf` (the DWARF types and
the linked tag schema, `tools/wii_tag_layout.py`). The Wii build's tag structures are laid out
exactly as the i386 build's, and none use C bitfields. A map is converted as follows:

- **Tags.** Every field of every tag is byteswapped, its pointers into the tag cache moved from
  0x803A6000 to 0x90100000. The schema's structures stand byte arrays (`reserved...`,
  `unused...`) for fields its validator doesn't check. Where the game's own declarations of the
  tag's structure name fields there, those are swapped too (`fill_placeholders` in
  `tools/wii_tag_layout.py`); the sky's fog colours are an example. A declaration that stands
  bytes for the tag's start and names only fields within the placeholders counts too
  (`source/ai/actions.c`'s vehicle_definition: the AI's charge and strafing values at 0x390,
  which only the banshees set). The data blocks are converted
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

  Each level is converted by WiiLibs' `wl/codec/xbox_tex.h` (`wl_xbox_to_gx`,
  `wl_xbox_dxt_to_cmpr`, `wl_xbox_dxt_alpha_i4`). `ninja wii` builds those sources as a host
  library, `build/wii/tools/libwl_xbox_tex.so`, which the converter loads with ctypes. Before
  using it, the converter checks that it makes the same bytes as the converter's own numpy
  conversions on a test texture of every format. Without the library (`--numpy-textures`, or a
  WiiLibs without the codec), numpy converts the bitmaps itself, more slowly. Either way the
  output is byte for byte the same on all 24 maps. The levels kept, the sphere maps and the
  padding are the port's own and stay in `tools/wii_gx_textures.py`.
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

`tools/wii_placeholder_check.py map.map...` checks that the game reads none of the tag bytes the
converter leaves as they are (the placeholders and gaps its notes list as holding data). It
converts the maps in memory, takes the bytes of each that hold data in any map, and looks for reads
of them in the Wii build:

- the game's declarations (the DWARF types of `halo.elf`) of every structure of the tag's names, or
  bound to the tag by the code, in every file, where none of their fields contradicts the schema's;
- the code: clang's AST of each of the build's files, with the build's flags (`ninja -t commands`),
  and every value read through a pointer into a tag, at an offset the file's own declarations
  give: a field, the address of a field cast to a wider type (vehicles.c's
  `(struct physics_variable_speed_parameters *)&definition->unknown330`, 16 bytes), and offsets
  added to the tag's pointer (`(byte *)unit_definition + 0x17c`). A tag's pointer is one of a
  type of its names, what `tag_get` returns for its group (or a parent's), or a tag block's element
  (`TAG_BLOCK_GET_ELEMENT`), whose type is then bound to the block's definition.

It reports each value of two bytes or more read over bytes that hold data (a byte reads the same
either way), and exits 1 if there is one. It finds both earlier misses when they are put back (the
slide's acceleration at 0x338 with `75dfe227` reverted, the AI's charge values at 0x390 with
`fill_placeholders`' tail views turned off). On the 24 maps the game reads none of the 17
placeholders that hold data. It can't follow a tag's pointer kept as `void *` or `byte *`, or an
index that isn't a constant. `--trace DEFINITION` lists every read it finds into one definition.

## The picture on each TV

The game draws a 640x480 picture for a 4:3 screen, 30 frames a second. WiiLibs
(`wl/platform/video_mode.h`; `src/wii_video.c` is the port's side) chooses the mode from the
console's settings and logs it once at the device's creation, as `video: 480i NTSC 59.94 Hz, XFB
640x480, EFB 640x480, line 640 at 40+0, 4:3 (display.wii_widescreen = "pillarbox")`.

| The console is set to | The video is |
| --- | --- |
| NTSC (or MPAL) | 480i at 59.94 Hz |
| progressive scan on, with a component cable | 480p instead (the cable is looked for; Dolphin always has one) |
| PAL with EuRGB60 on (most PAL TVs take it) | 480 lines at 59.94 Hz, as NTSC: the game runs as it does anywhere |
| PAL with EuRGB60 off | 576i at 50 Hz; with progressive scan and a cable, 576p |
| 16:9 | the 640-wide framebuffer is widened to a line of 704 pixels; the picture is kept 4:3 (below) |
| a display position (the console's settings) | added to the line's origin by libogc, kept on the 720-pixel line: up to 40 pixels either way on a 4:3 setting, 8 on a 16:9 one |

- **16:9.** The video interface stretches the line it scans over the whole screen, so a 4:3 picture
  came out a third too wide. `display.wii_widescreen = "pillarbox"` (the default) narrows it: the
  interface can widen a framebuffer but not narrow one, so at the end of each frame WiiLibs copies
  the finished picture (the HOME menu included) out of the EFB, clears the EFB to black in the same
  copy and draws it back as one filtered quad over columns 80 to 560 of 640 (`wl_gx_target_squeeze`);
  the interface then widens the 640 columns and the 480 of them are three quarters of the screen: 4:3,
  circles round. The cost is one 640x480 EFB copy and one quad a frame, the picture 25% less sharp
  sideways, and a 1.2 MB block of memory, the one the render targets park the back buffer's colour
  in (`efb_park_alloc`, in the contiguous window when there is room; the two share it). With no
  memory the picture is shown stretched (one warning in the log). `"stretch"` fills the screen as
  before. A 4:3 console setting is not touched. The game's projection is unchanged: a real
  anamorphic mode (drawing all 640 columns in a 16:9 picture) would widen the HUD and the field of
  view, and is left for a third value of the setting.
- **PAL 50.** The EFB's 480 picture rows are copied to the XFB and scaled to its 576 lines
  (libogc's `TVPal576IntDfScale` has an EFB of 480 rows; the 48 rows below, to 528, are scratch, the
  view's sphere maps among them, and are never copied to the screen). A display of 50 Hz cannot show
  30 frames a second at two retraces each (that is 25), so `wii_video_present_interval` asks the
  display queue for 30 frames a second as a ratio of retraces: alternately two and one (the 3:2
  unevenness of film on television), 30.0 a second. The game's clock ticks 30 a second whatever is
  shown, so its speed is right at 50 Hz, with uneven frame lengths; on EuRGB60 they are even.
- **Checkpoint runs** (Dolphin, which ignores the line's width, so it shows the pillarbox as the
  framebuffer has it: black at the sides; a PAL 50 run needs a PAL region): `--wii-aspect 16:9`
  with `display.wii_widescreen` `"pillarbox"` and then `"stretch"`; `--wii-progressive`;
  `--wii-eurgb60 off` on a PAL NAND for 50 Hz. `wl_console_tests` prints the mode
  (`INFO video: ...`) and checks it against the console's settings.

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
    program). A blended vertex is its two nodes and its weight (up to 64 blends a draw). Up to
    nine values a draw get a position matrix each (`GX_PNMTX1` to `GX_PNMTX9`), which GX picks per
    vertex by its matrix index. Where the draw's values are more than nine, or some blend is past
    the 64, a value needs six of the draw's indices to have a matrix. The other vertices (those of
    a value without one) are the CPU's: their position is sent in the same draw, through the
    identity (`GX_PNMTX0`) and the same projection, and every pass of the model makes the same
    choice, as it depends on the vertices alone.
  - A model's fog from its nodes (the lit programs with the atmospheric fog, `cfc6f3a4`) is a
    function of the world position, which each node's position matrix maps the vertex to by an
    affine map the camera makes invertible: so it is one affine function of that matrix's
    result for every node, and for the blended vertices the CPU positions through `GX_PNMTX0`.
    Texture coordinate 4 is made of the position through the vertex's own position matrix (its
    matrix index) and a post-transform matrix (`GX_DTTMTX1`), solved from the first node's
    matrix and fog row and checked against the program at a vertex of every other node
    (`fog_post_matrix`, `fog_post_check`); where it does not hold, the draw is the CPU's. Such
    draws were all the CPU's: b30's opening drew 6,400 of them, 3.7 million vertices, in 60
    frames. The bench's fogged characters (below) give each vertex's fog coordinate within
    0.00001 of the CPU's path.
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
    it ran 7.9 million, and a frame took about 160 ms in Dolphin then (it took 200 to 450; the introduction has
    the latest figures).
    `debug.gpu_transform = false` keeps every program on the CPU.
  - What is the same from draw to draw is kept, not found again (a draw's cost is mostly its
    setup):
    - an indexed draw's index range, and a model part's node keys, by a hash of its indices (two
      32-bit lanes, about two instructions an index) and a copy of its vertices' node indices and
      weights, in 256 index records (`draw_range_find`, `node_keys_find`);
    - the constants a program reads, hashed a row at a time from hashes kept as the rows are set
      (`constant_rows_changed`: the exclusive or of the rows read, or of all 192 for a program
      that indexes them), where every word of every row read was hashed after any constant was
      set;
    - the streams GX reads, flushed from the CPU's cache once under their buffer's serial (the
      header's `Lock`, cleared by `D3DVertexBuffer_Lock`), not at every pass: a serial's first
      frame is flushed at every draw, so a buffer the game locks each frame always is
      (`stream_flush`);
    - whether a draw's indices name only kept outputs already computed (its index record notes
      the entry), so the scan is skipped;
    - the pixel engine's state (blend, updates, depth, alpha test, culling), written only where it
      changes (`states_shadow`), and only the texture coordinates and colour channels the
      stages read generated.
    - `tools/wii_draw_bench.py` measures it without a Wii: `port/wii/tests/draw_bench.c` includes
      `src/d3d8_gx.c`, compiled as the Wii build compiles it, over a stand-in of libogc's GX
      (`tests/host/gx_stand_in.c`), draws synthetic scenes the game's way (a structure's
      sections, rigid, skinned, unlit and fogged characters) and runs under `qemu-ppc`: `count`
      gives the instructions a draw by function, `record` a digest of what GX would draw
      (`--save`, `--compare`, `--device` another copy of the device's sources). Later frames'
      draws, `wii-port` at a5581ec6 then now: the structure's 164,076 instructions then 43,304,
      the rigid models' 92,971 then 25,154, the skinned characters' 225,373 then 171,176, the
      fogged ones' 292,638 then 174,271. With `BENCH_PROBE` it lists the GX state an immediate
      draw finds after each draw, with `BENCH_TRACE` each vertex's fog coordinate as GX makes it,
      and `BENCH_CPU` puts every program on the CPU.
  - An indexed draw on the CPU runs each vertex's program once, however many indices name it.
  - A draw on the CPU plans its vertex elements once: only the registers its program reads, each
    type with its own case, bytes and packed normals converted by table. Each vertex then computes
    only what the stages read: the program skips instructions that reach no output sent
    (`nv2a_cpu_run_live`), and the texture scale, sphere map and fog table apply only where set.
  - At build time, `tools/nv2a_codegen.c` decodes the game's 67 programs with the interpreter's own
    decoder (`src/nv2a_cpu.c`) and writes each one as a C function (about 180 KB of code). Each takes the
    outputs a draw reads, and runs an instruction only if it reaches one of them (found backwards
    from the end, a register's component at a time). A program is found by a hash of its words.
    For the 23 skinned programs (those that index their constants by a0) it also writes a
    function of the position alone for four values of v0 at once (`nv2a_cpu_run_positions`):
    what reads nothing of v0 (the nodes' blend) once, the rest for each point, operation for
    operation as a run does, so a position matrix's four runs (and the depth's fit's two) are
    one call, the same to the bit (`tests/nv2a_native_check.c` compares them with runs; about
    76 KB more code). A skinned model makes one for every key of every part each frame.
  - Square roots and ARL's floor give the libraries' results without their calls
    (`nv2a_sqrtf`: frsqrte refined in double, rounded, then corrected by the midpoints' exact
    squares). A skinned vertex takes about 1,600 instructions where it took 3,400.
  - Any other program runs on the interpreter.
  - A cube map sampled as a sphere map at a direction the vertices give (a model's reflection)
    no longer keeps its draw on the CPU: GX reads the position and the other coordinates by
    index, and the CPU computes only the sphere map's coordinates (and the colours, where they are
    its), sent with each vertex. In b30 these programs (`8f831b43`, `9d2757d2`, `cfc6f3a4`,
    `10c30c53`) were nearly all the CPU's vertices. The program itself saves little (1110 to 1108
    instructions a vertex for `8f831b43`: its skinning and lighting are most of it); each index
    sends 7 words to the FIFO in place of 14.
  - **Static buffers' outputs kept.** An output that reads no constant is, for a vertex, the
    same whatever the draw's constants (`nv2a_cpu_analyze_constants`; the generated programs
    list them as "the vertex's alone"). Where what the processor computes for such a draw (a
    sphere-mapped stage's coordinates, or both colours) reads no constant and the streams it
    reads are static, it is computed once a vertex, kept beside the buffers in the contiguous
    window (at most 1 MB, and only while the window keeps 1 MB more free in one run), and GX
    reads it by index as it reads the buffers (`static_outputs_find`).
    - A vertex buffer is static while the game does not lock it: its header's `Lock` holds a
      serial the device gives it, which `D3DVertexBuffer_Lock` clears. The kept outputs are
      keyed by each stream's header, data, stride and serial, the program, the shader and the
      stages and colours kept, and computed only from the frame after they were first seen (a
      buffer locked every frame is never kept). Entries not drawn for 120 frames are freed.
    - The environment's bumped lightmap pass (`10c30c53`) is what it is for: its texture
      coordinate 3, the incoming light's direction (v7) in the tangent frame (v1 to v3), was the
      processor's for every vertex, every frame (a10: 33,000). Its colours read the lights'
      constants and stay the processor's, as they are with `display.bumped_lightmaps = false`.
    - Of the 67 programs, 34 have an output of the vertex's alone; those the processor computes
      in GX's draws and that can be kept, where their buffers are static: a cube map's direction
      (`10c30c53` t3, `e44b4813` t1 and t2, `918a94c8` t3, `67d0128c` t1, `4f0f94bd` t0 to t2,
      the normal in t2 of `693a1023`, `77aa0f41`, `cb99676b` and `ff0b9366`, where those stages
      are cube maps) and the colours of `9c72fc7c`, `af3b677a` and `9a8643d5` (the vertex's own,
      v9). Not kept: a colour of the vertex's beside one that reads constants (D1 of `27f6eaae`,
      `d7379dd1`, `ddbd4abe`, `6b9ad0b3`, D0 of `63152d86`), single components of a colour (D0's
      alpha of the lit models' programs), the bumped reflections' and specular lights' frames
      (`3f6a2f54`, `dc6482ca`, `16128183`, `6e1e2f91`: the processor's draws, `draw_run`, which
      also send the eye, a constant), and coordinates that are affine and not a cube map's (GX's
      texture matrices already).
    - `tests/nv2a_native_check.c` checks that every component the analysis finds reads no
      constant is the same, to the bit, for other constants, on the interpreter and in the
      program's function for those outputs alone. The log's `static buffers' outputs kept` line
      counts the draws that read them, their vertices and those computed, and the memory.
  - In the menu, the draws take 10 ms a frame compiled, against 58 ms interpreted.
  - **The models' lighting on GX's lights** (`display.gx_lighting`, on by default on the Wii;
    `src/gx_lighting.c`). The models' lit programs (`cfc6f3a4`, `9d2757d2`, `8f831b43`: the
    reflection, lights and planar fog permutations of a skinned model; `71f9f272`, `1d3f5b13`,
    `e6e006c7`) light each vertex by its skinned normal, and take a cube map at the eye's
    reflection about it: about 1,100 of a vertex's 1,200 instructions on the hybrid path above.
    GX now makes those outputs itself:
    - GX lights in the space its position matrices make, which the device's (x w, y w, -w) is
      not (its x and y are pixels, its z the depth). That is K E, K upper triangular with a last
      row of (0, 0, 1), E the eye's space, turned and moved from the world's: the position
      matrices make E, the projection is multiplied by K, the depth's row is the same. The
      normal matrices are E's rows of each node's matrix.
    - The normal is three floats, from the map's 11:11:10 word as the programs read it, decoded
      once for each static buffer into an array beside it in the contiguous window (at most
      768 KB, and only while the window keeps 1 MB more free in one run), which GX reads by
      index as it reads the positions (`kept_normals_find`; three floats in the buffer it reads
      where they are). It was decoded and sent for every index of every draw, 58,000 a frame in
      b30. A draw with vertices whose position is the processor's (a blend beyond the draw's
      matrices) sends every normal, those vertices' the processor's skinned one.
    - COLOR0 is D0: the ambient colour, a light far along each distant light's direction (and
      one opposite for a translucent shader's back side), and each point light. COLOR1A1 is
      D1, the Fresnel colour c-80 + c-81 (n.e): two lights at the eye, signed
      (`GX_DF_SIGNED`), one for the channels where c-81 is positive and one, with a negative
      attenuation, for those where it is negative. D0's alpha is 0, or the planar fog's.
    - The planar fog's alpha (`8f831b43`: the Pelican, the first-person weapon, b30's
      scenery at sea level) is a function of two values affine in the world position, the
      depth below the fog's plane (c-87) and the distance ahead (c-86), constant beyond 0 and
      1. A 32 by 32 intensity texture (the ramp, written once for each fog's c-85.y and z,
      four kept) holds it, read at texture coordinate 5, which GX makes of the position
      through the vertex's own position matrix (to E) and a post-transform matrix
      (`GX_DTTMTX0`, E to the ramp's coordinates). The pixel shader is translated again with
      D0's alpha that texture's (`gx_psh_textures.d0_alpha_map`, a map of 4 to 7 the stages
      leave free), so the vertices send no alpha. The fog is then the fog's at each pixel,
      where the Xbox interpolates the vertices'. The product with the atmospheric fog
      (`1d3f5b13`, `e6e006c7`), a draw with no map free or whose coordinates at its centre
      are more than 256 ramps from it, and a pixel shader beyond GX with it keep the
      processor's alpha, computed from the world position.
    - A point light's fall-off, max(1 - d^2/r^2, 0), is not a form GX has: its reciprocal's
      second-order expansion about the draw's distance gives GX's k0 + k1 d + k2 d^2. Its spot,
      clamp(cos s + o, 0, 1), is a parabola through the same cut-off, 1 on its axis.
    - The reflection's cube map (the world's sphere map, a mirror ball looking down the world's
      z) is read at texture coordinates GX makes of the normal through a matrix for each node
      (given with each vertex, as its position matrix is): the normal in a frame turned toward
      the eye along the draw's direction (rounded to 15 degrees), x / 2 + 1/2 and y / 2 + 1/2.
      Its sphere map of the view is drawn from the world's in the 48 rows of the EFB below the
      back buffer, 48 texels square, a 24 by 24 grid each of whose points is the world's
      sphere map at the reflection a normal there makes of the eye straight along the frame,
      then copied out, for each cube map and rounded direction. The direction's elevation and
      azimuth are rounded in the world, the frame's x level, so the map is the same however the
      camera turns and is drawn again only when a draw's direction from the eye crosses a step
      (rounded in the eye's space, a moving camera drew every map again each frame: b30, riding
      the Pelican, 25 a frame). 64 are kept, in the contiguous window (9 KB each), the one drawn
      longest ago replaced; at most 24 are drawn in a frame, and a draw past those takes the kept
      map of its cube map whose frame is nearest its own. With 32 kept, fewer than b30's draws
      need, every map was drawn again every frame (583 to 1,862 in 60 frames). A draw's stages'
      texture matrices take two rows each (30 to 39, by their first row) and the node matrices
      the ten pairs 40 to 58: every one of a draw's nine keys and its blended vertices'. With
      eight (rows 39 and 45 to 58), a part of nine keys had every vertex's reflection
      coordinates from the processor (the program run whole, then the sphere map's square
      roots). Row 60 is
      `GX_IDENTITY`, which every draw in screen space reads its texture coordinates through: the
      first build put the eighth and ninth nodes' matrices over it, and every such draw after came out as
      streaks. Each frame now begins with `GX_IDENTITY` loaded again, and the display copy's
      source, scale and filter are set again before the frame's copy.
    - `tests/gx_lighting_check.c` (run by `tools/wii_nv2a_check.py`) runs each program on the
      interpreter for random scenes (a camera, a skinned model of nodes within 40 degrees of
      each other, lights about it) and compares what GX makes of the same vertices: D0 with
      the distant lights 0.8 to 0.9/255 on average (the largest, 30/255, where a blend of
      two nodes shortens the programs' normal, which GX makes unit again), with point lights
      1.8 to 2.6/255 (their fit; largest 166/255), D1 0.6/255, the planar fog's alpha exact
      on the processor and from GX's ramp 0.25/255 on average at the vertices (largest
      1/255; GX's bilinear filter in 128ths of a texel) and 0.32/255 halfway between two
      (largest 1.5/255; against the programs' interpolation 0.41/255, largest 20/255), the
      position within 0.007 pixels. The reflection's direction is 7 degrees off on average
      (25 with the eye straight ahead of every draw, 3.3 with each draw's own direction), and
      the grid's interpolation of the world's sphere map 1.4 texels in 128 on average (the
      largest across the mirror ball's rim, straight down).
    - On the processor, a vertex of these draws takes 17 instructions (the normal's decode and
      the FIFO's words), 129 with the planar fog's alpha, where the hybrid path took 920 to
      1,200 (`qemu-ppc` counts, with GCC's code: `cfc6f3a4` 920 to 17, `9d2757d2` 1,149 to 17,
      `8f831b43` 1,200 to 129). The planar fog's ramp takes `8f831b43`'s back to the others'
      (a harness of the loop's lit vertex, with its call: 193 to 54, 206 to 66 with a node's
      matrix index, about 140 fewer). A vertex whose position is the processor's (a blend beyond the
      draw's matrices) still runs the program for its position and is skinned for its normal.
    - The log's `lit by GX` lines count the draws, their vertices, those refused (no such space,
      more lights or colours than GX holds), the view's sphere maps drawn and the draws that took
      a near one, the draws whose planar fog was the ramp's (and the ramps written) or the
      processor's, and the draws whose normals GX read by index, the normals decoded and the
      kilobytes kept. (The log keeps some 200 characters of a line: the statistics' lines are
      short of it.)

  GX then draws the results:
  - A program that divides by its clip-space w is sent as (x·w, y·w, −w) through a perspective
    projection. GX then clips at the eye and interpolates textures with perspective.
  - The depth is fitted per draw as a + b/w from a few of its vertices. This is exact for the
    game's projections.
  - Draws whose w doesn't vary, such as the interface's, are drawn in screen space.
- **Pixel shaders.** The register combiners are translated to TEV stages (`src/gx_psh.c`).
  - The stages are a WiiLibs TEV program (`wl/platform/tev.h`): its builder, register pool and
    merging of what a stage's inputs need of its order and konst selections. WiiLibs' validator
    checks each program as it is translated, and `wl_tev_apply_unchecked` sets it, with the swap
    tables it reads through.
  - After translation, dead halves are removed, a half alone in its stage moves into another
    stage's free half where nothing between depends on it, and each stage's order is reduced to
    what it reads (`stages_compact`): about half the stages of the random set, three of the menu
    planet's twelve, and fewer texture coordinates and colours asked of the vertices.
  - A program is translated once for its structure (`src/gx_psh_cache.c`): its combiners, its
    textures, and each constant's signature (which channels are 0 or 1, which are equal, which
    constants are equal). Where the translation's result changes with the constants only in the
    konst and preloaded values that hold them (two more translations with other constants of the
    same signatures show it), a draw with other constants rewrites those values in place. The
    game sets a model's fog constants from its distance, so a cache by the whole of the render
    states translated b30's model programs for nearly every draw (about 215 a frame, often all
    ten of a refusal's attempts). `tests/gx_psh_cache_check.c` compares every program the cache
    gives with a fresh translation; the log's `pixel shader programs` line counts the
    translations, the programs kept and changed, and the changes translated again to check them.
  - Three of the game's programs are recognized and made by hand (`gx_psh.c`'s recipes):
    - the models' (`set_environment_shader_pixel_shader`: base, detail, multipurpose and cube
      maps, eight combiners), which keeps more values alive than TEV's four registers: with the
      cube map bound the general translation refused it ("no free register"), and every model
      with a reflection (the Pelicans, the Marines, the ring in b30's sky) was the fallback's base
      map times its diffuse colour, its detail, self-illumination, change colour, reflection and
      fog left out, and its alpha that times D0's (0 for a lit model). The recipe reads the
      multipurpose map's channels through swap tables (the game's dot products pick them),
      keeps its values in PREV, REG0 and REG1 and the fifth constant in REG2, in 11 to 14
      stages, exactly; `tools/wii_gx_psh_check.py` checks all 432 variants (detail masks and
      functions, doubled or not, both fogs, the cube map or not, D0's alpha a ramp's or not)
      against NV2A's combiners (`nv2a_eval`: the detail's biased add is signed, so 2 t1 - 1
      darkens, which the general translation's clamped model does not), to 0/255;
    - the structure's fog pass (`_rasterizer_environment_fog_begin`), in seven stages, exactly,
      which with both density maps and the game's constants (b30's sea) ran out of registers;
    - the specular lights' pass from a lightmap (below).
    A fallback (a program still beyond GX) asks only texture coordinate 0 and colour 0 of the
    vertices, which is all it reads.
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
    - A dot product of a register's colour is still taken as 1 (no swap reaches its channels),
      but for the specular pass below.
  - The environment's specular pass from its lightmaps (its eight programs: bumped or with the
    bump map as a mask, overbright, extra shiny) is recognized and made by hand
    (`specular_lightmap_recipe`): its r.l, a dot product of a register, is 2(n.e)(n.l) - e.l.
    - n.e and n.l are signed dot products as above, sharing the bump map's loads (nine stages),
      clamped to [0, 1] (NV2A keeps them signed).
    - e.l is the vertices', of their eye and light directions normalized, in colour 0 as
      (1 - e.l) / 2: no stages are left for a third dot product.
    - The self-shadowing, the lightmap's intensity, the Fresnel colour and the powers are exact.
      The Fresnel colour times the intensity is clamped after the shadows and brightness
      multiply it (NV2A clamps it first). When extra shiny, the two more squarings leave no room
      for n.l, which is then the flat normal's. With the bump map as a mask, only e.l is not exact.
    - The alpha (NV2A's mask for the alpha test, 0 only where the colour is) is 1.
    - The host check compares each with NV2A's combiners evaluated signed: a mean error of 0.03
      to 0.1/255 with the mask, 0.5 to 1/255 bumped, 3/255 bumped and extra shiny.
    - The specular pass of a dynamic light reads a 3D texture (the distance attenuation) GX
      has no form of, and still draws nothing.
  - A bumped reflection (texture modes DOTPRODUCT, DOT_RFLCT_DIFF, DOT_RFLCT_SPEC) reads its
    cube map's sphere map through GX's indirect unit, as GX's environment-mapped bump
    (`GX_SetTevIndBumpST`) does:
    - The bump map is bound again as map 5 through a TLUT of its normals' x (alpha, the
      indirect unit's s) and y (intensity, its t).
    - Per vertex (on the processor), texture coordinate 3 is the sphere coordinates of the flat
      normal's reflection (the tangent frame's rows in coordinates 1 to 3, the eye in their w),
      and 1 and 2 how far they move as the normal tilts along the frame's x and y.
    - Before each lookup, two stages that read no texture take coordinate 1 times the bump's
      x and add coordinate 2 times its y (GX's dynamic S and T matrices); the lookup adds its own
      coordinate (`bumped_lookups_place`, `indirect_apply`). This is the reflection's move made
      linear per vertex.
    - The Fresnel term's normal (DOT_RFLCT_DIFF's lookup) is the flat one, per vertex, in
      colour 1. The combiners are then translated as the flat cube map's, exactly (14 stages).
    - A reflection whose bump GX cannot follow (the water's: DOTPRODUCT in stages 1 and 2, its
      bump map the ripples the game draws each frame, not a palette of normals; or any with
      `display.bumped_highlights` off) reads the cube map's sphere map at the flat normal's
      reflection, its coordinates computed per vertex as above (`gx_psh_textures.reflected`).
      Without it the water's reflection pass added nothing, and b30's far sea, where nothing
      lies under the water, was black.
  - (1 - x)p + xq is TEV's lerp, one stage.
  - Constants read as 0 or 1 need no register. Those needed in every channel take the four konst
    registers first, and any beyond four are set in TEV registers kept for them.
  - The fog register reads the vertex's fog factor, sent as a fifth texture coordinate into a
    ramp texture whose intensity is its coordinate (WiiLibs' `wl_gx_ramp_bind`), and
    `D3DRS_FOGCOLOR` as a constant.
  - A program that doesn't fit GX's 16 stages and 4 registers is translated again with these
    simplified, and each approximation says why. One that still doesn't fit draws its first
    texture times its diffuse colour.
  - `tools/wii_gx_psh_check.py` simulates the TEV stages against the combiners, for random
    programs and the game's. It also counts WiiLibs' validator's findings and compares WiiLibs'
    evaluator (`wl_tev_eval`) with its simulation. It finds WiiLibs from `WIILIBS`, else
    `./WiiLibs` (the CI's), else `../WiiLibs`.
- **Culling.** `D3DRS_CULLMODE` names the screen winding to discard. Both vertex paths keep the
  screen's winding. GX's own convention is measured once at start-up: the device draws a
  clockwise triangle with `GX_CULL_FRONT` and reads the pixel back.
- **Render targets.** A draw into another surface takes over the EFB. WiiLibs does it
  (`wl/platform/gx_target.h`: the owner, the copies, the quads); the device says which surface
  each draw goes to, records the targets that were copied, and sets its own vertex and texture
  coordinate state again after a quad:
  - The back buffer's colour and depth are parked (in the contiguous window where there is room,
    else the heap).
  - The target is drawn at the EFB's origin and copied into its own memory in GX's format, where
    its texture is bound from.
  - The back buffer returns through a quad that writes the parked colour, and the parked depth
    through a Z texture.
  - A clear once the frame has begun is a quad.
- **Textures.** A map's bitmap is bound straight from the texture cache in its GX format. Any
  other texture is decoded to RGBA8 into a 1.5 MB cache, such as the glyph cache, which is
  re-decoded when its pixels change. The game writes some of those itself (the glyphs, the
  fallback checkers), with 16- and 32-bit texels in the Wii's byte order, so the decoder, which
  reads them as the Xbox stored them, is given a swapped copy. The device marks a texture's
  header once it has seen its pixels (D3D's `Lock` field: the entry it found them in), and a
  marked texture is bound without being looked up, hashed or flushed again. The game clears the
  field when it loads a bitmap into the texture cache, and `d3d8_resources.c`'s lock functions
  clear it before the game writes a texture (the glyph cache's new glyphs); a texture outside the
  texture cache is hashed once a frame as well. GX's texture cache (TMEM) is emptied only when
  texture memory has changed (a decode, a bitmap loaded, a copy out of the EFB) and at each
  frame's start, not before every draw. A cube map is bound as its sphere
  map, and each vertex's direction is mapped onto it (on the CPU): the normalization cube map's
  too, whose texel is the direction a bump map's dot product takes.
- **Texels the processor reads.** The game reads some texels itself: an object is lit by the
  lightmap under it, and by the diffuse texture there (`object_lights.c`'s `sample_lightmap`,
  through `bitmap_2d_get_pixel`). The Xbox's swizzled or DXT addressing finds another texel of a
  GX texture (on a lightmap, often the black between its pieces, which drew a10's crewmen and
  consoles nearly black), so on the Wii `bitmap_2d_get_pixel` asks
  `port/wii/src/wii_bitmap_texel.c` which levels the converter rewrote and reads the texel from
  GX's tiles: the Xbox's texel value for the game's own conversion, or for DXT the CMPR block as
  the GPU decodes it, with DXT3/5's alpha from the I4 levels. `tools/wii_gx_texel_check.py`
  checks it on the host against WiiLibs' conversions of every format.
- **Depth.** A program's position matrix and its depth's a + b/w come from its constants alone,
  from two points along w's gradient. Both vertex paths take them, so a surface's passes meet
  their equal-depth tests whichever path each pass took. A draw whose vertices are all at one w
  takes the last fit's depth: any draw's whose depth was fitted, on either path.
  - The z bias (`D3DRS_ZBIAS`, a polygon offset of -bias depth units and -bias/4 times the
    depth's slope on the Xbox) has no GX form: a biased draw's depth is pulled toward the eye by
    bias / 16384 of its distance (b of a + b/w). The decals (bias 8, `LESSEQUAL` over their
    wall) were not drawn with GX's transform on: the wall's depth is GX's and the decal's the
    CPU's, a few units apart (a10's screens beside the cryo bay's red doors).
- **Registers no stream gives.** A stream draw's program reading a register no stream gives
  reads what `D3DDevice_SetVertexData` last set, as NV2A keeps it (`draw_unstreamed`): the
  detail objects' (grass) cells set their place in register 2 and their reference vector in 3,
  then draw their quads from the one buffer. The matrices' keys include those values.
- **Frame buffer.** The EFB has an alpha channel (RGBA6_Z24), because the game blends by
  destination alpha.
- **GX kept from hanging.** GX's transform unit can hang on a clipped primitive that follows an
  EFB copy, a draw-sync token or a draw-done. libogc's `GX_SetMisc(GX_MT_XF_FLUSH,
  GX_XF_FLUSH_SAFE)` names the remedy, and the device sets it, but libogc's `GX_Begin` sends
  nothing for it (its `gx.o`, disassembled: the value is only kept). So after each of those the
  device's next primitive is preceded by eight vertices of zeros in its own vertex format (its
  size from libogc's vertex descriptor and the formats the device set), which are one point and
  draw nothing (`gx_begin`; `debug.gpu_xf_flush_safe`, on by default). Where WiiLibs' GX wait (250
  ms) runs out, at the frame's draws or its display copy, the log says where GX stood: its status
  flags, the GP FIFO's read and write pointers and fill, the CPU FIFO's write pointer, the last
  token passed and the flushes sent (`GX did not finish in time`).
- **No vertex of a position and matrix indices alone.** GX misreads a vertex whose descriptor
  holds only its indexed position and matrix indices, which a skinned model's pass whose stages
  read no texture coordinate from its vertices and whose colours are constant would be. Such a
  draw sends a colour of 0 (RGB565, two bytes) that neither channel reads; the processor's draws
  of a matrix index and nothing else read send their first colour as 0. A build with assertions
  checks every primitive's descriptor (`vertex_shape_check`).
- **Debugging.** The desktop ports' `debug.gpu_debug_flat` and `debug.gpu_debug_texture0` (in
  `config.toml`) show the diffuse colour, or the first texture, alone. One frame's draws are
  described in the log (`DRAW_LOG_FRAME`).

### What the frame draws

The game's own passes, counted in the x86 build (headless, `debug.null_renderer`, which still
runs the game's rasterizer) with the scripted player at the same ticks as the lockstep check:
vertices a frame (each draw's range of indices), averaged over 30 frames after tick 1300, with
the Wii's other defaults (`display.model_detail = 0.5`, no object shadows).

| Pass | a10 (cryo bay) | b30 (beach) |
| --- | --- | --- |
| The environment's lightmap pass (`10c30c53`, a cube map at each vertex's direction) | 33,000 | 7,000 |
| Its texture pass, its fog pass (GX's) | 33,000 each | 7,000 each |
| Its reflections: the lightmap mask and the cube map (GX's) | 32,000 each | 3,700 each |
| Objects (skinned and lit) | 11,600 | 52,000 |
| The first-person weapon and hands | (none at that tick) | 3,200 |
| Object shadows (off on the Wii) | | 14,000 |
| Decals, particles, contrails, lens flares, the HUD | under 1,500 | under 1,500 |
| In all | 178,000 | 89,000 |

- `display.environment_reflections = false` leaves out two of a10's five passes over its
  environment: 178,000 vertices a frame become 113,000. `display.bumped_lightmaps = false`
  takes the cube map's direction out of the first pass, so GX transforms it and the processor
  computes only its colours; with it on, the direction is the static buffers' kept outputs
  (computed once a vertex, then read by GX by index), so the processor computes only its
  colours too.
- `display.model_detail` changes almost nothing here (b30: 89,700 vertices at 1.0, 91,800 at
  0.5, 91,200 at 0.25). The grunts, elites and jackals far away are already at their lowest
  level of detail (about 450 vertices a grunt), and the trees, shrubs and rocks have only one.
  b30's objects at that moment: the Pelican (10,700, its planar fog the processor's, or GX's ramp with
  `display.gx_lighting`), 12 grunts
  (5,400), about 25 trees (17,000), 4 elites (3,300).
- None of these settings changes the game's state: with each on and off, the x86 build's
  lockstep hashes agree at every logged tick (a10 to tick 88,500 for the lightmaps and 32,200
  for both settings; b30 to tick 1,400 for `display.model_detail` 0.25 to 1).

## The lockstep check

The same session, played by the x86 Linux build and by the Wii in Dolphin, compared tick by tick:
the big-endian game core against the little-endian one, with the maps the Wii's converter wrote.

- Both builds run a solo network test with `debug.fixed_ticks = true` and `debug.lockstep_log = 10`.
  Every 10 ticks they log a hash of every object's position, velocity, facing, vitality and
  parent, and the game's random seed (`port/linux/game/network_test.c`). `debug.fixed_ticks`
  takes the machine's clocks out of the game:
  - A game tick each frame (`source/game/game_time.c`), whatever time passed.
  - Every frame is 1/30 s long to what runs on the frame's time (`source/main/main.c`; not in a
    movie): the players' turning, the camera and the menus. Two x86 runs of the same build had
    differed in b30 once the scripted player had control.
  - The scripted player's clock is the game's ticks (`port/linux/src/test_input.c`), and it gives
    no input before a game's first tick, so both see the same input at the same tick.
  - A speaking unit's mouth stays closed (`game_sound_set_mouth_aperture`,
    `source/sound/game_sound.c`): it follows the sound as the audio device plays it, in real
    time, and its overlay moves the unit's nodes, from which weapons aim and fire.
  - The test's waits in seconds (`debug.network_test_start`, 15 s by default, in the menu before a
    solo game) are summed from the frames' lengths, so they count frames (450 of them), and the
    game starts at the same frame on a machine that draws at 2 frames a second and on one that
    draws at 200.
- The Linux build runs headless (`debug.null_renderer`, SDL's dummy video and audio drivers) in the
  CI's Arch Linux image, with the Xbox maps. Both builds use musl's maths functions, so the floats
  agree to the bit.
- `tools/lockstep_compare.py linux.log wii.log` names the first tick that differs. With
  `debug.lockstep_detail = <tick>` both list every object's values (and its nodes' matrices,
  hashed) at that tick, and the tool names the objects that differ.

It found `list_get`'s element index read from the wrong end of its cell, which made a10's opening
teleport player 0 to player 1's flag. With that fixed, a10's opening (the cryo bay, the cinematic and
its skip, the scripted player) agrees at every tick of 2,600.

## Where the processor's time goes (the sampling profiler)

The log's frame split (draws, textures, pixel shaders, the end of the frame, the rest) says which
half of a frame is slow; the profiler says which functions. WiiLibs' sampling profiler
(`wl/platform/profile.h`; its README has the design, the cost and the caveats) takes the decrementer
interrupt's view of the processor 500 times a second: where it was, and who called there.

It is off unless the game's folder holds `profile.txt` (`sd:/apps/halo/profile.txt`, or the USB
drive's). It is a file and not a setting of `port_config.c`, so that a normal run knows nothing of
it. The file's reading, the watcher thread, the ring and the dump are WiiLibs'
(`wl/platform/profile_arm.h`, whose README section has the details); `src/wii_profile.c` reads it
at start-up and gives the watcher what is Halo's, the three hooks that say when to sample. The
file is lines of `name value` (`#` starts a comment; a name WiiLibs does not know is logged as a
warning):

```
# sd:/apps/halo/profile.txt: "name value" lines; these are the defaults
seconds 20     # how long to sample once the level is playing
after 10       # seconds of the level's game time to let go by first
hz 500         # samples a second (1 to 10000)
depth 12       # callers kept per sample (0 to 16)
skip 1         # maps to let load first: the main menu is the first
repeat 1       # levels to profile one after another (a tour of levels)
log 1          # also write the samples to the log (Dolphin's OSREPORT)
note b30       # words for the header of the dump
```

The watcher asks the game's hooks every tenth of a second. `ready`: a level is playing (the game
clock running, no map loading, the clock past `after` seconds, `skip` maps already loaded; a map is
told by its load, or by its clock starting over). `stop` and `advanced`: the next map has started
to load, which ends the profile early and, in a tour, lets the next profile wait for its level.
The profile lasts `seconds`, then `profile<N>.txt` is written beside `profile.txt` and, with
`log`, the same lines go to the log. The ring is malloc'd (700 KB for the defaults), memory the
game then does not have. `tools/wii_profile.py` writes the file and runs the tables:

```sh
tools/wii_profile.py setup --note b30 --repeat 2        # build/wii/sd/apps/halo/profile.txt; prints the config.toml lines
tools/wii_dolphin.py --seconds 900 --until "[profile] done" > build/wii/profile.log   # a checkpoint run
tools/wii_profile.py report build/wii/profile.log --top 40 --callers update --tree 2 --timeline
tools/wii_profile.py setup --remove                     # the game profiles nothing without the file
```

`wl_profile.py` prints the threads (the game's is the busiest), the functions by their own samples
and by samples anywhere on the stack, `--callers F` (who calls F, where its time goes), `--tree`,
`--hot` (instructions, with lines), `--timeline` and `--collapsed` (flame graph input). Under
Dolphin it counts instructions executed (about one cycle each, no cache or bus waits, and a sample
is taken at the end of a translated block); on the console it counts where the processor waits as
well. Use Dolphin's profile to find what the game does, and the console's to find what it costs.

### The hardware counters (what the frame waits for)

The profiler says which functions; the counters say which part of the console the frame waits for,
and so name the bottleneck of a frame that misses its 33 ms: the processor (and whether its memory,
its branches or its instruction count) or the GPU (transform, fill and TEV, textures, vertex fetch).
They are WiiLibs' `wl/platform/hwcount.h` (its README section "Hardware counters" has the registers,
the groups and the verdict's rules), armed here by a second control file, `hwcount.txt` beside
`profile.txt` (`sd:/apps/halo/hwcount.txt`, or the USB drive's); `src/wii_profile.c` calls
`wl_hwcount_arm` at start-up and `wl_gx_begin_frame` and `wl_gx_end_frame` do the rest. Without the
file a run is unchanged. It needs a console: Dolphin does not model the counters, and the log then
says `cpu: no counters` and `gp: no counters` (and prints the frame times and the FIFO's fill).

```
# sd:/apps/halo/hwcount.txt: "name value" lines; these are the defaults
seconds 1      # a block of log lines every so many seconds
cpu 1          # the Broadway's performance monitor (PMC1 to PMC4)
gp 1           # GX's counters
vcache 1       # the vertex cache's counters with each GPU group
timeline 1     # marks (draw-sync tokens) and the FIFO fill sampler
fifo_hz 480    # the FIFO sampler's rate
selftest 1     # check that the counters count
first 60       # frames to let go by (a level's load) before measuring
raw 0          # also log each group's raw counters (set 1 on the first run)
```

Each frame measures one CPU group (cycles on PMC1, three events on the others: instructions and L1
data misses and mispredicts; miss stall cycles; L2; branch stalls; the instruction mix; TLB) and one
GPU group (the XF/RAS busy and wait clocks, or a pair of metrics: transform or lighting clocks,
vertices, texels, triangles, quads, texture-memory stalls, texture-cache misses, vertex-cache
stalls); the groups take turns, so a rotation is 42 frames and a metric is sampled about five times
a second. A block, once a second, in the log (Dolphin's console, or `apps/halo/logs` on a console: item 1 of "What only a real Wii can show") under the tag `hw`:

```
[hw] block 12: 30 frames, frame 38.10 ms (worst 52.40 ms), cpu waits: gpu 1.20 ms display 0.00 ms | gpu done at 14.30 ms
[hw] cpu: ipc 0.52 | L1D miss/ki 18 stall 31.0% load 18.0% | L2 miss/kc 5 hit/kc 5 castout/kc 20
[hw] cpu: br mispredict/ki 6 stall 0.0% lr/ctr 1.0% | fp 9.0% int 45.4% | dtlb search 1.0% itlb 1.0% | kcycles 23571
[hw] gp: xf wait-in 72.0% wait-out 1.0% | ras busy 15.0% | xfrm 8.0% lit 2.0% bot 3.0% | clk 243.0 MHz
[hw] gp: verts 30000 tris 10000 quads 50000 texels 400000 | tc miss/ktexel 20 tx stall 2.0% idle 40.0% | vc stall 1.0% vcache miss 10.0%
[hw] fifo: mean 6.2% max 6.2% of 1048576 bytes (30 samples)
[hw] verdict: CPU-bound (gp starved: xf wait-in 72.0%, fifo 6.2% full, gpu wait 3.0%); cpu: memory stalls (dcache stall 31.0%, 18 L1D miss/ki)
```

(Those numbers are made up, from the host test's fake machine; the first real ones are the console's.)

How to read it:

- **`frame` and `worst`** are the processor's frame-to-frame time. `cpu waits` is the time the
  processor spent blocked in `wl_gx_sync` and the end-of-frame wait for the GPU, and for a free
  display buffer (the retrace limiter). The processor's counters leave those waits out; everything
  else (the game, the drawing, the sound thread, the interrupts) is counted. `gpu done at` is when
  the GPU had finished the frame, from the frame's start.
- **The verdict.** `CPU-bound (gp starved ...)`: the XF waits for input (`xf wait-in` 60% or more of
  the GP's clocks), the FIFO is nearly empty and the processor hardly waits for the GPU: the frame
  is made on the processor, and the part after it says why: `memory stalls` (the L1 data miss stall
  cycles are 30% of the cycles; look at the profiler's functions that walk data), `branches`,
  `pipeline stalls` (IPC under 0.6 without a memory or branch cause: load-use and floating-point
  latency) or `compute` (instructions flow; there are too many of them). `transform-bound`,
  `fill/TEV-bound`, `texture-bound` and `vertex-fetch-bound` name the busiest GPU part, with the
  figure that decided it; `GPU-bound (mixed)` is a busy GPU with no part over its threshold; `fits`
  is a frame under 35 ms that waits for the display. These thresholds are starting values, set from
  the counters' definitions and not from a console: if the first run's verdict disagrees with the
  numbers above it, the numbers are the evidence and the thresholds
  (`wl_hwcount_thresholds_default`) are what to change.
- **First run on the console.** Set `raw 1`. The `armed` line and the self-test lines say whether
  the counters count: `gp: counters count (drew 6 vertices; slots read 6 0 6 0; perf0 on slot 0,
  perf1 on slot 2)` confirms the register map (the self-test needs the renderer to call
  `wl_hwcount_selftest_begin`/`_end` around its known-vertex draw; without it, the GPU group set is
  checked by whether any group reads non-zero); `cpu: no counters (cycles read ...)` means the
  performance monitor did not follow the time base. `clk 243.0 MHz` is the GP's measured clock
  (a result far from 243 means the nominal clock the pairs' shares are computed with is wrong, and
  they follow the measured one). A `raw` line per group lists the four counter slots: if a pair's
  second metric is not on slot 2, the raw lines show which slot moves.
- **The marks.** `wl_hwcount_mark(id)` (0 to 15, from `d3d8_gx.c` or the game) sends a draw-sync
  token and the block's `marks` line shows, per id, the mean time into the frame at which the
  processor gave it and the GPU passed it, `cpu/gpu` in ms; a growing gap is the GPU falling behind
  at that point of the frame (the world, then the HUD). A mark flushes the write-gather pipe, so
  keep them few.
- **Brackets.** A wait for the GPU outside WiiLibs' own (`GX_DrawDone` in `d3d8_gx.c`) is counted
  as the processor's time and spins the processor's counters; bracket it with
  `wl_hwcount_wait_begin(WL_HWWAIT_GPU)` and `wl_hwcount_wait_end(WL_HWWAIT_GPU)`.

What to look for per level (and report with the level's name): the block's `frame` against 33.3 ms,
the verdict at the slowest and fastest moments, the three or four numbers that decided it, and the
`raw` lines once. A level that is CPU-bound with memory stalls is a data-layout problem; one that is
CPU-bound with `compute` wants fewer instructions (the profiler's top functions); a texture or
fill verdict wants fewer or cheaper passes (the draw counts in the frame split); a transform verdict
wants fewer vertices through the GPU (`gpu_transform` and the vertex programs).

## The playthrough check

The game played as a person plays it, through the menus, rather than started by the network test:
`debug.test_input = "script:<file>"` plays controller 1 from a file of steps
(`port/linux/src/test_input.c` lists them: wait for a menu screen or focused widget by its tag's
name, for play, ticks or a checkpoint; press, hold and move the sticks; walk to a point of the
level). `tests/playthrough.txt` is a first session: the main menu, CAMPAIGN, a new profile, NORMAL,
a10's opening skipped, the cryo tube's look training and its exit, a walk across the cryo bay,
SAVE AND QUIT from the pause menu, the profile's settings, and the saved game loaded and played.

- Copy it to `apps/halo/playthrough.txt` on the SD card, empty `apps/halo/saves` (the script
  makes the first profile), and set in `apps/halo/config.toml`:

  ```toml
  [display]
  menus = "xbox"

  [debug]
  test_input = "script:playthrough.txt"
  ```

  with no `debug.network_test` (the menus must run) and no `debug.fixed_ticks`.
- The log has a line for each screen and focused widget as they change (`test_input: screen
  "ui\shell\main_menu\main_menu", focus ...`), each step as it begins, with the script's clock,
  each `log` line with where the player is (`step 9: ... (at -57.2 -3.0 0.0, facing -94
  degrees, tick ...)`), and each checkpoint saved, reverted or loaded (`game state: ...`). It ends
  with `test_input: script finished`, or `test_input: script stopped at line N (...): timed out`
  and what was shown then. Its last step quits, which on the Wii goes back to the loader.
- The Wii's controllers and buttons have steps of their own, for the HOME menu and the notices
  (`tests/home_menu.txt` plays them over a10, with a list in its header of the log lines to look for):

  ```
  home                              HOME pressed and let go, on any Remote
  remote <port> off|on|real         Wii Remote 0 to 3 gone, there (with nothing pressed, if none is), or as reported
  extension <port> none|nunchuk|classic|real
  battery <port> <percent>|real
  event reset|power                 the watcher thread sees the console's button (logged; nothing leaves)
  ```

  They change the raw input before `xinput_wii.c` reads it, so the game's own handling of a Remote
  that leaves (`remote 0 off` is a controller removed: its pause message) is what is tested; and while
  the HOME menu is up the scripted controller's up, down, a and b (`press down`, `press a`) steer it.
  `remote 0 on` makes a Remote of its own when there is none (Dolphin with no Remote connected).
  `tools/wii_input_check.py` runs the verbs against the real `test_input.c` on the host.
- The x86 Linux build plays the same script (`HALO_MENUS=xbox HALO_TEST_INPUT=script:...`, headless
  or on a virtual display with `debug.screenshot_every`) to the same screens and places.
- The Xbox's pause menu has no settings (the PC version's has); the settings are reached from the
  main menu. a10's first checkpoint after its opening comes past the cryo bay's tutorial (the red
  and yellow squares, the panels to look at, the shield test), which the script does not play yet:
  the saved game is the opening's checkpoint.

## The controllers

`src/xinput_wii.c` is the Xbox's XInput over WiiLibs' raw input (`wl_platform_input_poll`);
`src/xinput_wii_map.c` says what each button is. Port N (the player on controller N, four at
most, so the game's split screen works) is the Nth Wii Remote, with its Nunchuk or Classic
Controller; when no Remote is connected there, the Nth GameCube controller. A controller that
comes or goes is a controller inserted or removed for the game, as on the Xbox; a Remote that
loses its data for a moment (a Nunchuk plugged in) stays, for up to two seconds.

The game's controls are its defaults: the left stick moves, the right looks, A jumps, B melees, X
is the action and reload, Y switches weapons, the left trigger throws a grenade, the right fires,
Black switches grenade type, White is the flashlight, a click of the left stick crouches and a
click of the right zooms, Back shows the scores, Start pauses. In the menus A accepts, B and Back
go back, and the D-pad or the left stick moves.

Sticks and triggers arrive from WiiLibs over their whole travel (the GameCube's sticks are
full at the end of their gates, its triggers run from 30 to 180 as `PAD_Clamp` takes them, the
Classic Controller's five-bit triggers are scaled to a byte). The game applies its own dead zone
to the sticks (`input_xbox.c`), so none is added here. A trigger is pressed fully when its digital
button is (the Classic Controller Pro has nothing else).

Where a controller has fewer buttons than the Xbox's, the D-pad's buttons do two jobs in play
(a press is the D-pad and the Xbox button). While any menu is up they do only the D-pad's, so
that a menu's left, right, up and down never also mean Back, a click or a weapon.

| Xbox control | Classic Controller (and Pro) | Wii Remote + Nunchuk | GameCube controller |
| --- | --- | --- | --- |
| Left stick (move) | left stick | Nunchuk stick | main stick |
| Right stick (look) | right stick | the pointer, below | C stick |
| A (jump; accept) | b | A | A |
| B (melee; back) | a | 1 (and B in the menus) | B |
| X (action, reload) | y | D-pad down | X |
| Y (switch weapons) | x | D-pad up | Y |
| Left trigger (grenade) | L | Z | L |
| Right trigger (fire) | R | B | R |
| Black (switch grenades) | ZL | D-pad left | Z |
| White (flashlight) | ZR | D-pad right | D-pad right |
| Left stick click (crouch) | D-pad down | C | D-pad down |
| Right stick click (zoom) | D-pad up | 2 | D-pad up |
| Back (scores) | - | - | D-pad left |
| Start (pause) | + | + | Start |
| D-pad (menus) | D-pad | D-pad | D-pad |

The console's reset and power buttons, and a Remote's power button, are served by the watcher
thread ("Reset, Power and quitting", above). The Remote without a Nunchuk has no way to move (its
buttons and pointer work, and the game tells the player so: "The notices", below); the Classic
Controller's pointer is not used; the Wii U Pro Controller is not supported (its stick clicks are
not read).

**The HOME button** (a Wii Remote's, or a Classic Controller's own; a GameCube controller has none)
opens a menu over the game, `src/wii_home.c` and `src/wii_home_game.c`:

```
HOME
> Resume
  Reset to title
  Return to the loader

WII REMOTE BATTERIES: REMOTE 1 [=========   ] 80%      A: CHOOSE   B OR HOME: RESUME
```

- **Resume** closes it. **Reset to title** is the game's way to its main menu
  (`main_goto_main_menu`), as if the level were left without saving (SAVE AND QUIT in the pause menu
  saves); it is not offered while a menu is up already (no level running). **Return to the loader**
  asks first, "Unsaved progress will be lost", with Cancel chosen, and then leaves for the Homebrew
  Channel as the menus' Quit does (`platform_request_quit`: the saves finish, the audio and motors
  stop, the SD card is written out). The battery gauges list each connected Remote.
- Any controller steers it: the D-pad or the stick moves (wrapping), A (the Classic Controller's
  lower button b) chooses, B, 1 (the Classic Controller's a) or HOME again goes back or resumes.
- While it is up the game is paused as its pause menu pauses it (`game_time_set_paused`; a game
  paused already, by its own menu, is left as it is when the menu closes), the sound fades out over
  300 ms and back over 300 ms when it closes (WiiLibs' `wl_platform_audio_fade`, at the output, so
  the game's mixer is untouched), the rumble is off, the pointer turns nothing and the game is
  given a controller with nothing pressed. A button held as it closes (the A that chose Resume)
  does nothing in the game until it is let go, or for a second and a half.
- It is drawn by WiiLibs at the end of the frame, over the finished picture (`wl_gx_set_overlay`,
  called from `wl_gx_end_frame`, which `D3DDevice_Present` ends its frame with; 5 by 7 capitals in a
  built-in font, `wl/platform/overlay.h`), and the hook is set only while there is a menu or a notice
  to draw. A closed menu costs the frame nothing; the controllers are looked at where the game reads
  them anyway (`xinput_wii.c`), a few comparisons a read, and the battery every two seconds, not each
  frame. The menu's draws are a few hundred flat quads.

**The notices** (the same overlay, a box at the top; they pause nothing and take no controller):

- *A Remote with nothing to move with*: in play, on a controller with a player, with no Nunchuk or
  Classic Controller for a moment and a half (a Nunchuk plugged in is read as none for a moment
  first), the box says "Wii Remote 1: Connect a Nunchuk or Classic Controller to move" for seven
  seconds. Once, until something has been plugged in and pulled out again. The game's own message
  (the Xbox's "controller unplugged", `source/input/input_abstraction.c`) still pauses the game when
  a Remote drops.
- *A weak battery*: once, when a Remote's battery (0 to 100, from its status reports; a 0 before
  the first nonzero reading is not known yet) falls below 10%: "Wii Remote 2: Battery low (8%)".
  Told again only after it has been 15% or more.

**On a run without a person** (`debug.test_input = "script:<file>"`, "The playthrough check") the
script's `home`, `remote`, `extension`, `battery` and `event` steps do the same as the hardware
would, through the same code (they change the raw input that `xinput_wii.c` reads, before the HOME
menu and the game see it), and the scripted controller's D-pad up and down, a and b steer the HOME
menu. `tests/home_menu.txt` plays them.

**The pointer** (a Remote without a Classic Controller) aims like a mouse's direct look
(`halo_linux_mouse_look`): the view does not turn while the pointer is within 35% of the half
screen of the middle, in either direction. Outside that box it turns toward the pointer, slowly
at first (the square of the distance from the box's edge) and at the player's own look speed
(the profile's look sensitivity) at the screen's edge. A pointer to the right turns right, one
above the middle looks up. Aim magnetism is off while the pointer is turning the view and on
within the box, where it helps to hold a target. When the sensor bar is lost the last position
carries on for 150 ms, then the view stops. The profile's inverted look does not apply to it.

**Rumble.** The Xbox controller has two motors, each given a strength from 0 to 65535 (the game
sets them every tick: `input_flush_rumble` in `input_xbox.c`, which sends zero while paused, in the
console or out of a game); a Remote's and a GameCube controller's has one that is on or off. The
game's call (`XInputSetState`) only records the stronger of the two strengths and when it was asked,
and zero while any menu is up. The motor is switched by a service on WiiLibs' watcher thread
(`wii_rumble_tick`, every 16 ms, 62 times a second) with a duty cycle (`wii_rumble_duty`,
`xinput_wii_map.c`): the motor is on for a fraction of the ticks equal to the strength (an error
that carries from tick to tick: half strength is on one tick in two, nine tenths on nine in ten),
off at once for strength zero, and not switched again within 10 ms of its last switch, so at most a
hundred switches a second go to the Remote whatever the rate. It stops when the game has not asked
for 250 ms (a map loading, a halt), when the controller is gone (its strength is cleared and the
motor's state forgotten, so that one that comes back starts still), when the game quits or the
console is reset or powered off. The console's setting (the Wii's Remote vibration, `BT.MOT` in
SYSCONF, read by libogc's `CONF_GetPadMotorMode`, through WiiLibs' `wl_platform_rumble_enabled`)
is kept: with it off no motor, GameCube controllers' included, is switched on.

Checking the HOME menu: the same script builds `tests/wii_home_check.c` with `src/wii_home.c` (the
state machine against fake controllers, a fake game and a clock: opening and closing, the pause
made once and undone only if it was the menu's, the sound's fades, the items and the loader's question,
the buttons held as it closes, every controller's navigation, HOME of the Classic Controller only on
one, the notices' debounce, their once and their expiry, the battery looked at every two seconds, and
nothing asked while closed) and `tests/wii_script_check.c` with the real `test_input.c` (a script of
the verbs played a read at a time, and lines that must be refused). WiiLibs' tests cover the frame
overlay, the font, the audio fade and the simulated buttons (`test_platform_overlay`,
`test_platform_audio_fade`, `test_platform_shutdown`).

Checking the mapping: `tools/wii_input_check.py` builds `tests/wii_input_check.c` for the host
with WiiLibs' `input_shape.c` and maps synthetic input states, comparing the Xbox gamepad with
this table (each button on its own in play and in the menus, every Xbox control reachable from
each controller, the sticks' signs and extremes, the triggers, the pointer's turn and its loss) and the rumble's duty cycle (the fraction of ticks
on for nine strengths at 60 and 30 ticks a second, at most a hundred switches a second at ticks a
millisecond apart, zero stopping the motor at once).
WiiLibs' own tests cover the reading (`test_platform_input_wii`: the backend against stand-ins
for libogc's, with the sticks' and triggers' ranges and a Remote's drop-out) and the shaping
(`test_platform_input_shape`). On a run (Dolphin or the console) `debug.input_log = true`
(`HALO_INPUT_LOG=1`) writes each controller's gamepad state to the log whenever it changes, and
`tools/wii_input_log.py LOG --expect classic|remote|gamecube` decodes those lines into button
names and checks that pressing each button in turn gave what the table says. What the host cannot
show is in the next section.

## What only a real Wii can show

Dolphin runs the game, but these need the console. The list is in the order to do them in: the
set-up first, then what shows whether the game can be used at all, then the rest. For each item:
what to set up, what to do, and what the log or the screen should show. Report each with its
number and, where the item says so, the level's name. The requirement behind all of it is 30 frames
a second (33 ms a frame) on every level; items 12 to 14 are the ones that say where a level that
misses it loses its time.

1. **Set-up and the log.** Copy `build/wii/halo-wii.zip`'s `apps/halo` to the SD card (the Homebrew
   Channel's folder), the converted maps to `apps/halo/maps`, and make an empty folder
   `apps/halo/logs`. With that folder the game writes its log to `apps/halo/logs/log-NN.log` (NN
   the first number not used, 00 to 99, then 00 again; opened by `wii_boot.c` after the SD card and
   the USB drive are mounted, so the first lines, up to the "IOS" line, are on the debug port only;
   written 4 KB at a time and when the game leaves, stops or reports a crash, so a cut power loses
   the last lines); without the folder it writes nothing to the card, and a console has no other
   way to give the log back, so every item below that says "the log" needs it. Delete old logs now
   and then. `apps/halo/config.toml` is where the settings go (`port/linux/README.md` has every
   setting and its default).
   Look for: the Homebrew Channel starts the game; the log has the `video:` line (item 8), the
   memory report every 240 frames (`memory: MEM1 heap`, `memory: MEM2 window`, the `[mem]` lines;
   no `malloc(N) failed`), and `a frame takes N ms` every 60 frames; the menu is up with its music;
   a stop screen instead names its cause (missing maps, the fixed memory, a stub).
2. **The controllers in the hand.** `debug.input_log = true` in `config.toml`, a run that only
   goes to the menu and in play, then for each controller (Wii Remote with Nunchuk, Wii Remote with
   a Classic Controller or Classic Controller Pro, and a GameCube controller on a port with no
   Remote on that player's number) press every button in turn, slowly, and move each stick to
   its gate and each trigger through its travel. Copy the log to a computer and run
   `tools/wii_input_log.py LOG --expect remote|classic|gamecube`: it must report every press as the
   table in "The controllers" says. Look for: the sticks reach their ends (the log's `left` and
   `right` values at the extremes are the Xbox's full range, and are 0 at rest, with no drift that
   the game's dead zone does not cover); the triggers' analog values rise through their travel
   (the Classic Controller's) and are full when the digital button is down (the Pro's); a
   Remote's drop-out (turn the Remote away for a moment) leaves the player in place for up to two
   seconds, then the game's own "controller unplugged" pause; a Nunchuk plugged in and out in play
   does not remove the controller. The pointer (a Remote with a Nunchuk, the sensor bar in place)
   needs a person: the view rests within the middle of the screen, turns toward the pointer outside it,
   slowly then at the profile's look speed at the screen's edge, is not jittery, stops 150 ms after
   the sensor bar is lost (cover it), and the aim magnetism holds a target. The Wii U Pro Controller
   is not supported; say whether it does anything. Remote batteries: `remote N battery code C` in the
   log against the percent the HOME menu shows, with a Remote that is full and one nearly spent
   (the notice appears once below 10%).
3. **The HOME menu.** In the menu and in play, press HOME on a Remote and on a Classic Controller
   (a GameCube controller has none). Look for: the menu opens over the paused game with its three
   items and the batteries' line (every connected Remote), the sound fades out over 300 ms and back
   in, nothing is rumbling, the pointer turns nothing, the text is legible on a television; A
   chooses, B, 1 or HOME resumes; the button that chose Resume does nothing in the game until it
   is let go; "Reset to title" reaches the main menu (not offered when a menu is up); "Return to
   the loader" asks first (Cancel chosen) and then leaves for the Homebrew Channel with the log's
   `leaving (loader)`. The notice for a Remote with no Nunchuk or Classic Controller (seven seconds,
   once) and the weak battery notice.
4. **Rumble.** In a level (a firefight, grenades, a vehicle) with a Remote and with a GameCube
   controller. Look for: the duty cycle (62 switches a second at most) feels like a strength and
   not a buzz, half strength is not full strength; it stops at a pause, the HOME menu, a
   disconnect, a quit and a map loading; the console's setting for Remote vibration set to off means
   no rumble at all, the GameCube controller's included.
5. **Reset, Power and the Remote's power button.** Each of them (a) while a map loads, (b) in
   play, (c) at the menu, and (d) at the stop screen (missing maps: start once with `apps/halo/maps`
   renamed; HOME returns to the loader). Look for: Reset returns to the Homebrew Channel, Power and the
   Remote's power button put the console into standby; the log's last lines are `leaving (loader)` or
   `leaving (power off)` (tag `system`), no motor is left running, the SD card's folder is intact
   (the card on a computer shows no `.wtmp` or `.wold` in `apps/halo/saves` after the next start has
   swept them, and the file system is clean).
6. **Saves, loads and cut saves.** (a) Play a campaign level to a checkpoint, SAVE AND QUIT, start
   again and Continue: the checkpoint loads and plays. (b) Power off (the console's button) during
   SAVE AND QUIT, as the log says the save is written (`begin player profile write`, the
   checkpoint's `persistent storage`); then with the card pulled during it; start again each
   time: the log has `save: ... put back after an interrupted write` or nothing, Continue loads
   the old checkpoint or the new one, never a broken one, and the card's FAT is clean when read on a
   computer. (c) A card with no room left (fill it), and a card with its lock switch on: SAVE AND
   QUIT says it could not save (the profile's and the checkpoint's messages, `save: <file> ...`
   naming the cause), the game goes on to the menu and nothing hangs. (d) The first start with an
   empty `apps/halo/saves` (the default profiles are written once; the `files:` lines say how
   long, the second start writes none). The files the game makes again at every start are in "Files
   the game makes again".
7. **Sound.** The console's Wii Settings, Sound: mono, stereo and surround, one run each. Look for:
   the log's `audio: playing in mono` only for mono, and a mono game sounds the same in both
   ears; stereo and surround play stereo; `audio.speakers = "stereo"` and `"mono"` override the
   console's. The log's `audio:` report every 240 frames has no underruns (blocks played against
   mixed), a mixing time a block (`mixing N us a block`; about 0.6 ms in the menu in Dolphin) that leaves the
   processor its share of the frame, and a
   peak below 32767 (the report's `peak N of 32767`). The reverb (`audio.reverb`, on): indoors (b30, a30's halls and
   corridors) and outdoors, a battle indoors, with it on and off: the room sounds like a room (a
   decay that follows the place) and not metallic or muffled, going through a door fades to the
   next room's without a click, and the mixing time rises by about 0.7% of the processor's (the harness's count). Music,
   dialogue and effects all heard; the volume settings do what they say.
8. **The picture on each TV.** The console's settings (Wii Settings, Screen), a run to the menu
   and into a level each time, and the first lines of the log: `video: ...` must name the mode the
   table in "The picture on each TV" gives.
   - 4:3 and 16:9: on the 16:9 setting `display.wii_widescreen` `"pillarbox"` (the default) shows a
     round circle (the HUD's and the pause menu's) in the middle three quarters with black at the
     sides, the HOME menu included; `"stretch"` fills the screen, a third too wide. No flicker
     at the picture's edges.
   - 480p: progressive scan on with a component cable: the `video:` line says progressive, the picture is steady
     (no interlace flicker), 30 frames a second.
   - PAL: EuRGB60 on (the log says 480 lines at 59.94 Hz, the game's speed is right and the frames
     even); EuRGB60 off (576i at 50 Hz; the 480 rows of the picture scaled to the 576 lines, nothing of the
     48 scratch rows below them on the screen, the game's speed right, and the frame lengths
     alternating two and one retraces: 30 a second).
   - The display position (Wii Settings, Screen, Display Position) moved to its extremes: the
     picture moves with it and nothing is cut that the television shows; a 16:9 setting allows 8
     pixels.
9. **The SD card's read speed and the loading of a level.** From the menu, load a10, b30 and the
   largest, b40 or c20, and a saved game, on the SD card and (if you have one) a USB drive. Look for:
   the `files:` lines (about every two seconds while reads go on): the reads' count and sizes
   (`reads in 2000 ms: N, K KB in M ms`) give the card's real MB/s (the converter's maps are 33 to
   36 MB a level), the three counts `B`, `U`, `S` (buffers not on 32 bytes, offsets not on 512,
   reads under 4 KB) say whether the card's driver likes the reads, and the longest `files:` call
   (`writes took N ms` lines); the load's time from the log (the long frame, `frame N took N ms`,
   and the time of the next `a frame takes`). Dolphin's 11 seconds is not the console's; report the
   card's brand and class. A load that takes much more than the card's speed explains is the
   game's own work (tags, structure, objects).
10. **Alignment exceptions.** Play every level (a tour: `debug.network_test` with a list of levels,
    `debug.network_test_tour` seconds in each, `debug.test_input = "bot:1"`, as in "The
    playthrough check"), the menus (the settings screens, the profile, the campaign menus), a saved
    game, and the multiplayer maps if they are reached. Look for: no `*** CRASH: alignment exception`
    (nor any other `*** CRASH`, which has the registers and the callers for `addr2line` on
    `build/wii/halo.elf`) in the log. The 750 can trap a floating-point load from an address that is
    not a multiple of 4; Dolphin does not. The compressed animations' floats are read a byte at a
    time for this, and the script stack's values start at a multiple of 4 (`HS_STACK_PADDING`,
    `source/hs/hs_runtime.c`). The rest was audited with an x86 build compiled with clang's
    `-fsanitize=alignment` (a handler that logs each site), played by the bot through every campaign
    level and the 13 multiplayer maps (25,000 to 470,000 ticks each): of the misaligned accesses it
    reported, only the script stack's are floating-point loads or stores in the Wii's objects (the
    others are integer loads and stores, which the 750 completes without a trap: the script
    stack's frames and longs, the debug allocator's headers, the recorded animations' shorts, the
    breakable surfaces' flags and the tag validator's read of compressed animations), and the
    game's objects have no `lmw` or `stmw` (their one `lwarx` is on an aligned pointer). Code the
    bot did not reach (the menus past the title, saved games, network play) is what a report would
    name.
11. **GPU hangs.** The same tour. Look for: no `GX did not finish in time` (it says where GX
    stood when it gave up: the status flags, the GP FIFO's pointers and fill, the last token), no
    frame that takes seconds. The flush after an EFB copy, a token or a draw-done
    (`debug.gpu_xf_flush_safe`, on: eight vertices of zeros, which draw nothing) is the remedy
    libogc names for a hang of GX's transform unit on a clipped primitive after them; if a hang does
    show, run the level again with it `false` to say whether it is the cause. The FIFO in MEM1: the
    log's `FIFO fill peaked at N of 524256 bytes` (by 64 KB steps and at shutdown) and `GX FIFO
    overflows so far` (none): the peak and the overflows are as on Dolphin, and a FIFO that fills
    makes the processor wait.
12. **The hardware counters, per level.** `apps/halo/hwcount.txt` with the line `raw 1` (the file
    is "name value" lines, the defaults in "The hardware counters"), `profile.txt` removed (one file at a time, so that the profiler's interrupts are not in the
    counters' frames), the tour of item 10 (`debug.network_test_tour` 40 or more: `first 60` frames are let go by, then
    every second has a block). First, once, the self-test: the `[hw]` lines at the start must say
    `gp: counters count (drew 6 vertices; slots read 6 0 6 0; perf0 on slot 0, perf1 on slot 2)`
    and not `cpu: no counters` or `gp: no counters`, and the `raw` lines show which slot moves
    for every group; `clk 243.0 MHz` is the GP's measured clock (far from 243 and the pairs' shares
    are wrong: say what it was). Then for each level, with the level's name: the block's `frame`
    and `worst` against 33.3 ms, `cpu waits`, the verdict at the slowest and fastest moment and the
    three or four numbers that decided it (the rules are in "The hardware counters"), and the
    `marks` line if one is set. Verdicts: CPU-bound with memory stalls (a data-layout problem),
    with compute (fewer instructions), transform-, fill/TEV-, texture- or vertex-fetch-bound (see
    that section for what each wants). If the verdict disagrees with the numbers above it, send
    both: the thresholds (`wl_hwcount_thresholds_default`) are starting values.
13. **The sampling profile, per level.** Remove `hwcount.txt`; `tools/wii_profile.py setup --note
    b30 --repeat 10` (or by hand: `apps/halo/profile.txt` with `seconds 20`, `after 10`, `skip 1`,
    `repeat N`), and the tour of item 10 with the levels in the order of the tour: `profile<N>.txt`
    appears beside `profile.txt` for each level and the log has the samples (`log 1`). On a
    computer: `tools/wii_profile.py report profile1.txt --top 40 --callers update --tree 2
    --timeline` (the ELF is `build/wii/halo.elf`, of the build that was run). Look for: the
    functions the console waits in that Dolphin does not show (the profile counts where the
    processor waits as well, Dolphin's only instructions): the game's thread is the busiest, and
    the top functions on the console against Dolphin's. Send each `profile<N>.txt` with its level's
    name.
14. **The frame rate.** With neither file, the settings of the person's choice (the defaults first):
    each level from its start to its end, and the multiplayer maps, as a person plays it (the
    scripted playthrough or by hand). Look for: the log's `a frame takes N ms` lines (every 60
    frames: draws, textures, pixel shaders, the end of the frame, the rest) against 33 ms, the
    slowest second of each level and where it is (a battle, a vehicle, the beach's water, a10's
    cryo bay), and a picture that moves smoothly. The settings that trade look for time:
    `display.model_detail`, `display.object_shadows`, `display.bumped_highlights`,
    `display.environment_reflections`, `display.bumped_lightmaps`, `display.gx_lighting`: run the
    slowest level with each changed from its default and report which brought the frame under 33 ms.
15. **TEV's arithmetic (the bump mapping).** The bump mapping's signed sums rely on TEV's lerp with
    a bias of -1/2, its scale, and results kept unclamped between stages (-4 to 4); the host
    check (`tools/wii_gx_psh_check.py`) simulates the documented rules in 8-bit steps, and the
    console is not checked. With `display.bumped_lightmaps = true` (the default): in a10's cryo bay
    floors and walls, and b30's metal, look for banding or the wrong sign in the lightmaps' bump
    term (a light on a bumped wall lighting the side that faces it, not the opposite), then the same
    place with `false` (flat lightmap) for the difference; a screenshot of each (a camera or the
    television) with the level's name.
16. **The indirect unit's offsets (the bumped reflections).** With `display.bumped_highlights =
    true` and `display.environment_reflections = true` (they cost the frame its time, so for the
    look only): the bumped reflection's offset is the coordinate times the indirect texel less 128,
    over 256, times 2 to the matrix's scale (`BUMP_OFFSET_SCALE` in `d3d8_gx.c`, as Dolphin
    computes it), and the coordinate as scaled by the map given with `GX_TEXMAP_DISABLE`. In
    a10's cryo bay floors and b30's metal, look at whether the reflection moves too far or too little
    over the bumps; if the console differs from Dolphin, that scale is what to change.
17. **GX's lighting.** `display.gx_lighting = true` (the Wii's default) against `false` (the
    processor's per vertex, as the Xbox's programs): the Fresnel colour (D1) takes a light with
    a negative attenuation (k0 of -1) where the reflection is brighter at grazing angles, as Dolphin
    computes it. If the console clamps an attenuation at 0 the models' reflections lose their grazing
    brightness (the models' reflections at a low angle: compare the two settings). GX makes the lit normal unit length (as Dolphin does); the view's
    sphere maps are drawn in the EFB's rows 480 to 527, below the back buffer: no stripe of them
    on the television, in any video mode (item 8).
18. **The square root's estimate.** The vertex programs' square root starts from `frsqrte`, checked
    on the host for estimates as coarse as the 750's documented 1/32; Broadway's own is not
    documented. Run WiiLibs' `tests/console/wl_console_tests.dol` from the Homebrew Channel (it is
    built with WiiLibs: `cmake --build build/wii`; its `sqrt` suite, `tests/gfx/suite_sqrt.c`, runs
    the real `frsqrte`): the suite passes, and no fall to the integer root is counted. If it fails,
    the first failing input and its estimate are the evidence.
19. **Memory.** In the tour of item 10, the memory report's peaks: MEM1 and MEM2 as the console's
    IOS leaves them with the Homebrew Channel's loader. The window line says where libogc's MEM2
    began (the window reaches down to it) and WiiLibs'
    `[mem]` lines say what its heaps were given; look for: no `malloc(N) failed`, the game's stack
    well under its 512 KB, the largest free run of the window not shrinking level over level, and no
    allocation refused (the profiler's ring is 700 KB). A level that fails here is run again
    alone, to tell a leak from a peak.

## What is next

1. Speed. A skinned model's vertices go to GX through its matrix palette by node, or by blend
   where a blend has enough vertices; the CPU computes the position of the rest (the blended
   ones, 2 to 14%), and `display.model_detail` (0.5 on the Wii) chooses simpler models sooner.
   A pass whose stages read a cube map's direction or a texture coordinate from the nodes is
   still the CPU's, vertex by vertex, less the position. Next: those passes' cost (the fetch, the
   program, the sphere map and the emission per vertex), and a profile on real hardware, where
   Dolphin's timing may not hold.
2. The rest of the drawing. Bump mapping's dot products of textures and constants are exact
   (the lightmap's bump term, a light's diffuse, the flat cube map's reflection and the bumped
   one's, where its lookups are sampled). `tools/wii_gx_psh_check.py` holds the game's bump
   programs and says which are exact. Approximate: the self-illuminated lightmap's (15 stages,
   but the exact forms do not fit GX's 16 stages and four registers, so its signed dot product's
   inputs are clamped to [0, 1]), a light's alpha test's (a dot product of a register, taken as
   1), the specular pass from the lightmaps (made by hand: e.l the vertices', n.e and n.l
   clamped, n.l the flat normal's when extra shiny), and the bumped reflections (their cube
   map through GX's indirect unit, the bump's move of the reflection linear per vertex, the
   Fresnel term the flat normal's). Both are still to be seen in Dolphin against the processor's
   path (a10's cryo bay floors, b30's metal). A dynamic light's specular pass reads a 3D
   texture and draws nothing (its attenuation could be the vertices').
   Sampling the cube maps put the environment's lit pass (`10c30c53`, whose cube map direction
   comes from the tangent frame) back on the CPU: a10's frame took about 200 ms in Dolphin (150
   without them) when that was measured; the introduction has the latest figures.
3. Playing on. The campaign beyond a10's opening, and the lockstep check over longer sessions and
   more levels. Big-endian faults found so far:
   - plain char's sign (fixed with `-fsigned-char`);
   - tag fields the schema left as placeholders, one of them a contrail's shader that the game
     reads through another structure, and a vehicle's AI charge and strafing values, which only a
     declaration naming nothing else does;
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
     (`tools/wii_animation_check.py` compares every animation);
   - a profile count (`playlist_profiles_enumerate_available_to_local_player_index`) written as a
     word into a long, so the multiplayer game-type list read the other half;
   - a chicago shader's `extra_flags`, two words in the game's structure over the tag's long;
   - a fog's `flags`, a byte in the game's structure over the tag's word (it read the word's high
     half);
   - a vehicle's slide acceleration and deceleration (0x338 and 0x33c), which the schema and
     `source/units/vehicles.c` left as bytes while `physics_variable_speed_parameters` reads
     them through a cast at 0x330: now named floats, so the converter swaps them;
   - network byte order, which the shared socket layer and the transport swapped by hand (right
     only on a little-endian machine), and little-endian file values read in the machine's order
     (the TGA header's size, a tag's four-character name, the PCM samples);
   - the pregame start request: the packet carries one short, which the server decodes into the
     first two bytes of a long and read as the long's low half, its other half on big-endian
     (nothing sent changes);
   - textures the game writes itself, decoded as if in the Xbox's byte order (`d3d8_gx.c`);
   - not big-endian: a shadow's bounding radius left unset in the mirror's window, which the
     object shadow pass then read (a NaN halted c40).
4. Real hardware, from the Homebrew Channel (`ninja wii-package`).
5. CI. The workflow's `wii` job checks out WiiLibs' `Xbox` branch, which holds everything the
   port takes from it (the TEV programs, the threads, the codec, the present).
