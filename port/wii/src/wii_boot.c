/*
WII_BOOT.C

What runs before the game's main() (source/shell/shell_xbox.c) on the Wii.
The link wraps main (tools/wii_build.py), so the start-up code calls
__wrap_main here, which then calls the game's:

- the fixed memory the game is linked to, claimed before anything can
  allocate there (port/wii/src/wii_memory.c);
- WiiLibs: the reset and power buttons, the log, its heaps (sized for what
  the port uses it for), IOS 58 for USB 2.0, the SD card and a USB drive;
- for the bring-up, a text console on the screen, in one of WiiLibs' video
  framebuffers (wl_platform_console_start);
- the sampling profiler's watcher, when the game's folder holds profile.txt
  (port/wii/src/wii_profile.c);
- the game's main runs on a thread of its own with a 512 KB stack (WiiLibs'
  wl_platform_run_on_stack; libogc's
  main thread has 128 KB, below which lie its audio DSP structures: a deep
  path search in b30 ran past it, and the audio interrupt then wrote into
  the search's frames);
- the accounting of the heaps by call site, for the memory report
  (WiiLibs' wl_heap_overflow_track_sites).

A fault of the start-up, or a platform function the port does not provide
yet (the bring-up stubs of tools/wii_link.py, WiiLibs' tools/link_stubs.py),
stops the game with WiiLibs' stop screen (wl_platform_stop_screen), which
says why on the console and waits for HOME.
*/

#include <stdint.h>
#include <stdlib.h>

#include "wl/platform/heap_overflow.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"
#include "wl/platform/stop.h"

#include "wii_memory.h"

int wii_memory_claim(void);
void wii_network_start(void);
void wii_profile_arm(void);
int __real_main(void);

/* WiiLibs' heaps, all in MEM2: thread stacks (the audio output's 24 KB, and
32 KB for an IO thread), the audio output's buffers (4 KB in use), the GX
command FIFO, the asynchronous reads' buffers (which the port does not use),
and the game's malloc once MEM1's is full (WiiLibs' heap overflow, enabled
in __wrap_main). MEM1 holds the game, its game state, and libogc's malloc
heap below them. Its own scene graph, network, asset and sample bank heaps
the port has no use for. port/wii/README.md, "Memory", has the whole
budget. */
static const WlMemTagDef wii_memory_tags[WII_MEMORY_TAG_COUNT] =
{
	[WL_TAG_STACK] = { "stack", WL_MEM2, 64u * 1024u },
	[WL_TAG_GXFIFO] = { "gxfifo", WL_MEM2, 1024u * 1024u },
	[WL_TAG_HOT] = { "hot", WL_MEM1, 0 },
	[WL_TAG_AUDIO] = { "audio", WL_MEM2, 64u * 1024u },
	[WL_TAG_NET] = { "net", WL_MEM1, 0 },
	[WL_TAG_ASSET] = { "asset", WL_MEM2, 0 },
	[WL_TAG_AUDIO_BANK] = { "audio_bank", WL_MEM2, 0 },
	[WL_TAG_IO] = { "io", WL_MEM2, 64u * 1024u },
	[WII_MEMORY_TAG_GAME] = { "game", WL_MEM2, 5120u * 1024u },
};

/* the accounting of the heaps by call site (the memory report's "heap"
lines): blocks of 1 KB or more by the two return addresses that asked for
them, the rest by size; its tables in the game's heap in MEM2 */
#if defined(__has_include)
#if __has_include("wl/platform/heap_sites.h")
#define WII_HEAP_SITES 1
#define WII_HEAP_SITES_STORAGE (64u * 1024u)
#endif
#endif

/* the game's main thread: its stack, which WiiLibs fills with a pattern so the
memory report can tell how deep it has been (wii_game_stack_used); the
deepest seen is about 290 KB (an AI path search in b30) */
#define GAME_STACK_SIZE (512u * 1024u)
static unsigned char game_stack[GAME_STACK_SIZE] __attribute__((aligned(64)));

static void *game_main(void *argument)
{
	(void)argument;
	return (void *)(intptr_t)__real_main();
}

/* the deepest the game's main thread's stack has been, in bytes */
unsigned long wii_game_stack_used(unsigned long *size)
{
	if (size)
		*size = GAME_STACK_SIZE;
	return (unsigned long)wl_platform_stack_used(game_stack, GAME_STACK_SIZE);
}

int __wrap_main(void)
{
	WlMemConfig memory;
	uint32_t ios = 0;
	/* first: nothing may allocate in the fixed windows */
	int claimed = wii_memory_claim();

	wl_platform_system_init();
	wl_platform_log_init(NULL);
	wl_platform_crash_report_install();
	/* (a console that cannot start leaves the log) */
	(void)wl_platform_console_start();
	wl_platform_console_report("wii", "Halo: Combat Evolved for the Wii (bring-up build)\n");
	if (!claimed)
		wl_platform_stop_screen("The fixed memory windows could not be claimed (see the log).");

	wl_mem_config_default(&memory);
	memory.tags = wii_memory_tags;
	memory.tag_count = WII_MEMORY_TAG_COUNT;
	memory.frame_arena_size = 0;
	memory.scene_stack_size = 0;
	if (wl_mem_init(&memory) != WL_OK)
		wl_platform_stop_screen("WiiLibs' heaps could not be made (see the log).");
	/* what MEM1's malloc cannot hold comes from the game's heap in MEM2
	(the link wraps malloc and the rest: tools/wii_build.py) */
	if (wl_heap_overflow_enable(WII_MEMORY_TAG_GAME) != WL_OK)
		wl_platform_stop_screen("The game's heap in MEM2 could not be made (see the log).");
#ifdef WII_HEAP_SITES
	{
		void *storage = WL_MEM_ALLOC(WII_MEMORY_TAG_GAME, WII_HEAP_SITES_STORAGE);

		if (!storage || wl_heap_overflow_track_sites(storage, WII_HEAP_SITES_STORAGE, 256u, 1024u) != WL_OK)
			WL_LOG_W("wii", "no accounting of the heaps by call site");
	}
#endif

	/* USB 2.0 needs IOS 58; then the SD card and a USB drive, either of
	which may hold the game (platform_data_root) */
	wl_platform_fs_ios_init(&ios);
	wl_platform_fs_mount(WL_DEVICE_SD);
	wl_platform_fs_mount(WL_DEVICE_USB);
	/* the network, without waiting for it (src/posix_net_wii.c) */
	wii_network_start();
	/* the sampling profiler, only if profile.txt is in the game's folder (src/wii_profile.c) */
	wii_profile_arm();
	wl_platform_console_report("wii", "IOS %u; SD card %s, USB drive %s\n", (unsigned)ios,
		wl_platform_fs_mounted(WL_DEVICE_SD) ? "mounted" : "absent",
		wl_platform_fs_mounted(WL_DEVICE_USB) ? "mounted" : "absent");
	{
		void *result = NULL;

		if (wl_platform_run_on_stack(game_main, NULL, game_stack, GAME_STACK_SIZE, &result) != WL_OK)
			wl_platform_stop_screen("The game's main thread could not be made (see the log).");
		return (int)(intptr_t)result;
	}
}
