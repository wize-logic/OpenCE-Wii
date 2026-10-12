/*
PATH_TRANSLATION.C (test)

The Wii's translation of an Xbox path to the card's (port/linux/src/xbox_files.c, append_path with the directories'
listings): a directory is read once and the spellings of its entries come from what was read, where the look at every
part of the path (the game's first way, kept below as translate_reference) opens and reads each directory along it at
every path. test_path_translation.py takes the code from the sources as under_test.inc (the listings
are WiiLibs', dircache.inc).

The answer must be the one the look at every part gives, for paths that are there, files that are not yet, folders
that are not there, other spellings of the names, names that differ in the case only, and dots; and the directories
read are counted: each once, whatever the number of paths, and never again by a second pass. A directory of more
entries than are kept, and a table of directories that is full, are looked into each time, and give the same answers.
*/

#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "harness.h"

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "posix.h"
#include "posix_files.inc"

typedef int BOOL;

static int looks; /* the directories looked into without a listing */

static int counted_find(const char *directory, const char *name, char *result, posix_ulong result_size)
{
	looks++;
	return posix_find_entry_case_insensitive(directory, name, result, result_size);
}
#define posix_find_entry_case_insensitive counted_find

#include "dircache.inc" /* the listings are WiiLibs' (wl/platform/dircache.c) */
#include "under_test.inc"

/* the look at every part of the path, as the code did before the listings */
static void translate_reference(const char *root, const char *cursor, char *host_path, unsigned long host_path_size)
{
	char resolved[1024];
	unsigned long length;

	snprintf(resolved, sizeof(resolved), "%s", root);
	while (*cursor)
	{
		char component[256];
		char on_disk[256];
		unsigned long component_length = 0;

		while (*cursor == '\\' || *cursor == '/')
			cursor++;
		while (*cursor && *cursor != '\\' && *cursor != '/' && component_length + 1 < sizeof(component))
			component[component_length++] = *cursor++;
		component[component_length] = '\0';
		if (!component_length || strspn(component, ". ") == component_length)
			continue;

		if (counted_find(resolved, component, on_disk, sizeof(on_disk)))
		{
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
		snprintf(resolved + length, sizeof(resolved) - length, "/%s", on_disk);
	}
	snprintf(host_path, host_path_size, "%s", resolved);
}

static char root[512];

static void make(const char *relative, int directory)
{
	char path[1100];
	char *cursor;

	snprintf(path, sizeof(path), "%s/%s", root, relative);
	for (cursor = path + strlen(root) + 1; *cursor; cursor++)
	{
		if (*cursor == '/')
		{
			*cursor = '\0';
			mkdir(path, 0755);
			*cursor = '/';
		}
	}
	if (directory)
	{
		mkdir(path, 0755);
	}
	else
	{
		FILE *file = fopen(path, "wb");

		CHECK(file != NULL, "cannot make %s", path);
		fclose(file);
	}
}

static int remove_entry(const char *path, const struct stat *information, int type, struct FTW *position)
{
	(void)information;
	(void)type;
	(void)position;
	return remove(path);
}

/* translated both ways: the answer is the same */
static void check(const char *path)
{
	char fast[1100];
	char reference[1100];
	char resolved[1024];
	int saved_looks;

	snprintf(resolved, sizeof(resolved), "%s", root);
	append_path(resolved, sizeof(resolved), path);
	snprintf(fast, sizeof(fast), "%s", resolved);
	saved_looks = looks;
	looks = 0;
	translate_reference(root, path, reference, sizeof(reference));
	looks = saved_looks;
	CHECK(!strcmp(fast, reference), "%s: %s, but the look at every part gives %s", path, fast, reference);
}

/* the paths of the cases, translated: the directories read since the last call */
static const char *const paths[] =
{
	/* there as written */
	"saved\\playlists\\default_playlist\\00\\blam.lst",
	"saved/playlists/default_playlist/00/blam.lst",
	"\\saved\\playlists\\",
	"maps\\a10.map",
	"saved\\Mixed\\File.TXT",
	/* not made yet, in a folder that is there, and in folders that are not */
	"saved\\playlists\\default_playlist\\00\\new.lst",
	"saved\\playlists\\default_playlist\\00\\other.lst",
	"empty\\anything",
	"saved\\playlists\\default_playlist\\01\\new.lst",
	"saved\\nothing\\deeper\\file",
	/* other spellings */
	"saved\\Mixed\\file.txt",
	"SAVED\\Playlists\\default_playlist\\00\\BLAM.LST",
	"Maps\\A10.MAP",
	/* names that differ in the case only: the one written wins, else the first the directory gives */
	"both\\name",
	"both\\Name",
	"both\\NAME",
	"both\\nAmE",
	/* dots are not parts of the path */
	"saved\\.\\playlists\\..\\default_playlist\\00\\blam.lst",
	"saved\\playlists\\...\\default_playlist",
	"",
	"\\\\",
	/* a file where a folder is */
	"maps\\a10.map\\inside",
	/* a folder of more entries than are kept */
	"large\\f000",
	"large\\F000",
	"large\\F599",
	"large\\f599",
	"large\\F700",
	"large\\g000",
};

static void translate_all(void)
{
	unsigned long index;

	for (index = 0; index < sizeof(paths) / sizeof(paths[0]); index++)
		check(paths[index]);
}

int main(int argc, char **argv)
{
	char template_path[] = "/tmp/halo-paths-XXXXXX";
	char name[64];
	unsigned long reads;
	int index;

	CHECK(argc == 1 || argc == 2, "no case is needed");
	CHECK(mkdtemp(template_path) != NULL, "mkdtemp");
	snprintf(root, sizeof(root), "%s", template_path);
	make("saved/playlists/default_playlist/00/blam.lst", 0);
	make("saved/Mixed/File.TXT", 0);
	make("maps/a10.map", 0);
	make("empty", 1);
	make("both/name", 0);
	make("both/Name", 0);
	for (index = 0; index < 600; index++)
	{
		snprintf(name, sizeof(name), "large/f%03d", index);
		make(name, 0);
	}

	/* every path gives the look at every part's answer */
	translate_all();

	/* a directory is read once: a second pass reads none and looks into the too large one only */
	reads = directory_cache.reads;
	looks = 0;
	translate_all();
	CHECK(directory_cache.reads == reads, "a second pass read %lu directories again", (unsigned long)directory_cache.reads - reads);
	CHECK(looks == 6, "%d looks into directories (the large one's, six paths), not six", looks);

	/* new files in one folder: no directory is read for them */
	reads = directory_cache.reads;
	looks = 0;
	for (index = 0; index < 16; index++)
	{
		char path[128];

		snprintf(path, sizeof(path), "saved\\playlists\\default_playlist\\00\\file%02d.lst", index);
		check(path);
	}
	CHECK(directory_cache.reads == reads && looks == 0, "16 new files in a listed folder read %lu directories, looked into %d",
		(unsigned long)directory_cache.reads - reads, looks);

	/* a file made since the directory was read is taken as written, a file removed since as it was: the same file
	on the card, which does not tell the cases apart */
	make("saved/Mixed/Made.TXT", 0);
	{
		char resolved[1024];

		snprintf(resolved, sizeof(resolved), "%s", root);
		append_path(resolved, sizeof(resolved), "saved\\Mixed\\made.txt");
		CHECK(!strcmp(resolved + strlen(root), "/saved/Mixed/made.txt"), "a file made since: %s", resolved);
	}
	{
		char path[1100];
		char resolved[1024];

		snprintf(path, sizeof(path), "%s/saved/Mixed/File.TXT", root);
		unlink(path);
		snprintf(resolved, sizeof(resolved), "%s", root);
		append_path(resolved, sizeof(resolved), "saved\\Mixed\\file.txt");
		CHECK(!strcmp(resolved + strlen(root), "/saved/Mixed/File.TXT"), "a file removed since: %s", resolved);
	}

	/* a table that is full: the 70 folders of one folder, then more than the table holds, give the same answers
	(the ones that were kept and the ones that were not) */
	for (index = 0; index < 80; index++)
	{
		snprintf(name, sizeof(name), "many/d%02d/x", index);
		make(name, 1);
	}
	for (index = 0; index < 80; index++)
	{
		char path[128];

		snprintf(path, sizeof(path), "many\\d%02d\\x\\file", index);
		check(path);
		snprintf(path, sizeof(path), "many\\D%02d\\X\\file", index);
		check(path);
	}
	CHECK(directory_cache.count == 64, "the table holds %lu directories, not 64", (unsigned long)directory_cache.count);
	for (index = 0; index < 80; index++)
	{
		char path[128];

		snprintf(path, sizeof(path), "many\\D%02d\\X\\Y", index);
		check(path);
	}

	nftw(template_path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	return 0;
}
