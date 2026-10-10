/*
WII_PROFILE.C

The sampling profiler (WiiLibs' wl/platform/profile.h) started from a file:
where the Broadway's time goes in a level, function by function. Off unless
the game's folder holds profile.txt (sd:/apps/halo/profile.txt, or the USB
drive's); nothing is allocated or started without it. It is a file, and not
a setting of port_config.c, so that the profiler stays out of the game's
settings and costs a normal run nothing.

profile.txt is lines of "name value" ("#" starts a comment):

	seconds	20	how long to sample, once the level is playing
	after	10	seconds of the level's game time to let go by first
	hz	500	samples a second (1 to 10000)
	depth	12	callers kept for each sample (0 to 16)
	skip	1	maps to let load first: 1 is the main menu's own
	repeat	1	levels to profile one after the other (a tour of levels)
	log	1	also write the samples to the log (Dolphin's OSREPORT), where
			wl_profile.py can read them from the runner's output
	note	b30	words for the dump's header (the level, the build)

A watcher thread (above the game's, asleep between a tenth of a second's
looks) waits for the level: the game's clock running, no map loading, the
clock past "after" seconds, and "skip" maps already loaded (the main menu is
the first map; a map is told by its load, or its clock starting over). Then
it samples for "seconds" (or until the next map starts to load), writes
profile<N>.txt into the game's folder and, with "log", the same lines into
the log, and stops. The dump's lines are what tools/wl_profile.py reads:

	tools/wl_profile.py build/wii/halo.elf build/wii/dolphin.log
	tools/wl_profile.py build/wii/halo.elf profile1.txt --tree 2

Cost: about a microsecond a sample (WiiLibs' README, "Sampling profiler"),
so 0.05% at the default 500 Hz; the ring is malloc'd (hz * seconds * (4 +
depth) words and a tenth over: 700 KB for the defaults), which is memory the
game does not have while it runs.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/platform/profile.h"

/* the game's clock and map state, declared here and not by its headers
(source/game/game.h): the types are the game's byte, long and float */
extern unsigned char game_time_initialized(void);
extern unsigned char game_in_progress(void);
extern long game_time_get(void);
extern unsigned char game_map_loading_in_progress(float *progress);
extern const char *wii_game_folder(void);
extern void *memalign(size_t alignment, size_t size); /* (newlib's; the port's malloc.h does not declare it) */

void wii_profile_arm(void);

#define PROFILE_TICKS_PER_SECOND 30
#define PROFILE_STACK_SIZE (16u * 1024u)
#define PROFILE_MAX_RING_BYTES (4u * 1024u * 1024u)
#define PROFILE_POLL_MS 100u

struct profile_settings
{
	unsigned long seconds;
	unsigned long after;
	unsigned long hz;
	unsigned long depth;
	unsigned long skip;
	unsigned long repeat;
	unsigned long log;
	char note[48];
};

static struct profile_settings settings;
static char folder[64];

/* The map the game is on, as far as a poll can tell. */
struct profile_watch
{
	unsigned long generation; /* maps seen: 1 is the first (the main menu's) */
	int loading;
	int reset_pending; /* a load seen, and the map's clock not yet */
	long last_time;
};

static int profile_parse_settings(const char *path)
{
	FILE *file = fopen(path, "r");
	char line[128];

	if (!file)
		return 0;
	settings.seconds = 20;
	settings.after = 10;
	settings.hz = 500;
	settings.depth = 12;
	settings.skip = 1;
	settings.repeat = 1;
	settings.log = 1;
	settings.note[0] = '\0';
	while (fgets(line, sizeof(line), file))
	{
		char name[16];
		char *value;
		unsigned long number;

		if (line[0] == '#' || sscanf(line, "%15s", name) != 1)
			continue;
		value = line + strspn(line, " \t");
		value += strlen(name);
		value += strspn(value, " \t=");
		number = strtoul(value, NULL, 10);
		if (!strcmp(name, "seconds"))
			settings.seconds = number;
		else if (!strcmp(name, "after"))
			settings.after = number;
		else if (!strcmp(name, "hz"))
			settings.hz = number;
		else if (!strcmp(name, "depth"))
			settings.depth = number;
		else if (!strcmp(name, "skip"))
			settings.skip = number;
		else if (!strcmp(name, "repeat"))
			settings.repeat = number;
		else if (!strcmp(name, "log"))
			settings.log = number;
		else if (!strcmp(name, "note"))
		{
			size_t length = strcspn(value, "\r\n");

			if (length >= sizeof(settings.note))
				length = sizeof(settings.note) - 1;
			memcpy(settings.note, value, length);
			settings.note[length] = '\0';
		}
	}
	fclose(file);
	if (settings.hz < 1)
		settings.hz = 1;
	if (settings.hz > 10000)
		settings.hz = 10000;
	if (settings.depth > WL_PROFILE_MAX_DEPTH)
		settings.depth = WL_PROFILE_MAX_DEPTH;
	if (settings.seconds < 1)
		settings.seconds = 1;
	if (settings.repeat < 1)
		settings.repeat = 1;
	return 1;
}

/* one look at the game: whether a level is playing now, and the maps seen so far */
static int profile_poll(struct profile_watch *watch, long *time_out)
{
	float progress;
	long time = -1;
	int loading = game_map_loading_in_progress(&progress) != 0;
	int playing = 0;

	if (loading && !watch->loading)
	{
		watch->generation++;
		watch->reset_pending = 1;
	}
	watch->loading = loading;
	if (!loading && game_time_initialized() && game_in_progress())
	{
		time = game_time_get();
		playing = 1;
		if (watch->generation == 0)
			watch->generation = 1; /* (the first map, whose load was too short to see) */
		else if (watch->reset_pending)
			watch->reset_pending = 0; /* (the clock of the map whose load was seen) */
		else if (time < watch->last_time)
			watch->generation++; /* (a clock started over, with no load seen) */
		watch->last_time = time;
	}
	*time_out = time;
	return playing;
}

static void profile_write_out(const WlProfileRing *ring, const WlProfileDumpInfo *info, unsigned long run)
{
	char path[96];
	WlResult result;

	snprintf(path, sizeof(path), "%s/profile%lu.txt", folder, run + 1);
	result = wl_profile_dump_file(ring, info, path);
	WL_LOG_I("profile", "%s: %s", path, wl_result_name(result));
	if (settings.log)
		wl_profile_dump_log(ring, info);
	wl_platform_log_flush();
}

/* one profile: from now for "seconds", or until the next map starts to load */
static void profile_run(struct profile_watch *watch, unsigned long run, long start_time)
{
	WlProfileRing ring;
	WlProfileDumpInfo info;
	char note[96];
	uint32_t words_per_sample = WL_PROFILE_HEADER_WORDS + (uint32_t)settings.depth;
	uint32_t samples = (uint32_t)(settings.seconds * settings.hz + settings.seconds * settings.hz / 10 + 64);
	size_t bytes = (size_t)samples * words_per_sample * 4u;
	uint32_t *storage;
	uint64_t started;
	unsigned long generation = watch->generation;
	long time;
	WlResult result;

	if (bytes > PROFILE_MAX_RING_BYTES)
	{
		samples = PROFILE_MAX_RING_BYTES / (words_per_sample * 4u);
		bytes = (size_t)samples * words_per_sample * 4u;
		WL_LOG_W("profile", "%lu s at %lu Hz do not fit %u KB: the ring holds %u samples", settings.seconds, settings.hz,
			(unsigned)(PROFILE_MAX_RING_BYTES / 1024u), (unsigned)samples);
	}
	storage = memalign(32, bytes);
	if (!storage)
	{
		WL_LOG_E("profile", "no memory for the ring (%u KB)", (unsigned)(bytes / 1024u));
		return;
	}
	if (wl_profile_ring_init(&ring, storage, (uint32_t)(bytes / 4u), (uint32_t)settings.depth,
		WL_PROFILE_STOP_WHEN_FULL) != WL_OK)
	{
		free(storage);
		return;
	}
	snprintf(note, sizeof(note), "%s map %lu game_time %ld", settings.note, generation, start_time);
	info.hz = (uint32_t)settings.hz;
	info.timebase_hz = 0;
	info.main_thread = 0;
	info.note = note;
	WL_LOG_I("profile", "sampling %lu Hz for %lu s, %u callers, %u KB: %s", settings.hz, settings.seconds,
		(unsigned)settings.depth, (unsigned)(bytes / 1024u), note);
	wl_platform_log_flush();
	started = wl_platform_time_us();
	result = wl_profile_start(&ring, (uint32_t)settings.hz);
	if (result != WL_OK)
	{
		WL_LOG_E("profile", "wl_profile_start: %s", wl_result_name(result));
		free(storage);
		return;
	}
	while (wl_platform_time_us() - started < (uint64_t)settings.seconds * 1000000u)
	{
		wl_platform_thread_sleep_ms(PROFILE_POLL_MS);
		profile_poll(watch, &time);
		if (watch->generation != generation)
			break;
	}
	wl_profile_stop();
	WL_LOG_I("profile", "%u samples in %lu ms (%u dropped)%s", (unsigned)ring.count,
		(unsigned long)((wl_platform_time_us() - started) / 1000u), (unsigned)ring.dropped,
		watch->generation != generation ? ", the map ended" : "");
	profile_write_out(&ring, &info, run);
	free(storage);
}

static void *profile_watcher(void *argument)
{
	struct profile_watch watch;
	unsigned long run;

	(void)argument;
	memset(&watch, 0, sizeof(watch));
	watch.last_time = -1;
	for (run = 0; run < settings.repeat; run++)
	{
		unsigned long generation;
		long time;

		/* the next level: past the maps to skip, its clock past "after" */
		for (;;)
		{
			int playing = profile_poll(&watch, &time);

			if (playing && watch.generation > settings.skip &&
				(unsigned long)time >= settings.after * PROFILE_TICKS_PER_SECOND)
				break;
			wl_platform_thread_sleep_ms(PROFILE_POLL_MS);
		}
		generation = watch.generation;
		profile_run(&watch, run, time);
		/* a tour: wait for the next map to begin */
		while (run + 1 < settings.repeat && watch.generation == generation)
		{
			wl_platform_thread_sleep_ms(PROFILE_POLL_MS);
			profile_poll(&watch, &time);
		}
	}
	WL_LOG_I("profile", "done");
	return NULL;
}

/* from __wrap_main (wii_boot.c), once the SD card and the USB drive are
mounted: reads profile.txt and, if it is there, starts the watcher */
void wii_profile_arm(void)
{
	static WlPlatformThread thread;
	char path[96];
	void *stack;

	snprintf(folder, sizeof(folder), "%s", wii_game_folder());
	snprintf(path, sizeof(path), "%s/profile.txt", folder);
	if (!profile_parse_settings(path))
		return;
	stack = memalign(32, PROFILE_STACK_SIZE);
	if (!stack || wl_platform_thread_create(&thread, profile_watcher, NULL, stack, PROFILE_STACK_SIZE,
		WL_THREAD_PRIO_NET) != WL_OK)
	{
		WL_LOG_E("profile", "no thread for the watcher");
		return;
	}
	WL_LOG_I("profile", "%s: %lu s at %lu Hz after %lu s of the map after %lu skipped, %lu time(s)", path,
		settings.seconds, settings.hz, settings.after, settings.skip, settings.repeat);
}
