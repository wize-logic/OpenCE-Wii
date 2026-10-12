/*
ATOMIC_SAVES.C (test)

The Wii's saves in port/linux/src/xbox_files.c ("saves that survive a cut": a
file opened for writing under the save root is written beside itself and takes
its place when the game closes it), built with HALO_WII on a real directory.
test_atomic_saves.py takes the code from the sources as under_test.inc.

Each case is a sequence of the Win32 calls the game makes to save a file
(source/saved games/game_state_xbox.c, player_profile.c):
	profile	one block written whole over the file
	savegame	the file opened or created, written whole with its header
		zeroed, then the header written (the persistent storage)
	patch	a few bytes written in the middle of a file
	append	bytes written at the end
	create	a file that is not there, written
	replace	a file emptied and written shorter (CREATE_ALWAYS)
	grow	a file made longer (SetEndOfFile)
	made	a new file written, then given its length (the persistent
		storage's first making)
The sequence is cut ("the power goes") before each of the calls that change
the card (open, write, fsync, rename, unlink, resize), and before each of the
writes a half of it is made, in turn; then the next start (shadow_sweep) looks
at what is left. The file must be the old one or the new one, whole, and no
copy may remain. Two kinds of card: one that renames over a file, and FAT's,
that does not. A copy renamed over the file must have been synced after its
last change. Then writes that fail (the card full, taking no bytes, read-only,
gone): the game is told, the old file stays, no copy remains.
*/

#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "harness.h"

#include <dirent.h>
#include <ftw.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <strings.h>
#include <strings.h>
#include <unistd.h>

#include "posix.h"
#include "posix_files.inc"

typedef int BOOL;
typedef unsigned long DWORD;
typedef long LONG;
typedef DWORD *LPDWORD;
typedef LONG *PLONG;
typedef const void *LPCVOID;
typedef void *LPVOID;
typedef void *HANDLE;
typedef const char *LPCSTR;
typedef void *LPSECURITY_ATTRIBUTES;
typedef struct _OVERLAPPED
{
	unsigned long Internal, InternalHigh, Offset, OffsetHigh;
	void *hEvent;
} OVERLAPPED, *LPOVERLAPPED;
#define WINAPI
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG)-1)
#include "constants.inc"

/* ---------- the platform's, as little as the code calls */

enum { _platform_handle_file = 1 };
struct platform_handle { long type; void *data; void (*destroy)(struct platform_handle *); };

static struct platform_handle *platform_handle_new(long type, void *data, void (*destroy)(struct platform_handle *))
{
	struct platform_handle *handle = calloc(1, sizeof(*handle));

	handle->type = type;
	handle->data = data;
	handle->destroy = destroy;
	return handle;
}

static struct platform_handle *platform_handle_get(HANDLE handle, long type)
{
	struct platform_handle *record = handle;

	return record && record != INVALID_HANDLE_VALUE && record->type == type ? record : NULL;
}

static BOOL CloseHandle(HANDLE handle)
{
	struct platform_handle *record = handle;

	record->destroy(record);
	free(record);
	return TRUE;
}

static DWORD last_error;
static DWORD GetLastError(void) { return last_error; }
static void SetLastError(DWORD error) { last_error = error; }
static BOOL SetEvent(HANDLE event) { (void)event; return TRUE; }
static void platform_queue_apc(void);
#define platform_last_error last_error

static char log_text[8192];
static void platform_log(const char *format, ...)
{
	va_list arguments;
	size_t used = strlen(log_text);

	va_start(arguments, format);
	vsnprintf(log_text + used, sizeof(log_text) - used - 1, format, arguments);
	va_end(arguments);
	strcat(log_text, "\n");
}

/* the card's timing (xbox_files.c, "where the card's time goes"): not what is tested here */
enum { _card_translate, _card_open, _card_directory, _card_attributes, _card_write, _card_sync, _card_close,
	_card_replace, _card_unlink };
static unsigned long long card_now(void) { return 0; }
static void card_end(int kind, unsigned long long started, const char *path) { (void)kind; (void)started; (void)path; }

/* WiiLibs' reader of a descriptor by offsets (the maps'): not what is tested here */
typedef struct WlPlacedReader { int taken; } WlPlacedReader;
static void wl_posix_placed_close(WlPlacedReader *reader) { (void)reader; }
#define WL_OK 0
static int wl_posix_placed_open(WlPlacedReader *reader, int descriptor) { (void)reader; (void)descriptor; return WL_OK; }

static char save_root[512];
static char data_root[512];
static const char *platform_save_root(void) { return save_root; }

/* z:\a\b -> <save root>/z/a/b; d:\a -> <data root>/a */
static void platform_translate_path(const char *xbox_path, char *host_path, unsigned long host_path_size)
{
	const char *root = (xbox_path[0] | 0x20) == 'd' ? data_root : save_root;
	char *cursor;

	snprintf(host_path, host_path_size, "%s/%c/%s", root, xbox_path[0] | 0x20, xbox_path + 3);
	if ((xbox_path[0] | 0x20) == 'd')
		snprintf(host_path, host_path_size, "%s/%s", root, xbox_path + 3);
	for (cursor = host_path; *cursor; cursor++)
	{
		if (*cursor == '\\')
			*cursor = '/';
	}
}

#include "errors.inc"

/* ---------- the card: calls that can be cut or fail */

static int operations;	/* the calls that change the card, counted */
static int cut_at;	/* cut before this one (0: never) */
static int cut_half;	/* a cut write makes half of itself first */
static jmp_buf cut_point;
static int fat;	/* a rename over a file fails, as on FAT */
static int dirty;	/* the copy has changes that are not synced */
static int copy_descriptor = -1;
static char kinds[512];	/* what each call was: 'w' a write, 'o' ... */

static int write_calls;
static int fail_write;	/* the write with this number fails, 0: none */
static int fail_write_errno;	/* its errno; 0: takes no bytes and says nothing */
static int fail_temporary_open;	/* errno of opening a copy; 0: opens */
static int fail_all_opens;	/* errno of opening anything; 0: opens */
static int fail_rename_errno;	/* errno of every rename; 0: none */

static int ends_with(const char *name, const char *suffix)
{
	size_t length = strlen(name);

	return length >= strlen(suffix) && !strcmp(name + length - strlen(suffix), suffix);
}

static void point(char kind)
{
	operations++;
	if (operations < (int)sizeof(kinds))
		kinds[operations] = kind;
	if (cut_at && operations == cut_at && !(kind == 'w' && cut_half))
		longjmp(cut_point, 1);
}

static int fi_open(const char *path, int flags, int mode)
{
	int descriptor;

	if (fail_all_opens)
	{
		errno = fail_all_opens;
		return -1;
	}
	if (fail_temporary_open && ends_with(path, ".wtmp"))
	{
		errno = fail_temporary_open;
		return -1;
	}
	if (flags & (O_CREAT | O_TRUNC))
		point('o');
	descriptor = open(path, flags, mode);
	if (descriptor >= 0 && ends_with(path, ".wtmp"))
		copy_descriptor = descriptor;
	return descriptor;
}

/* a write that is cut makes half of itself (cut_half), or fails */
static ssize_t fi_write(int descriptor, const void *buffer, size_t count, off_t offset, int positioned)
{
	ssize_t result;

	write_calls++;
	if (fail_write && write_calls == fail_write)
	{
		errno = fail_write_errno;
		return fail_write_errno ? -1 : 0;
	}
	point('w');
	if (cut_at && operations == cut_at && cut_half)
	{
		if (positioned)
			pwrite(descriptor, buffer, count / 2, offset);
		else
			write(descriptor, buffer, count / 2);
		longjmp(cut_point, 1);
	}
	if (descriptor == copy_descriptor)
		dirty = 1;
	result = positioned ? pwrite(descriptor, buffer, count, offset) : write(descriptor, buffer, count);
	return result;
}

static ssize_t fi_write_plain(int descriptor, const void *buffer, size_t count)
{
	return fi_write(descriptor, buffer, count, 0, 0);
}

static ssize_t fi_pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	return fi_write(descriptor, buffer, count, offset, 1);
}

static int fi_fsync(int descriptor)
{
	point('f');
	if (descriptor == copy_descriptor)
		dirty = 0;
	return 0;	/* (the card is a model: what is synced is what the cut cannot take back; see "dirty") */
}

static int fi_rename(const char *from, const char *to)
{
	struct stat information;

	point('r');
	if (fail_rename_errno)
	{
		errno = fail_rename_errno;
		return -1;
	}
	if (ends_with(from, ".wtmp"))
		CHECK(!dirty, "%s renamed with changes that are not synced", from);
	if (fat && stat(to, &information) == 0)
	{
		errno = EEXIST;
		return -1;
	}
	return rename(from, to);
}

static int fi_unlink(const char *path)
{
	point('u');
	return unlink(path);
}

#define open(path, ...) fi_open(path, __VA_ARGS__)
#define write(descriptor, buffer, count) fi_write_plain(descriptor, buffer, count)
#define pwrite(descriptor, buffer, count, offset) fi_pwrite(descriptor, buffer, count, offset)
#define fsync(descriptor) fi_fsync(descriptor)
#define rename(from, to) fi_rename(from, to)
#define unlink(path) fi_unlink(path)

static int truncate_descriptor(int descriptor, posix_ulong size_low, posix_ulong size_high)
{
	point('t');
	if (descriptor == copy_descriptor)
		dirty = 1;
	return posix_truncate(descriptor, size_low, size_high);
}
#define posix_truncate(descriptor, low, high) truncate_descriptor(descriptor, low, high)

/* ---------- the code under test */

#include "under_test.inc"

/* ---------- files */

#define SIZE 70000 /* more than two copying steps */

static unsigned char old_bytes[SIZE + 4096], new_bytes[SIZE + 4096];
static long old_size, new_size;
static int old_exists;

static void fill(unsigned char *bytes, long size, int seed)
{
	long index;

	for (index = 0; index < size; index++)
		bytes[index] = (unsigned char)(index * 7 + seed + (index >> 8));
}

static long read_file(const char *path, unsigned char *bytes, long capacity)
{
	FILE *file = fopen(path, "rb");
	long size;

	if (!file)
		return -1;
	size = (long)fread(bytes, 1, (size_t)capacity, file);
	fclose(file);
	return size;
}

static void write_file(const char *path, const unsigned char *bytes, long size)
{
	FILE *file = fopen(path, "wb");

	CHECK(file != NULL, "cannot make %s", path);
	fwrite(bytes, 1, (size_t)size, file);
	fclose(file);
}

#define SAVE_FILE "z:\\saved\\player_profiles\\a.sav"

static char file_path[1024];

static int remove_entry(const char *path, const struct stat *information, int type, struct FTW *position)
{
	(void)information;
	(void)type;
	(void)position;
	return remove(path);
}

static void remove_tree(const char *path)
{
	nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void make_directory(const char *path)
{
	char partial[1200];
	char *cursor;

	snprintf(partial, sizeof(partial), "%s", path);
	for (cursor = partial + 1; *cursor; cursor++)
	{
		if (*cursor == '/')
		{
			*cursor = '\0';
			mkdir(partial, 0755);
			*cursor = '/';
		}
	}
	mkdir(partial, 0755);
}

/* ---------- the game's sequences */

static const char *scenario;
static DWORD game_error;	/* the error the game last got */
static int game_failed;	/* a call the game made failed */

static void game_sets(BOOL result)
{
	if (!result)
	{
		game_failed = 1;
		game_error = GetLastError();
	}
}

static void game_seek(HANDLE file, LONG distance, DWORD method)
{
	if (SetFilePointer(file, distance, NULL, method) == INVALID_SET_FILE_POINTER)
	{
		game_failed = 1;
		game_error = GetLastError();
	}
}

static void game_write(HANDLE file, const unsigned char *bytes, long size)
{
	DWORD written;

	game_sets(WriteFile(file, bytes, (DWORD)size, &written, NULL) && (long)written == size);
}

/* the file as the sequence leaves it, in new_bytes (and new_size) */
static void game_sequence(void)
{
	HANDLE file;

	game_failed = 0;
	game_error = 0;
	if (!strcmp(scenario, "profile"))
	{
		file = CreateFileA(SAVE_FILE, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
		{
			game_sets(FALSE);
			return;
		}
		game_seek(file, 0, FILE_BEGIN);
		game_write(file, new_bytes, new_size);
		CloseHandle(file);
	}
	else if (!strcmp(scenario, "savegame"))
	{
		static unsigned char body[SIZE + 4096];

		file = CreateFileA(SAVE_FILE, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
		if (file == INVALID_HANDLE_VALUE)
		{
			game_sets(FALSE);
			return;
		}
		memcpy(body, new_bytes, (size_t)new_size);
		memset(body, 0, 2048);	/* the header is written last */
		game_seek(file, 0, FILE_BEGIN);
		game_write(file, body, new_size);
		game_seek(file, 0, FILE_BEGIN);
		game_write(file, new_bytes, 2048);
		CloseHandle(file);
	}
	else if (!strcmp(scenario, "patch"))
	{
		file = CreateFileA(SAVE_FILE, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
		{
			game_sets(FALSE);
			return;
		}
		game_seek(file, 40000, FILE_BEGIN);
		game_write(file, new_bytes + 40000, 100);
		CloseHandle(file);
	}
	else if (!strcmp(scenario, "head"))
	{
		file = CreateFileA(SAVE_FILE, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
		{
			game_sets(FALSE);
			return;
		}
		game_seek(file, 0, FILE_BEGIN);
		game_write(file, new_bytes, 2048);
		CloseHandle(file);
	}
	else if (!strcmp(scenario, "append"))
	{
		file = CreateFileA(SAVE_FILE, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
		{
			game_sets(FALSE);
			return;
		}
		game_seek(file, 0, FILE_END);
		game_write(file, new_bytes + old_size, new_size - old_size);
		CloseHandle(file);
	}
	else if (!strcmp(scenario, "create") || !strcmp(scenario, "replace"))
	{
		file = CreateFileA(SAVE_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
		{
			game_sets(FALSE);
			return;
		}
		game_write(file, new_bytes, new_size);
		CloseHandle(file);
	}
	else if (!strcmp(scenario, "grow"))
	{
		file = CreateFileA(SAVE_FILE, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
		if (file == INVALID_HANDLE_VALUE)
		{
			game_sets(FALSE);
			return;
		}
		game_seek(file, (LONG)new_size, FILE_BEGIN);
		game_sets(SetEndOfFile(file));
		CloseHandle(file);
	}
	else if (!strcmp(scenario, "made"))
	{
		static unsigned char zeroes[16 * 1024];

		file = CreateFileA(SAVE_FILE, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
		if (file == INVALID_HANDLE_VALUE)
		{
			game_sets(FALSE);
			return;
		}
		game_write(file, zeroes, sizeof(zeroes));
		game_seek(file, (LONG)new_size, FILE_BEGIN);
		game_sets(SetEndOfFile(file));
		CloseHandle(file);
	}
	else
	{
		CHECK(FALSE, "no such sequence: %s", scenario);
	}
}

/* the old file and the new one, for a sequence */
static void contents(void)
{
	fill(old_bytes, SIZE, 1);
	fill(new_bytes, SIZE, 2);
	old_exists = 1;
	old_size = SIZE;
	new_size = SIZE;
	if (!strcmp(scenario, "profile") || !strcmp(scenario, "savegame"))
	{
		new_size = SIZE;
	}
	else if (!strcmp(scenario, "patch"))
	{
		memcpy(new_bytes, old_bytes, SIZE);
		fill(new_bytes + 40000, 100, 9);
	}
	else if (!strcmp(scenario, "head"))
	{
		memcpy(new_bytes + 2048, old_bytes + 2048, SIZE - 2048);
	}
	else if (!strcmp(scenario, "append"))
	{
		memcpy(new_bytes, old_bytes, SIZE);
		fill(new_bytes + SIZE, 4000, 5);
		new_size = SIZE + 4000;
	}
	else if (!strcmp(scenario, "create"))
	{
		old_exists = 0;
		new_size = 30000;
	}
	else if (!strcmp(scenario, "replace"))
	{
		new_size = 20000;
	}
	else if (!strcmp(scenario, "grow"))
	{
		memcpy(new_bytes, old_bytes, SIZE);
		memset(new_bytes + SIZE, 0, 4000);
		new_size = SIZE + 4000;
	}
	else if (!strcmp(scenario, "made"))
	{
		old_exists = 0;
		memset(new_bytes, 0, SIZE);
		new_size = SIZE;
	}
}

/* a clean card with the old file */
static void card(int scenario_fat)
{
	char directory[1024];
	char *slash;

	remove_tree(save_root);
	make_directory(save_root);
	make_directory(data_root);
	platform_translate_path(SAVE_FILE, file_path, sizeof(file_path));
	snprintf(directory, sizeof(directory), "%s", file_path);
	slash = strrchr(directory, '/');
	*slash = '\0';
	make_directory(directory);
	contents();
	if (old_exists)
		write_file(file_path, old_bytes, old_size);
	fat = scenario_fat;
	operations = 0;
	cut_at = 0;
	cut_half = 0;
	dirty = 0;
	copy_descriptor = -1;
	write_calls = 0;
	fail_write = fail_write_errno = fail_temporary_open = fail_all_opens = fail_rename_errno = 0;
	log_text[0] = '\0';
	memset(writable_files, 0, sizeof(writable_files));
	writes_in_flight = 0;
	writes_closed = FALSE;
}

static int file_is(const char *path, const unsigned char *bytes, long size)
{
	static unsigned char found[SIZE + 8192];

	return read_file(path, found, sizeof(found)) == size && !memcmp(found, bytes, (size_t)size);
}

/* nothing but the file is left in the directory */
static void check_no_leftovers(const char *label)
{
	char directory[1024];
	char *slash;
	DIR *stream;
	struct dirent *entry;

	snprintf(directory, sizeof(directory), "%s", file_path);
	slash = strrchr(directory, '/');
	*slash = '\0';
	stream = opendir(directory);
	CHECK(stream != NULL, "directory");
	while ((entry = readdir(stream)))
	{
		CHECK(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || !strcmp(entry->d_name, "a.sav"),
			"%s: %s was left", label, entry->d_name);
	}
	closedir(stream);
}

/* the file is the old one or the new one, whole */
static void check_old_or_new(const char *label)
{
	int is_old = old_exists && file_is(file_path, old_bytes, old_size);
	int is_new = file_is(file_path, new_bytes, new_size);
	struct stat information;

	if (!is_old && !is_new)
	{
		if (stat(file_path, &information) != 0)
			CHECK(!old_exists, "%s: the file is gone", label);
		else
			CHECK(FALSE, "%s: the file is neither the old nor the new (size %ld)", label, (long)information.st_size);
	}
}

/* ---------- cases */

/* the sequence cut at every call, then the next start */
static void cut_everywhere(int scenario_fat)
{
	int total;
	int point_number;
	int half;

	card(scenario_fat);
	game_sequence();
	CHECK(!game_failed, "the sequence fails with nothing in its way (error %lu)", game_error);
	total = operations;
	CHECK(file_is(file_path, new_bytes, new_size), "the sequence does not write the new file");
	check_no_leftovers("complete");
	CHECK(total > 3, "too few calls counted (%d)", total);
	{
		char counted[sizeof(kinds)];

		memcpy(counted, kinds, sizeof(counted));
		for (point_number = 1; point_number <= total; point_number++)
		{
			for (half = 0; half < 2; half++)
			{
				char label[64];
				struct stat information;
				int survived;

				if (half && counted[point_number] != 'w')
					continue;
				card(scenario_fat);
				cut_at = point_number;
				cut_half = half;
				snprintf(label, sizeof(label), "cut %d%s of %d", point_number, half ? " (half)" : "", total);
				if (setjmp(cut_point) == 0)
				{
					game_sequence();
					CHECK(FALSE, "%s: not cut", label);
				}
				cut_at = 0;
				cut_half = 0;
				/* power back: nothing of the run is left, the card is as it is */
				memset(writable_files, 0, sizeof(writable_files));
				writes_in_flight = 0;
				/* before the sweep: the file, or (FAT, between the renames) its old self beside it */
				survived = stat(file_path, &information) == 0;
				if (survived)
				{
					check_old_or_new(label);
				}
				else
				{
					char old_name[1100];

					snprintf(old_name, sizeof(old_name), "%s.wold", file_path);
					CHECK(!old_exists || file_is(old_name, old_bytes, old_size),
						"%s: the file is missing and its old self is not beside it", label);
				}
				shadow_sweep(save_root);
				check_old_or_new(label);
				if (old_exists)
					CHECK(stat(file_path, &information) == 0, "%s: the old file is not back", label);
				check_no_leftovers(label);
				/* and the game goes on: the sequence done again (from the old file) is whole */
				if (old_exists)
					write_file(file_path, old_bytes, old_size);
				game_sequence();
				CHECK(!game_failed, "%s: the sequence fails after the start (error %lu)", label, game_error);
				CHECK(file_is(file_path, new_bytes, new_size), "%s: the sequence after it is not whole", label);
				check_no_leftovers(label);
			}
		}
	}
}

/* a write that fails: the game is told, the old file stays */
static void write_fails(int number, int error, DWORD game_wants)
{
	card(0);
	fail_write = number;
	fail_write_errno = error;
	game_sequence();
	CHECK(game_failed, "the game is not told of a write that fails");
	CHECK(game_error == game_wants, "the game gets error %lu, not %lu", game_error, game_wants);
	check_no_leftovers("a write failed");
	if (old_exists)
		CHECK(file_is(file_path, old_bytes, old_size), "a write that failed has changed the file");
	else
		CHECK(access(file_path, F_OK) != 0, "a write that failed has made a file");
	CHECK(strstr(log_text, "save: "), "a failed save is not in the log");
}

/* ---------- files made again (the default profiles and game variants, written at every start) */

#define REGEN_DIRECTORY "z:\\saved\\playlists\\default_playlist\\00"
#define REGEN_FILE REGEN_DIRECTORY "\\blam.lst"

static unsigned char block_a[512], block_b[300];

/* the game's sequence for one default variant (playlist_profile.c): the folder's files deleted, the file made
and closed, opened and written whole, closed */
static void regenerated_game(const unsigned char *bytes, long size)
{
	HANDLE file;
	char host[1100];

	/* (the folder is emptied: what is in it is deleted) */
	platform_translate_path(REGEN_FILE, host, sizeof(host));
	if (access(host, F_OK) == 0)
		DeleteFileA(REGEN_FILE);
	file = CreateFileA(REGEN_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE, "the file is not made");
	CloseHandle(file);
	file = CreateFileA(REGEN_FILE, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE, "the file is not opened");
	game_seek(file, 0, FILE_BEGIN);
	game_write(file, bytes, size);
	CloseHandle(file);
	CHECK(!game_failed, "the sequence fails (error %lu)", game_error);
}

/* a card with the folder (and the file, when it has bytes); returns the file's host path */
static const char *regenerated_card(const unsigned char *bytes, long size)
{
	static char path[1100];
	char directory[1100];
	char *slash;

	card(0);
	platform_translate_path(REGEN_FILE, path, sizeof(path));
	snprintf(directory, sizeof(directory), "%s", path);
	slash = strrchr(directory, '/');
	*slash = '\0';
	make_directory(directory);
	if (bytes)
		write_file(path, bytes, size);
	operations = 0;
	game_failed = 0;
	return path;
}

static void regenerated_nothing_left(const char *path)
{
	char directory[1100];
	char *slash;
	DIR *stream;
	struct dirent *entry;

	snprintf(directory, sizeof(directory), "%s", path);
	slash = strrchr(directory, '/');
	*slash = '\0';
	stream = opendir(directory);
	CHECK(stream != NULL, "directory");
	while ((entry = readdir(stream)))
		CHECK(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || !strcmp(entry->d_name, "blam.lst"),
			"%s was left", entry->d_name);
	closedir(stream);
}

int main(int argc, char **argv)
{
	const char *case_name;
	const char *name;
	char template_path[] = "/tmp/halo-atomic-XXXXXX";

	CHECK(argc == 2, "case name");
	alarm(20);
	CHECK(mkdtemp(template_path) != NULL, "mkdtemp");
	snprintf(save_root, sizeof(save_root), "%s/saves", template_path);
	snprintf(data_root, sizeof(data_root), "%s/data", template_path);
	case_name = argv[1];
	name = strchr(case_name, ':');
	CHECK(name != NULL, "case is <sequence>:<kind>");
	{
		static char sequence[64];

		snprintf(sequence, sizeof(sequence), "%.*s", (int)(name - case_name), case_name);
		scenario = sequence;
	}
	case_name = name + 1;

	CASE("cut") cut_everywhere(0);
	CASE("cut-fat") cut_everywhere(1);
	CASE("full") write_fails(1, ENOSPC, ERROR_DISK_FULL);
	CASE("full-later") write_fails(2, ENOSPC, ERROR_DISK_FULL);
	CASE("no-room") write_fails(1, 0, ERROR_DISK_FULL);
	CASE("read-only")
	{
		card(0);
		fail_temporary_open = EROFS;
		game_sequence();
		CHECK(game_failed, "the game is not told of a card it cannot write to");
		CHECK(game_error == ERROR_ACCESS_DENIED, "the game gets error %lu", game_error);
		check_no_leftovers("read-only");
		if (old_exists)
			CHECK(file_is(file_path, old_bytes, old_size), "the file changed");
		CHECK(strstr(log_text, "save: ") || !old_exists, "not in the log");
	}
	CASE("missing")
	{
		card(0);
		fail_all_opens = ENODEV;
		game_sequence();
		CHECK(game_failed, "the game is not told of a card that is gone");
		fail_all_opens = 0;
		if (old_exists)
			CHECK(file_is(file_path, old_bytes, old_size), "the file changed");
	}
	CASE("rename-fails")
	{
		card(0);
		fail_rename_errno = EIO;
		game_sequence();
		check_no_leftovers("rename fails");
		if (old_exists)
			CHECK(file_is(file_path, old_bytes, old_size), "the file changed");
		CHECK(strstr(log_text, "the old file stays") != NULL, "not in the log");
	}
	CASE("not-shadowed")
	{
		/* outside the save root, and the unbuffered revert buffer: written in place */
		HANDLE file;
		char path[1024];
		DWORD written;

		card(0);
		platform_translate_path("d:\\core\\x.bin", path, sizeof(path));
		snprintf(log_text, sizeof(log_text), "%s", "");
		make_directory(data_root);
		{
			char directory[1100];

			snprintf(directory, sizeof(directory), "%s/core", data_root);
			make_directory(directory);
		}
		file = CreateFileA("d:\\core\\x.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(file != INVALID_HANDLE_VALUE, "d:");
		WriteFile(file, "abc", 3, &written, NULL);
		{
			char copy[1100];

			snprintf(copy, sizeof(copy), "%s.wtmp", path);
			CHECK(access(copy, F_OK) != 0, "a file outside the saves has a copy");
		}
		CloseHandle(file);
		CHECK(file_is(path, (const unsigned char *)"abc", 3), "d: file");
		file = CreateFileA("z:\\savegame.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
			FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
		CHECK(file != INVALID_HANDLE_VALUE, "z: revert");
		WriteFile(file, "abc", 3, &written, NULL);
		platform_translate_path("z:\\savegame.bin", path, sizeof(path));
		CHECK(file_is(path, (const unsigned char *)"abc", 3), "the revert buffer is not written in place");
		CloseHandle(file);
	}
	CASE("two-writers")
	{
		/* a second file made over one with a copy is told: the copy is not shared */
		HANDLE first, second;

		card(0);
		first = CreateFileA(SAVE_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(first != INVALID_HANDLE_VALUE, "first");
		second = CreateFileA(SAVE_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(second == INVALID_HANDLE_VALUE, "the copy is shared by two files");
		CloseHandle(first);
	}
	CASE("create-new")
	{
		HANDLE file;

		card(0);
		file = CreateFileA(SAVE_FILE, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		CHECK(file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS,
			"CREATE_NEW over a file is not refused");
		CHECK(file_is(file_path, old_bytes, old_size), "the file changed");
	}
	CASE("sweep")
	{
		/* what a cut leaves, in a folder below the root: the old file beside nothing, the old file beside the
		new one, and copies */
		char path[1200];
		char directory[1100];

		card(0);
		snprintf(directory, sizeof(directory), "%s/z/deep/er", save_root);
		make_directory(directory);
		snprintf(path, sizeof(path), "%s/lost.sav.wold", directory);
		write_file(path, (const unsigned char *)"old", 3);
		snprintf(path, sizeof(path), "%s/both.sav", directory);
		write_file(path, (const unsigned char *)"new", 3);
		snprintf(path, sizeof(path), "%s/both.sav.wold", directory);
		write_file(path, (const unsigned char *)"old", 3);
		snprintf(path, sizeof(path), "%s/half.sav.wtmp", directory);
		write_file(path, (const unsigned char *)"half", 4);
		shadow_sweep(save_root);
		snprintf(path, sizeof(path), "%s/lost.sav", directory);
		CHECK(file_is(path, (const unsigned char *)"old", 3), "the old file is not back");
		snprintf(path, sizeof(path), "%s/both.sav", directory);
		CHECK(file_is(path, (const unsigned char *)"new", 3), "the new file was not kept");
		snprintf(path, sizeof(path), "%s/both.sav.wold", directory);
		CHECK(access(path, F_OK) != 0, "the old file beside the new one was left");
		snprintf(path, sizeof(path), "%s/half.sav.wtmp", directory);
		CHECK(access(path, F_OK) != 0, "the half copy was left");
	}
	CASE("find-skips-copies") { /* the enumeration is checked by the name test below */
		CHECK(shadow_name_has_suffix("a.sav.wtmp", SHADOW_TEMPORARY_SUFFIX), "suffix");
		CHECK(shadow_name_has_suffix("a.sav.wold", SHADOW_OLD_SUFFIX), "suffix");
		CHECK(!shadow_name_has_suffix("a.sav", SHADOW_OLD_SUFFIX), "suffix");
		CHECK(!shadow_name_has_suffix(".wold", SHADOW_OLD_SUFFIX), "a name that is only the suffix");
	}
	CASE("regen-first")
	{
		/* a card with none of them: made and written whole once, with no copy, no sync and no rename */
		const char *path;
		int number;

		fill(block_a, sizeof(block_a), 3);
		path = regenerated_card(NULL, 0);
		regenerated_game(block_a, sizeof(block_a));
		CHECK(file_is(path, block_a, sizeof(block_a)), "the file is not written");
		regenerated_nothing_left(path);
		for (number = 1; number <= operations; number++)
			CHECK(kinds[number] == 'o' || kinds[number] == 'w' || kinds[number] == 'u', "a call of kind %c on a file made again",
				kinds[number]);
	}
	CASE("regen-again")
	{
		/* a start after the first: the same bytes are there, and nothing is written to the card */
		const char *path;

		fill(block_a, sizeof(block_a), 3);
		path = regenerated_card(block_a, sizeof(block_a));
		regenerated_game(block_a, sizeof(block_a));
		CHECK(operations == 0, "%d calls changed the card for a file already there", operations);
		CHECK(file_is(path, block_a, sizeof(block_a)), "the file changed");
		regenerated_nothing_left(path);
	}
	CASE("regen-changed")
	{
		/* other bytes, and fewer: the file is the new ones and no longer */
		const char *path;

		fill(block_a, sizeof(block_a), 3);
		fill(block_b, sizeof(block_b), 4);
		path = regenerated_card(block_a, sizeof(block_a));
		regenerated_game(block_b, sizeof(block_b));
		CHECK(file_is(path, block_b, sizeof(block_b)), "the file is not the new bytes");
		regenerated_nothing_left(path);
	}
	CASE("regen-count")
	{
		/* not a check: the calls that change the card for one default variant, on a card with none (a start
		for the first time) and on one that has it (every start after), on FAT's kind of card */
		int counts[2][6];
		const char *kinds_named = "owfrut";
		int run, number;
		const char *path;

		fill(block_a, sizeof(block_a), 3);
		for (run = 0; run < 2; run++)
		{
			path = regenerated_card(run ? block_a : NULL, run ? sizeof(block_a) : 0);
			fat = 1;
			regenerated_game(block_a, sizeof(block_a));
			memset(counts[run], 0, sizeof(counts[run]));
			for (number = 1; number <= operations; number++)
				counts[run][strchr(kinds_named, kinds[number]) - kinds_named]++;
			CHECK(file_is(path, block_a, sizeof(block_a)), "the file is not written");
			printf("%s: %d calls: %d opens, %d writes, %d syncs, %d renames, %d unlinks, %d resizes\n",
				run ? "start after the first" : "first start", operations, counts[run][0], counts[run][1],
				counts[run][2], counts[run][3], counts[run][4], counts[run][5]);
		}
	}
	CASE("regen-other-deleted")
	{
		/* any other file is deleted when the game says so */
		card(0);
		CHECK(DeleteFileA(SAVE_FILE), "not deleted");
		CHECK(access(file_path, F_OK) != 0, "the file is still there");
	}
	remove_tree(template_path);
	return 0;
}
