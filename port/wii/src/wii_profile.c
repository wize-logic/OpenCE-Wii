/*
WII_PROFILE.C

The sampling profiler (WiiLibs' wl/platform/profile.h) started from a file:
where the Broadway's time goes in a level, function by function. Off unless
the game's folder holds profile.txt (sd:/apps/halo/profile.txt, or the USB
drive's); nothing is allocated or started without it. It is a file, and not
a setting of port_config.c, so that the profiler stays out of the game's
settings and costs a normal run nothing.

The file, its keys, the watcher thread and the dump are WiiLibs'
(wl/platform/profile_arm.h, which has the keys and their defaults: seconds,
after, hz, depth, skip, repeat, log, note). What is Halo's is when to
sample, in three hooks the watcher asks every tenth of a second:

	ready		a level is playing: the game's clock running, no map
			loading, the clock past "after" seconds of game time, and
			"skip" maps already loaded (the main menu is the first map;
			a map is told by its load, or its clock starting over)
	stop		the next map has started to load
	advanced	the same, once the profile is over (for "repeat": a tour
			waits for the next map to begin)

The dump's lines are what tools/wl_profile.py reads:

	tools/wl_profile.py build/wii/halo.elf build/wii/dolphin.log
	tools/wl_profile.py build/wii/halo.elf profile1.txt --tree 2

Cost: about a microsecond a sample (WiiLibs' README, "Sampling profiler"),
so 0.05% at the default 500 Hz; the ring is malloc'd (hz * seconds * (4 +
depth) words and a tenth over: 700 KB for the defaults), which is memory the
game does not have while it runs.
*/

#include <stdio.h>

#include "wl/platform/platform.h"
#include "wl/platform/profile_arm.h"

/* the game's clock and map state, declared here and not by its headers
(source/game/game.h): the types are the game's byte, long and float */
extern unsigned char game_time_initialized(void);
extern unsigned char game_in_progress(void);
extern long game_time_get(void);
extern unsigned char game_map_loading_in_progress(float *progress);
extern const char *wii_game_folder(void);

void wii_profile_arm(void);

#define PROFILE_TICKS_PER_SECOND 30

/* The map the game is on, as far as a poll can tell. */
struct profile_watch
{
	unsigned long generation; /* maps seen: 1 is the first (the main menu's) */
	unsigned long profiled; /* the map being profiled */
	int loading;
	int reset_pending; /* a load seen, and the map's clock not yet */
	int ready; /* the game has been initialized (game_globals exists) */
	long last_time;
};

static struct profile_watch watch = {0, 0, 0, 0, 0, -1};
static WlProfileArm arm;

/* one look at the game: whether a level is playing now, and the maps seen so far */
static int profile_poll(long *time_out)
{
	float progress;
	long time = -1;
	int loading = 0;
	int playing = 0;

	*time_out = -1;
	/* (game_initialize makes the game's globals just before the clock: until the clock is up, they are not there) */
	if (!watch.ready)
	{
		if (!game_time_initialized())
			return 0;
		watch.ready = 1;
	}
	loading = game_map_loading_in_progress(&progress) != 0;
	if (loading && !watch.loading)
	{
		watch.generation++;
		watch.reset_pending = 1;
	}
	watch.loading = loading;
	if (!loading && game_time_initialized() && game_in_progress())
	{
		time = game_time_get();
		playing = 1;
		if (watch.generation == 0)
			watch.generation = 1; /* (the first map, whose load was too short to see) */
		else if (watch.reset_pending)
			watch.reset_pending = 0; /* (the clock of the map whose load was seen) */
		else if (time < watch.last_time)
			watch.generation++; /* (a clock started over, with no load seen) */
		watch.last_time = time;
	}
	*time_out = time;
	return playing;
}

/* the next level: past the maps to skip, its clock past "after" */
static bool profile_ready(void *user, const WlProfileArmSettings *settings, uint32_t run, char *note, size_t size)
{
	long time;

	(void)user;
	(void)run;
	if (!profile_poll(&time) || watch.generation <= settings->skip ||
		(unsigned long)time < (unsigned long)settings->after * PROFILE_TICKS_PER_SECOND)
		return false;
	watch.profiled = watch.generation;
	snprintf(note, size, "map %lu game_time %ld", watch.generation, time);
	return true;
}

/* the profile ends early when the map does, and a tour goes on when it has */
static bool profile_map_ended(void *user, const WlProfileArmSettings *settings, uint32_t run)
{
	long time;

	(void)user;
	(void)settings;
	(void)run;
	profile_poll(&time);
	return watch.generation != watch.profiled;
}

static void profile_issue(void *user, WlProfileArmIssue issue, const char *key, uint32_t line)
{
	(void)user;
	WL_LOG_W("profile", "profile.txt line %u: %s %s", (unsigned)line,
		issue == WL_PROFILE_ARM_UNKNOWN_KEY ? "no such setting" : "no number for", key);
}

/* from __wrap_main (wii_boot.c), once the SD card and the USB drive are
mounted: reads profile.txt and, if it is there, starts the watcher */
void wii_profile_arm(void)
{
	WlProfileArmConfig config;

	if (wl_profile_arm_config_load(&config, wii_game_folder(), profile_issue, NULL) != WL_OK)
		return;
	config.hooks.ready = profile_ready;
	config.hooks.stop = profile_map_ended;
	config.hooks.advanced = profile_map_ended;
	wl_profile_arm_launch(&arm, &config);
}
