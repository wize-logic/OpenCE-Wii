/*
XBOX_FILES.C

Win32 file API for the Linux build, over POSIX descriptors.

Xbox paths are translated below two roots. d:\ is the data root, the
directory holding the game's maps/ folder: paths.data (port_config.c), else the current
directory when it has maps/, else assets/ in the current directory or two
levels above the executable (the repository root for build/linux/halo).
h:\ is the Halo Custom Edition install paths.custom_edition names, if any
(its maps folder holds the resource maps Custom Edition maps need, and more
maps).
Every other drive letter X:\ is the subdirectory X/ of the save root (z:\ holds the persistent cache and saves,
u:\ user data, t:\ title data): paths.saves, else
$XDG_DATA_HOME/halo-linux or ~/.local/share/halo-linux. Path components are
matched case-insensitively, like the Xbox's FATX volumes.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HALO_WII
#include <pthread.h>
#endif

#ifdef HALO_WII
/* port: the Wii's (port/wii/src/wii_system.c): the message that stops the game
when it has no maps folder, and the files' part of leaving it */
void wii_stop_no_maps(void);
static void shadow_sweep(const char *root);
#endif

#ifdef HALO_WII
/* ---------- where the card's time goes

port: the calls that reach the SD card or the USB drive are timed, by kind,
and the totals are logged (about every two seconds while there are any), so
that a slow start or a slow load can be read from the log: "files: 3210 ms in
167 calls: ..." names the kinds that took most. One call of 25 ms or more is
logged by itself (the first 64 of them). */
enum
{
	_card_translate,
	_card_open, /* CreateFileA, with the copy's making */
	_card_directory, /* mkdir, rmdir, a listing's opening */
	_card_attributes, /* stat, chmod */
	_card_write,
	_card_sync, /* fsync */
	_card_close, /* close of a descriptor */
	_card_replace, /* the copy's rename over its file, and the unlink of the old */
	_card_unlink,
	NUMBER_OF_CARD_KINDS
};

static const char *const card_kind_names[NUMBER_OF_CARD_KINDS] =
{
	"paths", "open", "directories", "attributes", "writes", "syncs", "closes", "replacing", "deleting"
};

static unsigned long card_microseconds[NUMBER_OF_CARD_KINDS];
static unsigned long card_calls[NUMBER_OF_CARD_KINDS];
static unsigned long card_reported_microseconds;
static unsigned long card_slow_logged;
static unsigned long card_last_report_ms;

static unsigned long long card_now(void)
{
	LARGE_INTEGER counter, frequency;

	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return (unsigned long long)counter.QuadPart * 1000000ULL / (unsigned long long)frequency.QuadPart;
}

static void card_report(unsigned long long now)
{
	unsigned long total = 0, calls = 0;
	char line[320];
	int used = 0;
	int kind;

	for (kind = 0; kind < NUMBER_OF_CARD_KINDS; kind++)
	{
		total += card_microseconds[kind];
		calls += card_calls[kind];
	}
	if (total == card_reported_microseconds)
		return;
	card_reported_microseconds = total;
	card_last_report_ms = (unsigned long)(now / 1000);
	used = snprintf(line, sizeof(line), "files: %lu ms in %lu calls:", total / 1000, calls);
	for (kind = 0; kind < NUMBER_OF_CARD_KINDS && used < (int)sizeof(line) - 40; kind++)
	{
		if (card_calls[kind])
			used += snprintf(line + used, sizeof(line) - (size_t)used, " %s %lu ms (%lu)", card_kind_names[kind],
				card_microseconds[kind] / 1000, card_calls[kind]);
	}
	platform_log("%s", line);
}

/* what a call of this kind took, from card_now() before it */
static void card_end(int kind, unsigned long long started, const char *path)
{
	unsigned long long now = card_now();
	unsigned long microseconds = (unsigned long)(now - started);

	__sync_fetch_and_add(&card_microseconds[kind], microseconds);
	__sync_fetch_and_add(&card_calls[kind], 1);
	if (microseconds >= 25000 && card_slow_logged < 64)
	{
		card_slow_logged++;
		platform_log("files: %s took %lu ms (%s)", card_kind_names[kind], microseconds / 1000, path ? path : "");
	}
	if ((unsigned long)(now / 1000) - card_last_report_ms >= 2000)
		card_report(now);
}
#endif

/* ---------- paths */

static BOOL directory_exists(const char *path)
{
	struct posix_file_information information;

	return posix_stat(path, &information) == 0;
}

/* whether directory has a maps folder, in any case */
static BOOL has_maps(const char *directory)
{
	char on_disk[256];

	return directory_exists(directory) &&
		posix_find_entry_case_insensitive(directory, "maps", on_disk, sizeof(on_disk));
}

static void trim_separators(char *path)
{
	while (strlen(path) > 1 && path[strlen(path) - 1] == '/')
		path[strlen(path) - 1] = '\0';
}

const char *platform_data_root(void)
{
	static char root[MAX_PATH];

	if (!root[0])
	{
		const char *environment = config_string("paths.data");

		if (*environment)
		{
			snprintf(root, sizeof(root), "%s", environment);
		}
#ifdef HALO_WII
		/* the Homebrew Channel's folder for the game, on the SD card or a USB
		drive (port/wii/README.md) */
		else
		{
			snprintf(root, sizeof(root), "%s", wii_game_folder());
			if (!has_maps(root))
			{
				platform_log("no maps/ folder in sd:/apps/halo or usb:/apps/halo");
				/* port: the screen says so, and waits for HOME (it does not
				come back) */
				wii_stop_no_maps();
			}
		}
#else
		else if (has_maps("."))
		{
			snprintf(root, sizeof(root), ".");
		}
		else
		{
			char executable[MAX_PATH];
			char executable_directory[MAX_PATH] = "";
			ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);

			if (length > 0)
			{
				char *slash;

				executable[length] = '\0';
				snprintf(executable_directory, sizeof(executable_directory), "%s", executable);
				slash = strrchr(executable_directory, '/');
				if (slash)
					*slash = '\0';
			}
			snprintf(root, sizeof(root), ".");
			if (executable_directory[0] && has_maps(executable_directory))
			{
				/* next to the executable, where the desktop ports' first start
				extracts it (platform_offer_game_data) */
				snprintf(root, sizeof(root), "%s", executable_directory);
			}
			else if (has_maps("assets"))
			{
				snprintf(root, sizeof(root), "assets");
			}
			else if (length > 0)
			{
				/* <repository>/build/linux/halo -> <repository>/assets */
				char *slash;
				int level;

				executable[length] = '\0';
				for (level = 0; level < 3 && (slash = strrchr(executable, '/')); level++)
					*slash = '\0';
				if (level == 3 && strlen(executable) + sizeof("/assets") <= sizeof(executable))
				{
					strcat(executable, "/assets");
					if (has_maps(executable))
						snprintf(root, sizeof(root), "%s", executable);
				}
			}
#if !defined(HALO_ANDROID) && !defined(HALO_WEB)
			if (!has_maps(root) && executable_directory[0] && platform_offer_game_data(executable_directory) &&
				has_maps(executable_directory))
			{
				snprintf(root, sizeof(root), "%s", executable_directory);
			}
#endif
			if (!has_maps(root))
				platform_log("no maps/ folder found; set paths.data in config.toml to the folder that holds maps/");
		}
#endif
		trim_separators(root);
		platform_log("data root: %s (the game's log: debug.txt there)", root);
	}
	return root;
}

/* creates every missing directory along path */
static void make_directories(const char *path)
{
	char partial[MAX_PATH];
	unsigned long index;

	snprintf(partial, sizeof(partial), "%s", path);
	for (index = 1; partial[index]; index++)
	{
		if (partial[index] == '/')
		{
			partial[index] = '\0';
			if (!directory_exists(partial))
				posix_make_directory(partial);
			partial[index] = '/';
		}
	}
	if (!directory_exists(partial))
		posix_make_directory(partial);
}

const char *platform_save_root(void)
{
	static char root[MAX_PATH];

	if (!root[0])
	{
		const char *environment = config_string("paths.saves");
		const char *data_home = getenv("XDG_DATA_HOME");
		const char *home = getenv("HOME");

		if (*environment)
			snprintf(root, sizeof(root), "%s", environment);
#ifdef HALO_WII
		/* beside the maps, on the same card or drive */
		else
			snprintf(root, sizeof(root), "%s/saves", platform_data_root());
		(void)data_home;
		(void)home;
#else
#ifdef _WIN32
		/* the Windows build (port/windows) keeps saves in the roaming
		application data folder */
		else if (getenv("APPDATA") && *getenv("APPDATA"))
			snprintf(root, sizeof(root), "%s/halo", getenv("APPDATA"));
#endif
		else if (data_home && *data_home)
			snprintf(root, sizeof(root), "%s/halo-linux", data_home);
		else if (home && *home)
			snprintf(root, sizeof(root), "%s/.local/share/halo-linux", home);
		else
			snprintf(root, sizeof(root), "%s", platform_data_root());
#endif
		trim_separators(root);
		make_directories(root);
		platform_log("save root: %s", root);
#ifdef HALO_WII
		/* port: put back what a cut left of the last run's saves */
		shadow_sweep(root);
#endif
	}
	return root;
}

const char *platform_custom_edition_root(void)
{
	static char root[MAX_PATH];
	static int found;

	if (!found)
	{
		char maps[MAX_PATH + 8];
		struct posix_file_information information;

		found = 1;
		snprintf(root, sizeof(root), "%s", config_string("paths.custom_edition"));
		trim_separators(root);
		snprintf(maps, sizeof(maps), "%s/maps", root);
		if (root[0] && posix_stat(maps, &information) != 0)
		{
			platform_log("Halo Custom Edition: %s has no maps folder; not using it", root);
			root[0] = 0;
		}
		if (root[0])
			platform_log("Halo Custom Edition: using the install at %s", root);
	}
	return root;
}

/* the next component of an Xbox path, with its separators skipped: FALSE at
the end. . and .. (and Windows' spellings of them, with trailing dots and
spaces) are skipped: FATX names are never only dots, and .. would leave the
root */
static BOOL next_path_component(const char **cursor, char *component, unsigned long size)
{
	for (;;)
	{
		unsigned long length = 0;

		while (**cursor == '\\' || **cursor == '/')
			(*cursor)++;
		if (!**cursor)
			return FALSE;
		while (**cursor && **cursor != '\\' && **cursor != '/' && length + 1 < size)
			component[length++] = *(*cursor)++;
		component[length] = '\0';
		if (length && strspn(component, ". ") != length)
			return TRUE;
	}
}

/* adds a component to a host path, in the spelling the directory has it (the
Xbox's volumes did not tell the cases apart) */
static void append_path_component(char *resolved, unsigned long size, const char *component)
{
	char on_disk[256];
	unsigned long length;

	if (posix_find_entry_case_insensitive(resolved, component, on_disk, sizeof(on_disk)))
	{
		/* prefer an exact match when several spellings exist */
		char exact[1100];
		struct posix_file_information information;

		snprintf(exact, sizeof(exact), "%s/%s", resolved, component);
		if (posix_stat(exact, &information) == 0)
			snprintf(on_disk, sizeof(on_disk), "%s", component);
	}
	else
	{
		snprintf(on_disk, sizeof(on_disk), "%s", component);
	}
	length = strlen(resolved);
	snprintf(resolved + length, size - length, "/%s", on_disk);
}

#ifdef HALO_WII
/* port: a path that is there as it is written needs no look into the
directories (each of those opens a directory of the card and reads all of its
entries, once for every part of the path, at every open): one stat of the
whole path, or when the file is not there yet (it is to be made) one of its
folder and one look into that folder for the file's own spelling. The result
is the same as the look at every part gives, which prefers the spelling as
written where it exists. Returns whether the host path is made; resolved is
spoilt when it is not. */
static BOOL translate_as_written(char *resolved, unsigned long size, const char *cursor, char *host_path,
	unsigned long host_path_size)
{
	char component[256];
	char last[256];
	unsigned long parent_length = 0;
	unsigned long length;
	struct posix_file_information information;
	BOOL any = FALSE;

	last[0] = '\0';
	while (next_path_component(&cursor, component, sizeof(component)))
	{
		length = strlen(resolved);
		if (length + 1 + strlen(component) + 1 > size)
			return FALSE;
		parent_length = length;
		snprintf(resolved + length, size - length, "/%s", component);
		snprintf(last, sizeof(last), "%s", component);
		any = TRUE;
	}
	if (!any)
		return FALSE;
	if (posix_stat(resolved, &information) == 0)
	{
		snprintf(host_path, host_path_size, "%s", resolved);
		return TRUE;
	}
	resolved[parent_length] = '\0';
	if (parent_length && posix_stat(resolved, &information) == 0 && (information.flags & _posix_file_is_directory))
	{
		append_path_component(resolved, size, last);
		snprintf(host_path, host_path_size, "%s", resolved);
		return TRUE;
	}
	return FALSE;
}
#endif

void platform_translate_path(const char *xbox_path, char *host_path, unsigned long host_path_size)
{
	char resolved[1024];
	char component[256];
	const char *cursor = xbox_path;
#ifdef HALO_WII
	char root[1024];
	unsigned long long started = card_now();
#endif

	snprintf(resolved, sizeof(resolved), "%s", platform_data_root());
	if (((cursor[0] >= 'a' && cursor[0] <= 'z') || (cursor[0] >= 'A' && cursor[0] <= 'Z')) && cursor[1] == ':')
	{
		char drive = (char)(cursor[0] | 0x20);

		if (drive == 'h' && platform_custom_edition_root()[0])
		{
			snprintf(resolved, sizeof(resolved), "%s", platform_custom_edition_root());
		}
		else if (drive != 'd')
		{
			struct posix_file_information information;

			snprintf(resolved, sizeof(resolved), "%s/%c", platform_save_root(), drive);
			/* every Xbox drive always exists; create its directory on first use */
			if (posix_stat(resolved, &information) != 0)
				posix_make_directory(resolved);
		}
		cursor += 2;
	}
#ifdef HALO_WII
	snprintf(root, sizeof(root), "%s", resolved);
	if (translate_as_written(resolved, sizeof(resolved), cursor, host_path, host_path_size))
	{
		card_end(_card_translate, started, NULL);
		return;
	}
	snprintf(resolved, sizeof(resolved), "%s", root);
#endif
	while (next_path_component(&cursor, component, sizeof(component)))
		append_path_component(resolved, sizeof(resolved), component);
	snprintf(host_path, host_path_size, "%s", resolved);
#ifdef HALO_WII
	card_end(_card_translate, started, NULL);
#endif
}

/* ---------- file handles */

struct platform_file
{
	int descriptor;
	char path[1024];
#ifdef HALO_WII
	int shadow; /* _shadow_* below */
	BOOL damaged; /* a write or a resize failed: the old file is kept */
	BOOL modified; /* a write or a resize was made (to the copy, when there is one) */
	unsigned long long original_size;
	char temporary_path[1032];
	/* a file the game makes again at every start ("files made again", below) */
	BOOL regenerated;
	BOOL regenerated_for_write; /* the descriptor can write (the file is opened for reading until it differs) */
	BOOL truncate_pending; /* it was to be emptied (CREATE_ALWAYS): cut to what is written at the close */
	unsigned long long written_end;
	/* a handle only read at offsets (a map's) keeps its place, so that the next read in line needs no seek */
	BOOL positioned_only;
	BOOL place_lock_ready;
	long long place; /* where the descriptor stands, -1: not known */
	pthread_mutex_t place_lock;
	/* what it has been read for, logged when it closes */
	unsigned long reads;
	unsigned long long read_bytes;
	unsigned long long read_microseconds;
#endif
};

#ifdef HALO_WII
/* port: what the Wii's leaving (a reset or power button, a quit from the menu:
wii_files_quiesce, called from the shutdown hook in port/wii/src/wii_system.c)
needs of the files: no write cut in the middle, and the data written to the
files that stay open (the saved game's) on the card. Writes in flight are
counted, a write that starts after the gate closes waits for the end, and the
files open for writing are synced. */
#define WRITABLE_FILE_SLOTS 32

static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
static int writes_in_flight;
static BOOL writes_closed;
static struct platform_file *writable_files[WRITABLE_FILE_SLOTS];

static void write_begin(void)
{
	pthread_mutex_lock(&write_lock);
	while (writes_closed)
	{
		/* (the game is leaving: this write is not made) */
		pthread_mutex_unlock(&write_lock);
		usleep(10000);
		pthread_mutex_lock(&write_lock);
	}
	writes_in_flight++;
	pthread_mutex_unlock(&write_lock);
}

static void write_end(void)
{
	pthread_mutex_lock(&write_lock);
	writes_in_flight--;
	pthread_mutex_unlock(&write_lock);
}

static void writable_register(struct platform_file *file)
{
	int slot;

	pthread_mutex_lock(&write_lock);
	for (slot = 0; slot < WRITABLE_FILE_SLOTS; slot++)
	{
		if (!writable_files[slot])
		{
			writable_files[slot] = file;
			break;
		}
	}
	pthread_mutex_unlock(&write_lock);
}

static void writable_unregister(struct platform_file *file)
{
	int slot;

	pthread_mutex_lock(&write_lock);
	for (slot = 0; slot < WRITABLE_FILE_SLOTS; slot++)
	{
		if (writable_files[slot] == file)
			writable_files[slot] = NULL;
	}
	pthread_mutex_unlock(&write_lock);
}

/* no more writes start; those under way finish (for at most timeout_ms); the
files open for writing are synced. Returns how many writes were still under
way at the end of the wait. */
int wii_files_quiesce(unsigned timeout_ms)
{
	unsigned waited_ms = 0;
	int unfinished;
	int slot;

	pthread_mutex_lock(&write_lock);
	writes_closed = TRUE;
	while (writes_in_flight > 0 && waited_ms < timeout_ms)
	{
		pthread_mutex_unlock(&write_lock);
		usleep(5000);
		waited_ms += 5;
		pthread_mutex_lock(&write_lock);
	}
	unfinished = writes_in_flight;
	for (slot = 0; slot < WRITABLE_FILE_SLOTS; slot++)
	{
		if (writable_files[slot])
			fsync(writable_files[slot]->descriptor);
	}
	pthread_mutex_unlock(&write_lock);
	return unfinished;
}

/* ---------- saves that survive a cut

port: a file opened for writing under the save root is written beside itself,
as <file>.wtmp, and takes the file's place when the game closes it, so that a
reset, a power-off or a pulled card leaves the old file or the new one and
never a torn one (a save is several writes, the header last). The data is
synced before the replacing. FAT cannot rename over a file, so then the old
one goes aside for a moment (<file>.wold; the sweep at the next start puts it
back if the cut came between the two renames). A file written whole from its
start (a profile, the saved game) is not copied into its copy first; one that
is not, is. A write that fails (the card is full, read-only or gone) leaves the
old file as it was, and the game gets the error its handlers know. The revert
buffer (z:\savegame.bin, opened unbuffered and kept open) is not shadowed:
nothing reads it after the game ends. */
#define SHADOW_TEMPORARY_SUFFIX ".wtmp"
#define SHADOW_OLD_SUFFIX ".wold"
#define SHADOW_COPY_SIZE 32768
#define SHADOW_SWEEP_DEPTH 8

enum
{
	_shadow_none,
	_shadow_deferred, /* the file is open as it was; the copy is made at the first write */
	_shadow_active /* every read and write goes to the copy */
};

static void shadow_name(char *name, unsigned long size, const char *path, const char *suffix)
{
	snprintf(name, size, "%s%s", path, suffix);
}

static BOOL shadow_name_has_suffix(const char *name, const char *suffix)
{
	size_t length = strlen(name);
	size_t suffix_length = strlen(suffix);

	return length > suffix_length && !strcmp(name + length - suffix_length, suffix);
}

/* whether a host path is under the save root (the Wii's saves folder) */
static BOOL shadow_wanted(const char *path)
{
	const char *root = platform_save_root();
	size_t length = strlen(root);

	return length && !strncmp(path, root, length) && path[length] == '/';
}

/* puts the written copy in place of the file: 0, or the errno that stopped it */
static int shadow_replace(const char *temporary_path, const char *path)
{
	char old_path[1032];
	struct posix_file_information information;
	int error;

	if (rename(temporary_path, path) == 0)
		return 0;
	if (posix_stat(path, &information) != 0 || (information.flags & _posix_file_is_directory))
		return errno ? errno : EIO;
	/* FAT: the old file goes aside, the copy takes its name, the old file goes */
	shadow_name(old_path, sizeof(old_path), path, SHADOW_OLD_SUFFIX);
	unlink(old_path);
	if (rename(path, old_path) != 0)
		return errno ? errno : EIO;
	if (rename(temporary_path, path) != 0)
	{
		error = errno ? errno : EIO;
		rename(old_path, path);
		return error;
	}
	unlink(old_path);
	return 0;
}

static BOOL shadow_busy(const struct platform_file *file, const char *temporary_path)
{
	int slot;
	BOOL busy = FALSE;

	pthread_mutex_lock(&write_lock);
	for (slot = 0; slot < WRITABLE_FILE_SLOTS; slot++)
	{
		if (writable_files[slot] && writable_files[slot] != file &&
			writable_files[slot]->shadow == _shadow_active &&
			!strcmp(writable_files[slot]->temporary_path, temporary_path))
			busy = TRUE;
	}
	pthread_mutex_unlock(&write_lock);
	return busy;
}

/* the copy, empty: 0 or the errno. (file is the one it is for, if it exists
yet; two files do not share a copy.) */
static int shadow_create(const struct platform_file *file, const char *temporary_path, int *descriptor)
{
	if (shadow_busy(file, temporary_path))
		return EBUSY;
	*descriptor = open(temporary_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	return *descriptor < 0 ? (errno ? errno : EIO) : 0;
}

/* opens a file for writing under the save root: the descriptor, or -1 with
errno. A file that is made new or emptied gets its copy at once; one that
is kept is opened as it is, and gets its copy at its first write. */
static int shadow_open(const char *path, int flags, BOOL existed, int *state, unsigned long long *size,
	char *temporary_path, unsigned long temporary_size)
{
	int descriptor;
	int error;
	struct posix_file_information information;

	shadow_name(temporary_path, temporary_size, path, SHADOW_TEMPORARY_SUFFIX);
	if ((flags & O_TRUNC) || !existed)
	{
		error = shadow_create(NULL, temporary_path, &descriptor);
		if (error)
		{
			errno = error;
			return -1;
		}
		*state = _shadow_active;
		return descriptor;
	}
	/* (for reading only: the old contents are copied from it, and nothing is
	written to it; a descriptor that can write may cost a flush of the card
	at its close, which the game's listing of its saves would pay for every
	file it opens to check) */
	descriptor = open(path, (flags & ~O_ACCMODE) | O_RDONLY | O_CLOEXEC, 0644);
	if (descriptor >= 0)
	{
		*state = _shadow_deferred;
		*size = posix_fstat(descriptor, &information) == 0 ?
			((unsigned long long)information.size_high << 32) | information.size_low : 0;
	}
	return descriptor;
}

/* the first write to a file that is open as it was: its copy is made, with
the old contents when the write does not cover them, and every later read and
write goes to the copy. 0 or the errno */
static int shadow_activate(struct platform_file *file, BOOL copy_old)
{
	int descriptor;
	int error = shadow_create(file, file->temporary_path, &descriptor);
	off_t position;

	if (error)
		return error;
	position = lseek(file->descriptor, 0, SEEK_CUR);
	if (copy_old)
	{
		char *buffer = malloc(SHADOW_COPY_SIZE);
		unsigned long long offset = 0;

		if (!buffer)
			error = ENOMEM;
		while (!error)
		{
			ssize_t count = pread(file->descriptor, buffer, SHADOW_COPY_SIZE, (off_t)offset);
			ssize_t done = 0;

			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0)
				error = errno ? errno : EIO;
			if (count <= 0)
				break;
			while (done < count && !error)
			{
				ssize_t result = write(descriptor, buffer + done, (size_t)(count - done));

				if (result < 0 && errno == EINTR)
					continue;
				if (result <= 0)
					error = (result < 0 && errno) ? errno : ENOSPC;
				else
					done += result;
			}
			offset += (unsigned long long)count;
		}
		free(buffer);
	}
	if (!error && lseek(descriptor, position < 0 ? 0 : position, SEEK_SET) < 0)
		error = errno ? errno : EIO;
	if (error)
	{
		close(descriptor);
		unlink(file->temporary_path);
		return error;
	}
	close(file->descriptor);
	file->descriptor = descriptor;
	file->shadow = _shadow_active;
	return 0;
}

/* before a write or a resize: the copy is made if it is time. The write
starts at offset, or at the file's position when it is not positioned. */
static BOOL shadow_before_write(struct platform_file *file, BOOL positioned, unsigned long long offset,
	DWORD count, BOOL resize)
{
	int error;

	if (file->shadow != _shadow_deferred)
		return TRUE;
	if (!positioned)
	{
		unsigned long low, high;

		offset = posix_seek(file->descriptor, 0, 0, SEEK_CUR, &low, &high) == 0 ?
			((unsigned long long)high << 32) | low : 1;
	}
	error = shadow_activate(file, resize || offset != 0 || count < file->original_size);
	if (error)
	{
		platform_log("save: no copy of %s to write to (error %d)", file->path, error);
		platform_set_last_error_from_errno(error);
		file->damaged = TRUE;
		return FALSE;
	}
	return TRUE;
}

/* the game closes a file with a copy: the copy takes the file's place when
every write went through, and is dropped (the old file stays) when one failed */
static void shadow_finish(struct platform_file *file)
{
	int error = 0;
	unsigned long long started;

	write_begin();
	if (file->damaged)
	{
		error = -1;
	}
	else if (file->modified)
	{
		/* (a copy nothing was written to, a file made empty, has nothing to sync) */
		started = card_now();
		if (fsync(file->descriptor) != 0)
			error = errno ? errno : EIO;
		card_end(_card_sync, started, file->path);
	}
	started = card_now();
	close(file->descriptor);
	card_end(_card_close, started, file->path);
	if (!error)
	{
		started = card_now();
		error = shadow_replace(file->temporary_path, file->path);
		card_end(_card_replace, started, file->path);
	}
	if (error)
	{
		unlink(file->temporary_path);
		if (error < 0)
			platform_log("save: %s was not written (a write failed); the old file stays", file->path);
		else
			platform_log("save: %s was not written (error %d); the old file stays", file->path, error);
	}
	write_end();
}

/* what a cut left in the save folders: the file that went aside
(<file>.wold) comes back where <file> is missing and goes where it is not,
and a half-written copy (<file>.wtmp) goes. Returns whether it changed
something (the folder is then read again). */
static BOOL shadow_sweep_directory(const char *directory, int depth)
{
	void *stream = posix_directory_open(directory);
	char name[256];
	char path[1400];
	char target[1400];
	struct posix_file_information information;
	BOOL changed = FALSE;

	if (!stream)
		return FALSE;
	while (!changed && posix_directory_next(stream, name, sizeof(name)))
	{
		snprintf(path, sizeof(path), "%s/%s", directory, name);
		if (shadow_name_has_suffix(name, SHADOW_OLD_SUFFIX))
		{
			snprintf(target, sizeof(target), "%.*s", (int)(strlen(path) - strlen(SHADOW_OLD_SUFFIX)), path);
			if (posix_stat(target, &information) == 0)
				changed = unlink(path) == 0;
			else
				changed = rename(path, target) == 0;
			if (changed)
				platform_log("save: %s put back after an interrupted write", target);
		}
		else if (shadow_name_has_suffix(name, SHADOW_TEMPORARY_SUFFIX))
		{
			changed = unlink(path) == 0;
		}
		else if (depth < SHADOW_SWEEP_DEPTH && posix_stat(path, &information) == 0 &&
			(information.flags & _posix_file_is_directory))
		{
			changed = shadow_sweep_directory(path, depth + 1);
		}
	}
	posix_directory_close(stream);
	return changed;
}

static void shadow_sweep(const char *root)
{
	int guard;

	for (guard = 0; guard < 1000 && shadow_sweep_directory(root, 0); guard++)
		;
}

/* ---------- files made again

port: the game writes its default player profiles and default game variants
(z:\saved\player_profiles\default_profile\, z:\saved\playlists\default_playlist\)
at every start: it empties their folder, then makes each file and writes the
same block into it (source/saved games/playlist_profile.c and player_profile.c).
Nothing in them is lost to a cut (the next start writes them again), so they
are not written beside themselves and synced, and what is already there is
left alone: DeleteFileA does not take them away, a file made again keeps its
contents until the game writes something else (a file opened to be emptied is
cut to what is written at its close), a write of the bytes the file holds is
not made, and a file is opened for reading until a write differs. A start
after the first writes nothing to the card for them; the first writes each
file once. */
static BOOL regenerated_wanted(const char *path)
{
	static const char *const folders[] =
	{
		"/z/saved/playlists/default_playlist/",
		"/z/saved/player_profiles/default_profile/"
	};
	const char *root = platform_save_root();
	size_t length = strlen(root);
	int index;

	if (!length || strncmp(path, root, length))
		return FALSE;
	for (index = 0; index < 2; index++)
	{
		if (!strncasecmp(path + length, folders[index], strlen(folders[index])))
			return TRUE;
	}
	return FALSE;
}

/* the file's descriptor is made one that can write, at the place it stood: 0
or the errno */
static int regenerated_upgrade(struct platform_file *file)
{
	int descriptor;
	off_t place;

	if (file->regenerated_for_write)
		return 0;
	place = lseek(file->descriptor, 0, SEEK_CUR);
	descriptor = open(file->path, O_RDWR | O_CLOEXEC, 0644);
	if (descriptor < 0)
		return errno ? errno : EIO;
	if (lseek(descriptor, place < 0 ? 0 : place, SEEK_SET) < 0)
	{
		int error = errno ? errno : EIO;

		close(descriptor);
		return error;
	}
	close(file->descriptor);
	file->descriptor = descriptor;
	file->regenerated_for_write = TRUE;
	return 0;
}

/* whether the file holds these bytes where they are to go */
static BOOL regenerated_holds(const struct platform_file *file, const void *buffer, DWORD count,
	unsigned long long offset)
{
	unsigned char held[512];
	DWORD done = 0;

	while (done < count)
	{
		DWORD step = count - done < sizeof(held) ? count - done : (DWORD)sizeof(held);
		ssize_t result = pread(file->descriptor, held, step, (off_t)(offset + done));

		if (result < (ssize_t)step || memcmp(held, (const char *)buffer + done, step))
			return FALSE;
		done += step;
	}
	return TRUE;
}

/* the file the game last made empty with CREATE_ALWAYS and closed without a write (file_create): the next
open of it for writing is the one that is to find it empty */
static char regenerated_emptied[1024];

/* the close of such a file: one that was to be emptied and was written is cut to what was written */
static void regenerated_finish(struct platform_file *file)
{
	struct posix_file_information information;

	if (file->truncate_pending && !file->modified)
		snprintf(regenerated_emptied, sizeof(regenerated_emptied), "%s", file->path);

	if (file->truncate_pending && file->modified && posix_fstat(file->descriptor, &information) == 0 &&
		information.size_high == 0 && information.size_low > file->written_end &&
		regenerated_upgrade(file) == 0)
		ftruncate(file->descriptor, (off_t)file->written_end);
}

/* ---------- how the files are read

port: the reads are counted by kind and logged about every two seconds while
there are any ("files: reads in 2.0 s: ..."), and a file that was read much
logs what it was read for as it closes. A read that is small, one into a
buffer that is not a multiple of 32 bytes in address, or from an offset that
is not a multiple of 512 (a sector) costs the card's driver more than one that
is none of these; one that is not next in line costs a seek, which on FAT
follows the file's chain of clusters from the one it stood at. */
static struct
{
	unsigned long reads, bytes, microseconds;
	unsigned long small; /* under 4 KB */
	unsigned long misaligned_buffer;
	unsigned long unaligned_offset;
	unsigned long seeks;
	unsigned long largest;
	unsigned long long reported;
} read_window;

static void read_counted(struct platform_file *file, const void *buffer, size_t count, unsigned long long offset,
	BOOL positioned, unsigned long microseconds, unsigned long long now)
{
	__sync_fetch_and_add(&read_window.reads, 1);
	__sync_fetch_and_add(&read_window.bytes, (unsigned long)count);
	__sync_fetch_and_add(&read_window.microseconds, microseconds);
	if (count < 4096)
		__sync_fetch_and_add(&read_window.small, 1);
	if (((unsigned long)buffer & 31) != 0)
		__sync_fetch_and_add(&read_window.misaligned_buffer, 1);
	if (positioned && (offset & 511) != 0)
		__sync_fetch_and_add(&read_window.unaligned_offset, 1);
	if (count > read_window.largest)
		read_window.largest = (unsigned long)count;
	file->reads++;
	file->read_bytes += count;
	file->read_microseconds += microseconds;
	if (!read_window.reported)
		read_window.reported = now;
	if (now - read_window.reported >= 2000000ULL)
	{
		platform_log("files: reads in %lu ms: %lu, %lu KB in %lu ms (largest %lu KB; %lu under 4 KB, %lu into a buffer "
			"not on 32 bytes, %lu from an offset not on 512, %lu seeks)",
			(unsigned long)((now - read_window.reported) / 1000), read_window.reads, read_window.bytes / 1024,
			read_window.microseconds / 1000, read_window.largest / 1024, read_window.small,
			read_window.misaligned_buffer, read_window.unaligned_offset, read_window.seeks);
		memset(&read_window, 0, sizeof(read_window));
		read_window.reported = now;
	}
}

static void file_report_reads(const struct platform_file *file)
{
	if (file->read_bytes >= 1024 * 1024)
		platform_log("files: %s closed after %lu reads, %lu KB in %lu ms (%lu KB a read)", file->path, file->reads,
			(unsigned long)(file->read_bytes / 1024), (unsigned long)(file->read_microseconds / 1000),
			(unsigned long)(file->read_bytes / 1024 / (file->reads ? file->reads : 1)));
}
#endif

static void file_destroy(struct platform_handle *handle)
{
	struct platform_file *file = handle->data;

#ifdef HALO_WII
	writable_unregister(file);
	if (file->shadow == _shadow_active)
	{
		shadow_finish(file);
	}
	else
	{
		unsigned long long started = card_now();

		if (file->regenerated)
			regenerated_finish(file);
		file_report_reads(file);
		close(file->descriptor);
		card_end(_card_close, started, file->path);
	}
	if (file->place_lock_ready)
		pthread_mutex_destroy(&file->place_lock);
#else
	close(file->descriptor);
#endif
	free(file);
}

static struct platform_file *file_from_handle(HANDLE handle)
{
	struct platform_handle *record = platform_handle_get(handle, _platform_handle_file);

	return record ? record->data : NULL;
}

HANDLE WINAPI CreateFileA(LPCSTR file_name, DWORD desired_access, DWORD share_mode,
	LPSECURITY_ATTRIBUTES security_attributes, DWORD creation_disposition,
	DWORD flags_and_attributes, HANDLE template_file)
{
	struct platform_file *file;
	struct platform_handle *handle;
	struct posix_file_information information;
	char path[1024];
	int flags = 0;
	int existed;
	int descriptor;
#ifdef HALO_WII
	int shadow = _shadow_none;
	unsigned long long original_size = 0;
	char temporary_path[1032] = "";
	BOOL regenerated = FALSE;
	BOOL regenerated_for_write = FALSE;
	BOOL truncate_pending = FALSE;
	unsigned long long started = card_now();
#endif

	(void)share_mode;
	(void)security_attributes;
	(void)flags_and_attributes;
	(void)template_file;

	platform_translate_path(file_name, path, sizeof(path));
	existed = posix_stat(path, &information) == 0;

	if ((desired_access & GENERIC_READ) && (desired_access & GENERIC_WRITE))
		flags = O_RDWR;
	else if (desired_access & GENERIC_WRITE)
		flags = O_WRONLY;
	else
		flags = O_RDONLY;

	switch (creation_disposition)
	{
	case CREATE_NEW:
		flags |= O_CREAT | O_EXCL;
		break;
	case CREATE_ALWAYS:
		flags |= O_CREAT | O_TRUNC;
		break;
	case OPEN_EXISTING:
		break;
	case OPEN_ALWAYS:
		flags |= O_CREAT;
		break;
	case TRUNCATE_EXISTING:
		flags |= O_TRUNC;
		break;
	default:
		SetLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if ((flags & O_TRUNC) && (flags & O_ACCMODE) == O_RDONLY)
		flags = (flags & ~O_ACCMODE) | O_RDWR;

	if (existed && (information.flags & _posix_file_is_directory))
	{
		SetLastError(ERROR_ACCESS_DENIED);
		return INVALID_HANDLE_VALUE;
	}

#ifdef HALO_WII
	if ((flags & O_ACCMODE) == O_WRONLY && !(flags_and_attributes & FILE_FLAG_NO_BUFFERING) &&
		(existed || (flags & O_CREAT)) && regenerated_wanted(path))
	{
		/* a file the game makes again at every start (xbox_files.c, "files made again") */
		if (existed && creation_disposition == CREATE_NEW)
		{
			SetLastError(ERROR_ALREADY_EXISTS);
			return INVALID_HANDLE_VALUE;
		}
		regenerated = TRUE;
		truncate_pending = existed && ((flags & O_TRUNC) || !strcmp(path, regenerated_emptied));
		regenerated_emptied[0] = '\0';
		flags &= ~O_TRUNC;
		regenerated_for_write = !existed;
		descriptor = existed ? open(path, O_RDONLY | O_CLOEXEC, 0644) : open(path, flags | O_CLOEXEC, 0644);
	}
	else if ((flags & O_ACCMODE) != O_RDONLY && !(flags_and_attributes & FILE_FLAG_NO_BUFFERING) &&
		(existed || (flags & O_CREAT)) && shadow_wanted(path))
	{
		if (existed && creation_disposition == CREATE_NEW)
		{
			SetLastError(ERROR_ALREADY_EXISTS);
			return INVALID_HANDLE_VALUE;
		}
		descriptor = shadow_open(path, flags, existed, &shadow, &original_size, temporary_path,
			sizeof(temporary_path));
	}
	else
#endif
	descriptor = open(path, flags | O_CLOEXEC, 0644);
	if (descriptor < 0)
	{
		platform_set_last_error_from_errno(errno);
		if (errno == ENOENT && !existed && (flags & O_CREAT))
			SetLastError(ERROR_PATH_NOT_FOUND);
#ifdef HALO_WII
		if (shadow != _shadow_none || temporary_path[0])
			platform_log("save: %s could not be opened for writing (error %d)", path, errno);
#endif
		return INVALID_HANDLE_VALUE;
	}

	file = calloc(1, sizeof(*file));
	handle = file ? platform_handle_new(_platform_handle_file, file, file_destroy) : NULL;
	if (!handle)
	{
		close(descriptor);
#ifdef HALO_WII
		if (shadow == _shadow_active)
			unlink(temporary_path);
#endif
		free(file);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	file->descriptor = descriptor;
	snprintf(file->path, sizeof(file->path), "%s", path);
#ifdef HALO_WII
	file->shadow = shadow;
	file->original_size = original_size;
	snprintf(file->temporary_path, sizeof(file->temporary_path), "%s", temporary_path);
	file->regenerated = regenerated;
	file->regenerated_for_write = regenerated_for_write;
	file->truncate_pending = truncate_pending;
	file->place = -1;
	/* a map and the like: read at offsets only, from a handle opened for it */
	if ((flags & O_ACCMODE) == O_RDONLY && (flags_and_attributes & FILE_FLAG_OVERLAPPED) &&
		pthread_mutex_init(&file->place_lock, NULL) == 0)
	{
		file->place_lock_ready = TRUE;
		file->positioned_only = TRUE;
	}
	if ((flags & O_ACCMODE) != O_RDONLY)
		writable_register(file);
	card_end(_card_open, started, path);
#endif

	/* Win32 reports whether an OPEN_ALWAYS/CREATE_ALWAYS target existed */
	SetLastError((existed && (creation_disposition == OPEN_ALWAYS || creation_disposition == CREATE_ALWAYS)) ?
		ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
	return handle;
}

#define READ_BOUNCE_SIZE 0x40000UL

#ifdef HALO_WII
/* port: a handle only read at offsets keeps its place: a read that starts where
the last ended (the map's tags, then its bitmaps in the order they were asked
for) is a read and nothing else, where pread (WiiLibs' wl_posix_pread) is a
seek to the offset, the read, and a seek back to where the handle stood, each
of them a walk along the file's chain of clusters on FAT. The handle's own
position is put back when something that uses it comes (file_end_positioned). */
static ssize_t read_at_place(struct platform_file *file, void *buffer, size_t count, unsigned long long offset)
{
	ssize_t result;

	pthread_mutex_lock(&file->place_lock);
	if (file->place != (long long)offset)
	{
		if (lseek(file->descriptor, (off_t)offset, SEEK_SET) == (off_t)-1)
		{
			int error = errno;

			file->place = -1;
			pthread_mutex_unlock(&file->place_lock);
			errno = error;
			return -1;
		}
		__sync_fetch_and_add(&read_window.seeks, 1);
	}
	result = read(file->descriptor, buffer, count);
	file->place = result > 0 ? (long long)offset + result : -1;
	pthread_mutex_unlock(&file->place_lock);
	return result;
}

/* the handle is used by position, or written: the descriptor stands where a handle never read by offset stands */
static void file_end_positioned(struct platform_file *file)
{
	if (!file->positioned_only)
		return;
	pthread_mutex_lock(&file->place_lock);
	file->positioned_only = FALSE;
	if (file->place >= 0)
		lseek(file->descriptor, 0, SEEK_SET);
	file->place = -1;
	pthread_mutex_unlock(&file->place_lock);
}
#endif

static ssize_t read_some(struct platform_file *file, void *buffer, size_t count, BOOL positioned,
	unsigned long long offset)
{
#ifdef HALO_WII
	if (positioned && file->positioned_only)
		return read_at_place(file, buffer, count, offset);
	/* (pread: three seeks) */
	__sync_fetch_and_add(&read_window.seeks, positioned ? 3 : 0);
#endif
	return positioned ?
		pread(file->descriptor, buffer, count, (off_t)offset) :
		read(file->descriptor, buffer, count);
}

struct platform_file_reads platform_file_reads;

static BOOL read_at(struct platform_file *file, LPVOID buffer, DWORD count, LPDWORD bytes_read,
	BOOL positioned, unsigned long long offset)
{
	DWORD total = 0;
	/* The renderer write-protects guest memory it caches (memory_watch.c),
	and the kernel fails a read() into a protected page instead of
	faulting. Guest memory is therefore filled through a bounce buffer: the
	copy faults like any other write, at the moment the data really lands,
	so a texture uploaded while the read is in flight is refreshed. */
#ifdef HALO_WII
	/* (the Wii protects none: straight in) */
	BOOL bounce = FALSE;
#else
	BOOL bounce = platform_is_contiguous(buffer) ||
		platform_is_contiguous((char *)buffer + (count ? count - 1 : 0));
#endif
	LARGE_INTEGER started, ended, frequency;
	char *staging = bounce ? malloc(count < READ_BOUNCE_SIZE ? count : READ_BOUNCE_SIZE) : NULL;

#ifdef HALO_WII
	if (!positioned)
		file_end_positioned(file);
#endif

	if (bounce && !staging)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	QueryPerformanceCounter(&started);
	while (total < count)
	{
		size_t wanted = count - total;
		ssize_t result;

		if (bounce && wanted > READ_BOUNCE_SIZE)
			wanted = READ_BOUNCE_SIZE;
		result = read_some(file, bounce ? staging : (char *)buffer + total, wanted, positioned, offset + total);
		if (result < 0)
		{
			if (errno == EINTR)
				continue;
			platform_set_last_error_from_errno(errno);
			if (bytes_read)
				*bytes_read = total;
			free(staging);
			return FALSE;
		}
		if (result == 0)
			break;
		if (bounce)
		{
			/* (where pages are watched by their hashes, port/web, so that a
			page long unchanged is checked again in the next frame) */
			memory_watch_prepare_write((char *)buffer + total, (unsigned long)result);
			memcpy((char *)buffer + total, staging, (size_t)result);
		}
		total += (DWORD)result;
	}
	free(staging);
	if (bytes_read)
		*bytes_read = total;
	platform_file_reads.calls++;
	platform_file_reads.bytes += total;
	QueryPerformanceCounter(&ended);
	QueryPerformanceFrequency(&frequency);
	platform_file_reads.microseconds += (unsigned long long)(ended.QuadPart - started.QuadPart) * 1000000ULL /
		(unsigned long long)frequency.QuadPart;
#ifdef HALO_WII
	read_counted(file, buffer, total, offset, positioned,
		(unsigned long)((unsigned long long)(ended.QuadPart - started.QuadPart) * 1000000ULL /
			(unsigned long long)frequency.QuadPart),
		(unsigned long long)ended.QuadPart * 1000000ULL / (unsigned long long)frequency.QuadPart);
#endif
	return TRUE;
}

#ifdef HALO_WII
#define write_at write_at_ungated
#endif
static BOOL write_at(struct platform_file *file, LPCVOID buffer, DWORD count, LPDWORD bytes_written,
	BOOL positioned, unsigned long long offset)
{
	DWORD total = 0;

	while (total < count)
	{
		ssize_t result = positioned ?
			pwrite(file->descriptor, (const char *)buffer + total, count - total, (off_t)(offset + total)) :
			write(file->descriptor, (const char *)buffer + total, count - total);

		if (result < 0)
		{
			if (errno == EINTR)
				continue;
			platform_set_last_error_from_errno(errno);
			if (bytes_written)
				*bytes_written = total;
			return FALSE;
		}
#ifdef HALO_WII
		if (result == 0)
		{
			/* port: a card with no room left may take no bytes without
			an error; this is the full disk, not a write to repeat */
			SetLastError(ERROR_DISK_FULL);
			if (bytes_written)
				*bytes_written = total;
			return FALSE;
		}
#endif
		total += (DWORD)result;
	}
	if (bytes_written)
		*bytes_written = total;
	return TRUE;
}

#ifdef HALO_WII
#undef write_at
/* port: a write is made whole before the Wii leaves, or not begun */
static BOOL write_at(struct platform_file *file, LPCVOID buffer, DWORD count, LPDWORD bytes_written,
	BOOL positioned, unsigned long long offset)
{
	BOOL result;
	unsigned long long started = card_now();

	write_begin();
	file->modified = TRUE;
	if (file->regenerated)
	{
		/* (xbox_files.c, "files made again": the bytes the file holds are not written again) */
		unsigned long long at = offset;
		int error = 0;

		if (!positioned)
		{
			off_t place = lseek(file->descriptor, 0, SEEK_CUR);

			at = place < 0 ? 0 : (unsigned long long)place;
		}
		if (regenerated_holds(file, buffer, count, at))
		{
			if (!positioned && lseek(file->descriptor, (off_t)(at + count), SEEK_SET) < 0)
				error = errno ? errno : EIO;
			result = error == 0;
			if (bytes_written)
				*bytes_written = error ? 0 : count;
		}
		else
		{
			error = regenerated_upgrade(file);
			result = error == 0 && write_at_ungated(file, buffer, count, bytes_written, positioned, offset);
		}
		if (error)
			platform_set_last_error_from_errno(error);
		if (result && at + count > file->written_end)
			file->written_end = at + count;
	}
	else if (shadow_before_write(file, positioned, offset, count, FALSE))
	{
		result = write_at_ungated(file, buffer, count, bytes_written, positioned, offset);
	}
	else
	{
		result = FALSE;
		if (bytes_written)
			*bytes_written = 0;
	}
	if (!result)
	{
		file->damaged = TRUE;
		platform_log("save: a write to %s failed (error %lu)", file->path, (unsigned long)GetLastError());
	}
	write_end();
	card_end(_card_write, started, file->path);
	return result;
}
#endif

static unsigned long long overlapped_offset(LPOVERLAPPED overlapped)
{
	return ((unsigned long long)overlapped->OffsetHigh << 32) | overlapped->Offset;
}

BOOL WINAPI ReadFile(HANDLE handle, LPVOID buffer, DWORD count, LPDWORD bytes_read, LPOVERLAPPED overlapped)
{
	struct platform_file *file = file_from_handle(handle);
	BOOL result;
	DWORD done = 0;

	if (!file)
		return FALSE;
	result = read_at(file, buffer, count, &done, overlapped != NULL, overlapped ? overlapped_offset(overlapped) : 0);
	if (bytes_read)
		*bytes_read = done;
	if (overlapped)
	{
		overlapped->Internal = result ? 0 : GetLastError();
		overlapped->InternalHigh = done;
		if (overlapped->hEvent)
			SetEvent(overlapped->hEvent);
	}
	return result;
}

BOOL WINAPI WriteFile(HANDLE handle, LPCVOID buffer, DWORD count, LPDWORD bytes_written, LPOVERLAPPED overlapped)
{
	struct platform_file *file = file_from_handle(handle);
	BOOL result;
	DWORD done = 0;

	if (!file)
		return FALSE;
	result = write_at(file, buffer, count, &done, overlapped != NULL, overlapped ? overlapped_offset(overlapped) : 0);
	if (bytes_written)
		*bytes_written = done;
	if (overlapped)
	{
		overlapped->Internal = result ? 0 : GetLastError();
		overlapped->InternalHigh = done;
		if (overlapped->hEvent)
			SetEvent(overlapped->hEvent);
	}
	return result;
}

/* ReadFileEx/WriteFileEx complete immediately; as on Win32, the completion
routine runs during the issuing thread's next alertable wait. */

static void file_completion_apc(void *routine, void *overlapped, void *unused)
{
	LPOVERLAPPED request = overlapped;

	(void)unused;
	((LPOVERLAPPED_COMPLETION_ROUTINE)routine)((DWORD)request->Internal, (DWORD)request->InternalHigh, request);
}

BOOL WINAPI ReadFileEx(HANDLE handle, LPVOID buffer, DWORD count, LPOVERLAPPED overlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine)
{
	struct platform_file *file = file_from_handle(handle);
	DWORD done = 0;
	BOOL result;

	if (!file || !overlapped)
	{
		if (file)
			SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	result = read_at(file, buffer, count, &done, TRUE, overlapped_offset(overlapped));
	overlapped->Internal = result ? ERROR_SUCCESS : GetLastError();
	if (result && done == 0 && count > 0)
		overlapped->Internal = ERROR_HANDLE_EOF;
	overlapped->InternalHigh = done;
	platform_queue_apc(file_completion_apc, (void *)completion_routine, overlapped, NULL);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI WriteFileEx(HANDLE handle, LPCVOID buffer, DWORD count, LPOVERLAPPED overlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine)
{
	struct platform_file *file = file_from_handle(handle);
	DWORD done = 0;
	BOOL result;

	if (!file || !overlapped)
	{
		if (file)
			SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	result = write_at(file, buffer, count, &done, TRUE, overlapped_offset(overlapped));
	overlapped->Internal = result ? ERROR_SUCCESS : GetLastError();
	overlapped->InternalHigh = done;
	platform_queue_apc(file_completion_apc, (void *)completion_routine, overlapped, NULL);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

DWORD WINAPI SetFilePointer(HANDLE handle, LONG distance, PLONG distance_high, DWORD method)
{
	struct platform_file *file = file_from_handle(handle);
	unsigned long low, high;
	int whence;

	if (!file)
		return INVALID_SET_FILE_POINTER;
	switch (method)
	{
	case FILE_BEGIN: whence = SEEK_SET; break;
	case FILE_CURRENT: whence = SEEK_CUR; break;
	case FILE_END: whence = SEEK_END; break;
	default:
		SetLastError(ERROR_INVALID_PARAMETER);
		return INVALID_SET_FILE_POINTER;
	}
#ifdef HALO_WII
	file_end_positioned(file);
#endif
	if (posix_seek(file->descriptor, distance,
		distance_high ? *distance_high : (distance < 0 ? -1 : 0), whence, &low, &high) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return INVALID_SET_FILE_POINTER;
	}
	if (distance_high)
		*distance_high = (LONG)high;
	SetLastError(ERROR_SUCCESS);
	return (DWORD)low;
}

DWORD WINAPI GetFileSize(HANDLE handle, LPDWORD size_high)
{
	struct platform_file *file = file_from_handle(handle);
	struct posix_file_information information;

	if (!file)
		return INVALID_FILE_SIZE;
	if (posix_fstat(file->descriptor, &information) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return INVALID_FILE_SIZE;
	}
	if (size_high)
		*size_high = information.size_high;
	SetLastError(ERROR_SUCCESS);
	return information.size_low;
}

BOOL WINAPI SetEndOfFile(HANDLE handle)
{
	struct platform_file *file = file_from_handle(handle);
	unsigned long low, high;

	if (!file)
		return FALSE;
#ifdef HALO_WII
	/* port: the resize goes to the copy of a file saved with one (xbox_files.c, "saves that survive a cut") */
	write_begin();
	file->modified = TRUE;
	if (file->regenerated ? regenerated_upgrade(file) != 0 : !shadow_before_write(file, FALSE, 0, 0, TRUE))
	{
		file->damaged = TRUE;
		write_end();
		return FALSE;
	}
#endif
	if (posix_seek(file->descriptor, 0, 0, SEEK_CUR, &low, &high) != 0 ||
		posix_truncate(file->descriptor, low, high) != 0)
	{
		platform_set_last_error_from_errno(errno);
#ifdef HALO_WII
		file->damaged = TRUE;
		platform_log("save: resizing %s failed (error %lu)", file->path, (unsigned long)GetLastError());
		write_end();
#endif
		return FALSE;
	}
#ifdef HALO_WII
	write_end();
#endif
	return TRUE;
}

BOOL WINAPI GetFileTime(HANDLE handle, LPFILETIME creation_time, LPFILETIME last_access_time, LPFILETIME last_write_time)
{
	struct platform_file *file = file_from_handle(handle);
	struct posix_file_information information;

	if (!file)
		return FALSE;
	if (posix_fstat(file->descriptor, &information) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	if (creation_time)
		platform_unix_time_to_filetime(information.creation_seconds, information.creation_nanoseconds, creation_time);
	if (last_access_time)
		platform_unix_time_to_filetime(information.access_seconds, information.access_nanoseconds, last_access_time);
	if (last_write_time)
		platform_unix_time_to_filetime(information.modification_seconds, information.modification_nanoseconds, last_write_time);
	return TRUE;
}

BOOL WINAPI SetFileTime(HANDLE handle, CONST FILETIME *creation_time, CONST FILETIME *last_access_time,
	CONST FILETIME *last_write_time)
{
	struct platform_file *file = file_from_handle(handle);
	unsigned long access_seconds = 0, access_nanoseconds = 0;
	unsigned long write_seconds = 0, write_nanoseconds = 0;

	(void)creation_time;
	if (!file)
		return FALSE;
	if (last_access_time)
		platform_filetime_to_unix_time(last_access_time, &access_seconds, &access_nanoseconds);
	if (last_write_time)
		platform_filetime_to_unix_time(last_write_time, &write_seconds, &write_nanoseconds);
#ifdef HALO_WII
	/* port: a file with a copy gets the times on the copy, which becomes the file */
	if (posix_set_file_times(file->shadow == _shadow_active ? file->temporary_path : file->path,
		access_seconds, access_nanoseconds, write_seconds, write_nanoseconds) != 0)
#else
	if (posix_set_file_times(file->path, access_seconds, access_nanoseconds, write_seconds, write_nanoseconds) != 0)
#endif
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

/* ---------- operations on paths */

static void fill_attribute_data(const struct posix_file_information *information, WIN32_FILE_ATTRIBUTE_DATA *data)
{
	data->dwFileAttributes = 0;
	if (information->flags & _posix_file_is_directory)
		data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
	if (information->flags & _posix_file_is_read_only)
		data->dwFileAttributes |= FILE_ATTRIBUTE_READONLY;
	if (!data->dwFileAttributes)
		data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
	platform_unix_time_to_filetime(information->creation_seconds, information->creation_nanoseconds, &data->ftCreationTime);
	platform_unix_time_to_filetime(information->access_seconds, information->access_nanoseconds, &data->ftLastAccessTime);
	platform_unix_time_to_filetime(information->modification_seconds, information->modification_nanoseconds, &data->ftLastWriteTime);
	data->nFileSizeHigh = information->size_high;
	data->nFileSizeLow = information->size_low;
}

/* posix_stat, timed on the Wii (the kind "attributes") */
static int timed_stat(const char *path, struct posix_file_information *information)
{
#ifdef HALO_WII
	unsigned long long started = card_now();
	int result = posix_stat(path, information);

	card_end(_card_attributes, started, path);
	return result;
#else
	return posix_stat(path, information);
#endif
}

DWORD WINAPI GetFileAttributesA(LPCSTR file_name)
{
	struct posix_file_information information;
	WIN32_FILE_ATTRIBUTE_DATA data;
	char path[1024];

	platform_translate_path(file_name, path, sizeof(path));
	if (timed_stat(path, &information) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return (DWORD)-1;
	}
	fill_attribute_data(&information, &data);
	return data.dwFileAttributes;
}

BOOL WINAPI GetFileAttributesExA(LPCSTR file_name, GET_FILEEX_INFO_LEVELS level, LPVOID file_information)
{
	struct posix_file_information information;
	char path[1024];

	if (level != GetFileExInfoStandard)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	platform_translate_path(file_name, path, sizeof(path));
	if (timed_stat(path, &information) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	fill_attribute_data(&information, file_information);
	return TRUE;
}

BOOL WINAPI SetFileAttributesA(LPCSTR file_name, DWORD attributes)
{
	char path[1024];
	struct posix_file_information information;

	platform_translate_path(file_name, path, sizeof(path));
	if (timed_stat(path, &information) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	/* only the read-only bit has a POSIX equivalent */
	/* port: the bit as it is already is not written again (the game makes a
	file ordinary before it deletes it; on FAT a change of the bit is a write
	of the card) */
	if (((information.flags & _posix_file_is_read_only) != 0) == ((attributes & FILE_ATTRIBUTE_READONLY) != 0))
		return TRUE;
	if (posix_set_read_only(path, (attributes & FILE_ATTRIBUTE_READONLY) != 0) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI DeleteFileA(LPCSTR file_name)
{
	char path[1024];
#ifdef HALO_WII
	struct posix_file_information information;
	unsigned long long started;
#endif

	platform_translate_path(file_name, path, sizeof(path));
#ifdef HALO_WII
	/* port: a file the game makes again at every start stays where it is
	(xbox_files.c, "files made again") */
	if (regenerated_wanted(path) && posix_stat(path, &information) == 0 &&
		!(information.flags & _posix_file_is_directory))
		return TRUE;
	started = card_now();
#endif
	if (unlink(path) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
#ifdef HALO_WII
	card_end(_card_unlink, started, path);
#endif
	return TRUE;
}

BOOL WINAPI MoveFileA(LPCSTR existing_file_name, LPCSTR new_file_name)
{
	char from[1024], to[1024];
	struct posix_file_information information;

	platform_translate_path(existing_file_name, from, sizeof(from));
	platform_translate_path(new_file_name, to, sizeof(to));
	if (posix_stat(to, &information) == 0)
	{
		SetLastError(ERROR_ALREADY_EXISTS);
		return FALSE;
	}
	if (rename(from, to) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI CopyFileA(LPCSTR existing_file_name, LPCSTR new_file_name, BOOL fail_if_exists)
{
	char from[1024], to[1024];
	char buffer[65536];
	int source, destination;
	BOOL success = TRUE;
#ifdef HALO_WII
	/* port: a copy made under the save root is written beside its name and
	takes it when whole ("saves that survive a cut") */
	char temporary[1032] = "";
	struct posix_file_information information;
#endif

	platform_translate_path(existing_file_name, from, sizeof(from));
	platform_translate_path(new_file_name, to, sizeof(to));
	source = open(from, O_RDONLY | O_CLOEXEC);
	if (source < 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
#ifdef HALO_WII
	if (shadow_wanted(to))
	{
		if (fail_if_exists && posix_stat(to, &information) == 0)
		{
			SetLastError(ERROR_ALREADY_EXISTS);
			close(source);
			return FALSE;
		}
		shadow_name(temporary, sizeof(temporary), to, SHADOW_TEMPORARY_SUFFIX);
		destination = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	}
	else
#endif
	destination = open(to, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0644);
	if (destination < 0)
	{
		platform_set_last_error_from_errno(errno);
		close(source);
		return FALSE;
	}
	for (;;)
	{
		ssize_t count = read(source, buffer, sizeof(buffer));

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
		{
			if (count < 0)
			{
				platform_set_last_error_from_errno(errno);
				success = FALSE;
			}
			break;
		}
		if (write(destination, buffer, (size_t)count) != count)
		{
			platform_set_last_error_from_errno(errno ? errno : ENOSPC);
			success = FALSE;
			break;
		}
	}
	close(source);
#ifdef HALO_WII
	if (temporary[0])
	{
		int error = 0;

		if (success && fsync(destination) != 0)
			error = errno ? errno : EIO;
		close(destination);
		if (success && !error)
			error = shadow_replace(temporary, to);
		if (!success || error)
		{
			if (success)
				platform_set_last_error_from_errno(error);
			unlink(temporary);
			platform_log("save: the copy %s was not made; the old file stays", to);
			return FALSE;
		}
		return TRUE;
	}
#endif
	close(destination);
	return success;
}

BOOL WINAPI CreateDirectoryA(LPCSTR path_name, LPSECURITY_ATTRIBUTES security_attributes)
{
	char path[1024];
#ifdef HALO_WII
	unsigned long long started;
#endif

	(void)security_attributes;
	platform_translate_path(path_name, path, sizeof(path));
#ifdef HALO_WII
	started = card_now();
#endif
	if (posix_make_directory(path) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
#ifdef HALO_WII
	card_end(_card_directory, started, path);
#endif
	return TRUE;
}

BOOL WINAPI RemoveDirectoryA(LPCSTR path_name)
{
	char path[1024];

	platform_translate_path(path_name, path, sizeof(path));
	if (rmdir(path) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR directory_name, PULARGE_INTEGER free_bytes_available,
	PULARGE_INTEGER total_bytes, PULARGE_INTEGER total_free_bytes)
{
	char path[1024];
	unsigned long free_low, free_high, total_low, total_high;

	platform_translate_path(directory_name ? directory_name : "d:\\", path, sizeof(path));
	if (posix_disk_space(path, &free_low, &free_high, &total_low, &total_high) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	if (free_bytes_available)
	{
		free_bytes_available->LowPart = free_low;
		free_bytes_available->HighPart = free_high;
	}
	if (total_free_bytes)
	{
		total_free_bytes->LowPart = free_low;
		total_free_bytes->HighPart = free_high;
	}
	if (total_bytes)
	{
		total_bytes->LowPart = total_low;
		total_bytes->HighPart = total_high;
	}
	return TRUE;
}

/* ---------- directory enumeration */

struct platform_find
{
	void *directory;
	char directory_path[1024];
	char pattern[MAX_PATH];
};

static void find_destroy(struct platform_handle *handle)
{
	struct platform_find *find = handle->data;

	posix_directory_close(find->directory);
	free(find);
}

/* Win32 wildcard match: '*' any run, '?' one character, case-insensitive;
"*.*" also matches names without a dot */
static BOOL wildcard_match(const char *pattern, const char *name)
{
	if (!strcmp(pattern, "*.*") || !strcmp(pattern, "*"))
		return TRUE;
	while (*pattern)
	{
		if (*pattern == '*')
		{
			while (*pattern == '*')
				pattern++;
			if (!*pattern)
				return TRUE;
			for (; *name; name++)
			{
				if (wildcard_match(pattern, name))
					return TRUE;
			}
			return FALSE;
		}
		if (!*name)
			return FALSE;
		if (*pattern != '?' && tolower((unsigned char)*pattern) != tolower((unsigned char)*name))
			return FALSE;
		pattern++;
		name++;
	}
	return *name == '\0';
}

static BOOL find_next_entry(struct platform_find *find, LPWIN32_FIND_DATAA data)
{
	char name[MAX_PATH];

	while (posix_directory_next(find->directory, name, sizeof(name)))
	{
		char full_path[1400];
		struct posix_file_information information;
		WIN32_FILE_ATTRIBUTE_DATA attributes;

		if (!wildcard_match(find->pattern, name))
			continue;
#ifdef HALO_WII
		/* port: the copies of files being saved are not the game's */
		if (shadow_name_has_suffix(name, SHADOW_TEMPORARY_SUFFIX) || shadow_name_has_suffix(name, SHADOW_OLD_SUFFIX))
			continue;
#endif
		snprintf(full_path, sizeof(full_path), "%s/%s", find->directory_path, name);
		if (posix_stat(full_path, &information) != 0)
			continue;
		fill_attribute_data(&information, &attributes);
		memset(data, 0, sizeof(*data));
		data->dwFileAttributes = attributes.dwFileAttributes;
		data->ftCreationTime = attributes.ftCreationTime;
		data->ftLastAccessTime = attributes.ftLastAccessTime;
		data->ftLastWriteTime = attributes.ftLastWriteTime;
		data->nFileSizeHigh = attributes.nFileSizeHigh;
		data->nFileSizeLow = attributes.nFileSizeLow;
		snprintf(data->cFileName, sizeof(data->cFileName), "%s", name);
		return TRUE;
	}
	SetLastError(ERROR_NO_MORE_FILES);
	return FALSE;
}

HANDLE WINAPI FindFirstFileA(LPCSTR file_name, LPWIN32_FIND_DATAA data)
{
	struct platform_find *find = calloc(1, sizeof(*find));
	struct platform_handle *handle;
	char xbox_directory[MAX_PATH];
	const char *separator = strrchr(file_name, '\\');
#ifdef HALO_WII
	unsigned long long started;
#endif

	if (!find)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	if (!separator)
		separator = strrchr(file_name, '/');
	if (separator)
	{
		snprintf(xbox_directory, sizeof(xbox_directory), "%.*s", (int)(separator - file_name), file_name);
		snprintf(find->pattern, sizeof(find->pattern), "%s", separator + 1);
	}
	else
	{
		xbox_directory[0] = '\0';
		snprintf(find->pattern, sizeof(find->pattern), "%s", file_name);
	}
	platform_translate_path(xbox_directory, find->directory_path, sizeof(find->directory_path));
#ifdef HALO_WII
	started = card_now();
#endif
	find->directory = posix_directory_open(find->directory_path);
#ifdef HALO_WII
	card_end(_card_directory, started, find->directory_path);
#endif
	if (!find->directory)
	{
		platform_set_last_error_from_errno(errno);
		free(find);
		return INVALID_HANDLE_VALUE;
	}
	handle = platform_handle_new(_platform_handle_find, find, find_destroy);
	if (!handle)
	{
		posix_directory_close(find->directory);
		free(find);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	if (!find_next_entry(find, data))
	{
		CloseHandle(handle);
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	return handle;
}

BOOL WINAPI FindNextFileA(HANDLE find_file, LPWIN32_FIND_DATAA data)
{
	struct platform_handle *handle = platform_handle_get(find_file, _platform_handle_find);

	if (!handle)
		return FALSE;
	return find_next_entry(handle->data, data);
}

/* FindClose is an XDK macro for CloseHandle */
