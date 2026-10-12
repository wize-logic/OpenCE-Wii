/*
PATH_TRANSLATION.C (test)

The Wii's translation of an Xbox path to the card's (port/linux/src/xbox_files.c, translate_as_written): a path
that is there as it is written is made with one stat, and a file to be made in a folder that is there with one
stat of the folder and one look into it, where the look at every part of the path (the game's first way, kept
below as translate_reference, taken from the code before) opens and reads each directory along it. test_path_
translation.py takes the code from the sources as under_test.inc.

The answer must be the one the look at every part gives, for paths that are there, files that are not yet, other
spellings of the names, and dots; and the directories looked into are counted.
*/

#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "harness.h"

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "posix.h"
#include "posix_files.inc"

typedef int BOOL;

static int looks; /* the directories looked into */

static int counted_find(const char *directory, const char *name, char *result, posix_ulong result_size)
{
	looks++;
	return posix_find_entry_case_insensitive(directory, name, result, result_size);
}
#define posix_find_entry_case_insensitive counted_find

#include "under_test.inc"

/* the look at every part of the path, as the code did before translate_as_written */
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

		if (posix_find_entry_case_insensitive(resolved, component, on_disk, sizeof(on_disk)))
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

/* translated both ways; the answer is the same, and the directories looked into are those said (-1: not counted) */
static void check(const char *path, int fast_expected, int looks_expected)
{
	char fast[1100];
	char reference[1100];
	char resolved[1024];
	int made;

	snprintf(resolved, sizeof(resolved), "%s", root);
	looks = 0;
	made = translate_as_written(resolved, sizeof(resolved), path, fast, sizeof(fast));
	CHECK(made == fast_expected, "%s: the quick way %s", path, made ? "answers" : "does not answer");
	if (made)
	{
		if (looks_expected >= 0)
			CHECK(looks == looks_expected, "%s: %d directories looked into, not %d", path, looks, looks_expected);
		translate_reference(root, path, reference, sizeof(reference));
		CHECK(!strcmp(fast, reference), "%s: %s, but the look at every part gives %s", path, fast, reference);
	}
}

int main(int argc, char **argv)
{
	char template_path[] = "/tmp/halo-paths-XXXXXX";

	CHECK(argc == 1 || argc == 2, "no case is needed");
	CHECK(mkdtemp(template_path) != NULL, "mkdtemp");
	snprintf(root, sizeof(root), "%s", template_path);
	make("saved/playlists/default_playlist/00/blam.lst", 0);
	make("saved/Mixed/File.TXT", 0);
	make("maps/a10.map", 0);
	make("empty", 1);

	/* there as written: one stat, no directory looked into */
	check("saved\\playlists\\default_playlist\\00\\blam.lst", 1, 0);
	check("saved/playlists/default_playlist/00/blam.lst", 1, 0);
	check("\\saved\\playlists\\", 1, 0);
	check("maps\\a10.map", 1, 0);
	check("saved\\Mixed\\File.TXT", 1, 0);
	/* not made yet, in a folder that is there: the folder's one stat and one look for the file's spelling */
	check("saved\\playlists\\default_playlist\\00\\new.lst", 1, 1);
	check("saved\\Mixed\\file.txt", 1, 1);
	check("empty\\anything", 1, 1);
	/* dots are not parts of the path */
	check("saved\\.\\playlists\\..\\default_playlist\\00\\blam.lst", 1, 0);
	check("saved\\playlists\\...\\default_playlist", 1, 0);
	/* the folder is not there, or is another spelling (on a host that tells cases apart): not the quick way's
	to answer, and the look at every part is what is done */
	check("saved\\playlists\\default_playlist\\01\\new.lst", 0, -1);
	check("SAVED\\playlists\\default_playlist\\00\\blam.lst", 0, -1);
	check("saved\\nothing\\deeper\\file", 0, -1);
	check("", 0, -1);
	check("\\\\", 0, -1);
	/* a file where a folder is */
	check("maps\\a10.map\\inside", 0, -1);

	nftw(template_path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	return 0;
}
