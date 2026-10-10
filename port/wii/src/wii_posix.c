/*
WII_POSIX.C

The POSIX functions the shared platform layer uses that devkitPPC's newlib
lacks, and the Wii's own answers to the C library's questions: where the
game's folder is, which the settings file is read from before any setting can
be; sysconf's memory; random bytes; and the game's thread priorities. (pread,
pwrite and pause, which newlib declares and does not define, are WiiLibs':
wl/platform/wii/posix_wii.c, weak definitions over wl_posix_pread and the
rest.) Built as the posix_*.c units are, against newlib and WiiLibs' headers.
*/

#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "wl/platform/heap_overflow.h"

/* the Homebrew Channel's folder for the game: sd:/apps/halo, or
usb:/apps/halo when only the USB drive has one with maps in it */
const char *wii_game_folder(void)
{
	struct stat information;

	if (stat("sd:/apps/halo/maps", &information) != 0 && stat("usb:/apps/halo/maps", &information) == 0)
		return "usb:/apps/halo";
	return "sd:/apps/halo";
}

/* the memory the game is told the machine has (GlobalMemoryStatus): the
Wii's 88 MB, and what its malloc can still give: newlib's heap in MEM1 and
what is left of the game's heap in MEM2 (WiiLibs' heap overflow) */
long sysconf(int name)
{
	switch (name)
	{
	case _SC_PAGESIZE:
		return 4096;
	case _SC_PHYS_PAGES:
		return (88L << 20) / 4096;
	case _SC_AVPHYS_PAGES:
	{
		WlOverflowUsage usage;

		wl_heap_overflow_usage(&usage);
		return (long)(usage.free_total / 4096);
	}
	case _SC_NPROCESSORS_CONF:
	case _SC_NPROCESSORS_ONLN:
		return 1;
	default:
		errno = EINVAL;
		return -1;
	}
}

/* random bytes (posix.h): the Wii has no entropy device; the time base's low
bits mixed (internet play's keys would want better, which the Wii does not
play) */
void posix_random_bytes(void *buffer, unsigned long size)
{
	static unsigned long long state;
	unsigned char *out = buffer;
	unsigned long index;

	for (index = 0; index < size; index++)
	{
		unsigned long upper, lower;

		__asm__ volatile ("mftbu %0; mftb %1" : "=r"(upper), "=r"(lower));
		state ^= ((unsigned long long)upper << 32 | lower) + 0x9E3779B97F4A7C15ULL;
		state ^= state >> 33;
		state *= 0xFF51AFD7ED558CCDULL;
		state ^= state >> 33;
		out[index] = (unsigned char)state;
	}
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

/* ---------- thread priorities

devkitPPC's pthread_create (libsysbase) starts every thread at libogc's
lowest priority (127) and takes none from its attributes; libogc's
scheduler is strict, so such a thread never runs while the game's main
thread (64, below) is ready, and the game's waits that yield (SwitchToThread) for
its worker threads never end. A thread the kernel layer starts
(port/linux/src/xbox_kernel.c) is given a Win32 priority here: normal is the
main thread's, each step above or below one less or more (a lower number
runs first). A pthread_t is libogc's thread (the block pthread_create
allocates begins with it), which the priority check below confirms. */

#include <tuxedo/thread.h>

/* (devkitPPC's sched_yield is a stub: SwitchToThread, which the game's waits
on its worker threads spin on, needs libogc's yield. WiiLibs provides
__wrap_sched_yield, which the link's --wrap=sched_yield (tools/wii_build.py)
sends the callers to.)

The game's main thread runs at WiiLibs' WL_THREAD_PRIO_MAIN, which is LWP
priority 64 (wl/platform/wii/thread_wii.c), one above libogc's own main
thread (KTHR_MAIN_PRIO, 63): the game's threads are placed around that. */
#define WII_GAME_MAIN_PRIO 64

int wii_thread_priority(pthread_t thread, int win32_priority)
{
	KThread *kernel_thread = (KThread *)thread;
	int priority = WII_GAME_MAIN_PRIO - (win32_priority > 15 ? 15 : win32_priority < -15 ? -15 : win32_priority);

	if (!kernel_thread || kernel_thread->prio > KTHR_MIN_PRIO)
		return 0;
	KThreadSetPrio(kernel_thread, (u16)priority);
	return 1;
}
