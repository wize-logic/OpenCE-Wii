/*
WII_BOOT.C

What runs before the game's main() (source/shell/shell_xbox.c) on the Wii.
The link wraps main (tools/wii_build.py), so the start-up code calls
__wrap_main here, which then calls the game's:

- the fixed memory the game is linked to, claimed before anything can
  allocate there (port/wii/src/wii_memory.c);
- WiiLibs: the reset and power buttons, the log, its heaps (sized for what
  the port uses it for), IOS 58 for USB 2.0, the SD card and a USB drive;
- for the bring-up, a text console on the screen.

Also the trap the bring-up stubs call (tools/wii_link.py): a platform
function the port does not provide yet stops the game, naming it.
*/

#include <gccore.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "wl/platform/mem.h"
#include "wl/platform/platform.h"

#include "wii_memory.h"

void wii_missing_symbol(const char *name) __attribute__((noreturn));
int wii_memory_claim(void);
void wii_network_start(void);
int __real_main(void);

/* WiiLibs' heaps, all in MEM2: thread stacks, the audio output's buffers, the
GX command FIFO, the asynchronous reads' buffers (which the port does not
use), and the game's malloc once MEM1's is full (src/wii_malloc.c). MEM1
holds the game, its game state, and libogc's malloc heap below them. Its
own scene graph, network, asset and sample bank heaps the port has no use
for. */
static const WlMemTagDef wii_memory_tags[WII_MEMORY_TAG_COUNT] =
{
	[WL_TAG_STACK] = { "stack", WL_MEM2, 256u * 1024u },
	[WL_TAG_GXFIFO] = { "gxfifo", WL_MEM2, 1024u * 1024u },
	[WL_TAG_HOT] = { "hot", WL_MEM1, 0 },
	[WL_TAG_AUDIO] = { "audio", WL_MEM2, 256u * 1024u },
	[WL_TAG_NET] = { "net", WL_MEM1, 0 },
	[WL_TAG_ASSET] = { "asset", WL_MEM2, 0 },
	[WL_TAG_AUDIO_BANK] = { "audio_bank", WL_MEM2, 0 },
	[WL_TAG_IO] = { "io", WL_MEM2, 256u * 1024u },
	[WII_MEMORY_TAG_GAME] = { "game", WL_MEM2, 5120u * 1024u },
};

static void wii_report(const char *format, ...)
{
	char line[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	/* the console, and the log */
	fputs(line, stdout);
	WL_LOG_I("wii", "%s", line);
}

static void wii_console_start(void)
{
	GXRModeObj *mode;
	void *framebuffer;

	VIDEO_Init();
	mode = VIDEO_GetPreferredMode(NULL);
	framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
	console_init(framebuffer, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(mode);
	VIDEO_SetNextFramebuffer(framebuffer);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (mode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

int __wrap_main(void)
{
	WlMemConfig memory;
	uint32_t ios = 0;
	/* first: nothing may allocate in the fixed windows */
	int claimed = wii_memory_claim();

	wl_platform_system_init();
	wl_platform_log_init(NULL);
	wii_console_start();
	wii_report("Halo: Combat Evolved for the Wii (bring-up build)\n");
	if (!claimed)
		wii_missing_symbol("the fixed memory windows (see the log)");

	wl_mem_config_default(&memory);
	memory.tags = wii_memory_tags;
	memory.tag_count = WII_MEMORY_TAG_COUNT;
	memory.frame_arena_size = 0;
	memory.scene_stack_size = 0;
	if (wl_mem_init(&memory) != WL_OK)
		wii_missing_symbol("memory for WiiLibs' heaps (see the log)");

	/* USB 2.0 needs IOS 58; then the SD card and a USB drive, either of
	which may hold the game (platform_data_root) */
	wl_platform_fs_ios_init(&ios);
	wl_platform_fs_mount(WL_DEVICE_SD);
	wl_platform_fs_mount(WL_DEVICE_USB);
	/* the network, without waiting for it (src/posix_net_wii.c) */
	wii_network_start();
	wii_report("IOS %u; SD card %s, USB drive %s\n", (unsigned)ios,
		wl_platform_fs_mounted(WL_DEVICE_SD) ? "mounted" : "absent",
		wl_platform_fs_mounted(WL_DEVICE_USB) ? "mounted" : "absent");
	return __real_main();
}

/* The way out, logged: exit and abort are wrapped (tools/wii_build.py), so a
game that ends says how, and from where (the return addresses up its frame
chain, for addr2line on build/wii/halo.elf). */
void __real_exit(int code) __attribute__((noreturn));
void __real_abort(void) __attribute__((noreturn));

static void wii_log_callers(const char *what, int code)
{
	unsigned long *frame = __builtin_frame_address(0);
	char line[160];
	int length = snprintf(line, sizeof(line), "%s(%d) from", what, code);
	int depth;

	for (depth = 0; depth < 10 && frame && (unsigned long)frame >= 0x80000000UL &&
		(unsigned long)frame < 0x94000000UL; depth++)
	{
		unsigned long *caller = (unsigned long *)frame[0];

		if (!caller || (unsigned long)caller < 0x80000000UL || (unsigned long)caller >= 0x94000000UL)
			break;
		/* (a PowerPC frame's caller keeps its return address one word into its own frame) */
		if (length < (int)sizeof(line) - 12)
			length += snprintf(line + length, sizeof(line) - (size_t)length, " %08lx", caller[1]);
		frame = caller;
	}
	WL_LOG_E("wii", "%s", line);
	wl_platform_log_flush();
}

void __wrap_exit(int code)
{
	wii_log_callers("exit", code);
	__real_exit(code);
}

void __wrap_abort(void)
{
	wii_log_callers("abort", 0);
	__real_abort();
}

void wii_missing_symbol(const char *name)
{
	static int stopped;

	/* (once: a stub reached while stopping only waits) */
	if (stopped++)
	{
		for (;;)
			VIDEO_WaitVSync();
	}
	wii_report("\nThe Wii port does not provide %s yet.\nPress HOME (or START) to return to the loader.\n", name);
	wl_platform_log_flush();
	for (;;)
	{
		WlRawInput input;
		uint32_t index;
		int home = 0;

		wl_platform_input_poll(&input);
		for (index = 0; index < WL_INPUT_MAX_REMOTES; index++)
			home |= input.remotes[index].connected && (input.remotes[index].buttons & WL_WM_HOME);
		for (index = 0; index < WL_INPUT_MAX_PADS; index++)
			home |= input.pads[index].connected && (input.pads[index].buttons & WL_PAD_START);
		if (home || wl_platform_system_events())
			wl_platform_exit(WL_EXIT_LOADER);
		VIDEO_WaitVSync();
	}
}
