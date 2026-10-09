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
