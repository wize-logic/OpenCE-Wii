/*
WII_POSIX.C

The Wii's answers to the questions the shared platform layer asks the C
library, which WiiLibs (wl/platform) works out: where the game's folder is,
which the settings file is read from before any setting can be; random
bytes; the game's thread priorities; and the heap report. Thin names over
WiiLibs' functions, kept for the shared files that call them. (pread, pwrite,
pause and sysconf, which newlib declares and does not define, are WiiLibs'
too: wl/platform/wii/posix_wii.c, weak definitions over wl_posix_pread and the
rest.) Built as the posix_*.c units are, against newlib and WiiLibs' headers.
*/

#include <pthread.h>

#include "wl/platform/heap_overflow.h"
#include "wl/platform/platform.h"
#include "wl/platform/posix.h"

/* the Homebrew Channel's folder for the game: sd:/apps/halo, or
usb:/apps/halo when only the USB drive has one with maps in it
(wl_platform_fs_find_app_dir; the SD folder when neither does) */
const char *wii_game_folder(void)
{
	static char folder[WL_FS_PATH_MAX];

	wl_platform_fs_find_app_dir("halo", "maps", folder, sizeof(folder));
	return folder;
}

/* random bytes (posix.h): WiiLibs' entropy, the time base's bits mixed;
not cryptographic (internet play's keys would want better, which the Wii
does not play) */
void posix_random_bytes(void *buffer, unsigned long size)
{
	wl_platform_random_bytes(buffer, size);
}

/* newlib's heap in MEM1: bytes in use and free (the Direct3D device's
memory report, port/wii/src/d3d8_gx.c) */
void wii_heap_usage(unsigned long *used, unsigned long *free_bytes)
{
	WlOverflowUsage usage;

	wl_heap_overflow_usage(&usage);
	*used = (unsigned long)usage.primary_used;
	*free_bytes = (unsigned long)usage.primary_free;
}

/* The game's thread priorities. A thread the kernel layer starts
(port/linux/src/xbox_kernel.c) is given a Win32 priority here: normal is the
main thread's, each step above or below one less or more (a lower number
runs first), by WiiLibs' wl_posix_thread_set_relative_priority (devkitPPC's
pthread_create starts every thread at libogc's lowest priority, which would
never run while the game's main thread, WiiLibs' WL_POSIX_PRIO_MAIN, 64, is
ready). Returns 0 when the thread is not a live libogc thread. */
int wii_thread_priority(pthread_t thread, int win32_priority)
{
	return wl_posix_thread_set_relative_priority(thread, win32_priority) == WL_OK;
}
