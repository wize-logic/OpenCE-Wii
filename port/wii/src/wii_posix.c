/*
WII_POSIX.C

The POSIX functions the shared platform layer uses that devkitPPC's newlib
lacks: pread and pwrite (port/linux/src/xbox_files.c). They seek, read or
write and seek back under one lock, as threads (the game's and the
asynchronous reads') may share a descriptor. And where the game's folder is,
which the settings file is read from before any setting can be. Built as the
posix_*.c units are, against newlib alone.
*/

#include <errno.h>
#include <pthread.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static pthread_mutex_t positioned_lock = PTHREAD_MUTEX_INITIALIZER;

static ssize_t positioned(int descriptor, void *buffer, size_t count, off_t offset, int writing)
{
	off_t position;
	ssize_t result;
	int error;

	pthread_mutex_lock(&positioned_lock);
	position = lseek(descriptor, 0, SEEK_CUR);
	if (position == (off_t)-1 || lseek(descriptor, offset, SEEK_SET) == (off_t)-1)
	{
		pthread_mutex_unlock(&positioned_lock);
		return -1;
	}
	result = writing ? write(descriptor, buffer, count) : read(descriptor, buffer, count);
	error = errno;
	lseek(descriptor, position, SEEK_SET);
	pthread_mutex_unlock(&positioned_lock);
	errno = error;
	return result;
}

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	return positioned(descriptor, buffer, count, offset, 0);
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	return positioned(descriptor, (void *)buffer, count, offset, 1);
}

/* the Homebrew Channel's folder for the game: sd:/apps/halo, or
usb:/apps/halo when only the USB drive has one with maps in it */
const char *wii_game_folder(void)
{
	struct stat information;

	if (stat("sd:/apps/halo/maps", &information) != 0 && stat("usb:/apps/halo/maps", &information) == 0)
		return "usb:/apps/halo";
	return "sd:/apps/halo";
}

/* waits for a signal, which the Wii never sends (port/linux/src/xbox_kernel.c,
a thread that sleeps for ever) */
int pause(void)
{
	for (;;)
		sleep(3600);
	return -1;
}

/* the memory the game is told the machine has (GlobalMemoryStatus): the
Wii's 88 MB, and what newlib's heap has free */
long sysconf(int name)
{
	switch (name)
	{
	case _SC_PAGESIZE:
		return 4096;
	case _SC_PHYS_PAGES:
		return (88L << 20) / 4096;
	case _SC_AVPHYS_PAGES:
		return (long)(mallinfo().fordblks / 4096);
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
	struct mallinfo heap = mallinfo();

	*used = (unsigned long)heap.uordblks;
	*free_bytes = (unsigned long)heap.fordblks;
}

/* ---------- thread priorities

devkitPPC's pthread_create (libsysbase) starts every thread at libogc's
lowest priority (127) and takes none from its attributes; libogc's
scheduler is strict, so such a thread never runs while the game's main
thread (63) is ready, and the game's waits that yield (SwitchToThread) for
its worker threads never end. A thread the kernel layer starts
(port/linux/src/xbox_kernel.c) is given a Win32 priority here: normal is the
main thread's, each step above or below one less or more (a lower number
runs first). A pthread_t is libogc's thread (the block pthread_create
allocates begins with it), which the priority check below confirms. */

#include <tuxedo/thread.h>

/* devkitPPC's sched_yield is a stub (ENOSYS): SwitchToThread, which the
game's waits on its worker threads spin on (the texture cache's for a read,
xbox_texture_cache.c), gave those threads nothing, and a wait at the menu
took most of a minute. It is libogc's yield (to the threads of the same
priority: the game's run at the main thread's), through the link's wrap
(tools/wii_build.py). */
int __wrap_sched_yield(void)
{
	KThreadYield();
	return 0;
}

int wii_thread_priority(pthread_t thread, int win32_priority)
{
	KThread *kernel_thread = (KThread *)thread;
	int priority = KTHR_MAIN_PRIO - (win32_priority > 15 ? 15 : win32_priority < -15 ? -15 : win32_priority);

	if (!kernel_thread || kernel_thread->prio > KTHR_MIN_PRIO)
		return 0;
	KThreadSetPrio(kernel_thread, (u16)priority);
	return 1;
}
