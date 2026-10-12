/*
WII_SYSTEM.C

The console's own buttons, and leaving the game properly. WiiLibs latches the
reset button, the power button and a Wii Remote's power button
(wl_platform_system_init) and, from wii_system_start, a thread of its own
(wl_platform_system_watch_start) collects them every 16 ms, so that they work
while a map loads for tens of seconds as much as in play:

- reset leaves for the loader (the Homebrew Channel);
- power, or a Remote's power button, powers the console off.

Either way WiiLibs' wl_platform_exit winds the game down first: the hooks
below, the audio output stopped, every motor stopped, the log flushed and
closed, the SD card and the USB drive unmounted so that their caches are
written. The same wind-down runs when the game quits from its menu
(platform_request_quit, wii_platform.c) or stops with its stop screen.

The hooks:

- the motors: the rumble service is told to stop, and cannot switch one on
  again (xinput_wii.c);
- the files: a write of the game's, a save, is not cut in the middle. Writes
  under way finish (for at most three seconds), the ones that would start wait,
  and the files open for writing are synced (xbox_files.c, wii_files_quiesce).
  A save is several writes whose first bytes are written last (the header,
  game_state_xbox.c), so one that is stopped between two is not a valid save
  and the one before it stays.

The watcher thread also runs the rumble service's tick (xinput_wii.c): the
motors' on and off by a duty cycle, which a stalled game must not leave
running.
*/

#include "platform.h"

#include "wl/platform/platform.h"

/* (xinput_wii.c) */
void wii_rumble_start(void);
void wii_rumble_tick(void *unused);
void wii_rumble_stop(void);
/* (xbox_files.c) */
int wii_files_quiesce(unsigned timeout_ms);

/* the time a save in progress is given, at the most */
#define WRITES_TIMEOUT_MS 3000
/* the period of the watcher: the buttons are heard within it, and the rumble's
duty cycle ticks at it (62 a second, so under the hundred switches a second the
Remote is allowed) */
#define WATCH_PERIOD_MS 16

static void motors_hook(WlExitTarget target, void *user)
{
	(void)target;
	(void)user;
	wii_rumble_stop();
}

static void files_hook(WlExitTarget target, void *user)
{
	int unfinished;

	(void)target;
	(void)user;
	unfinished = wii_files_quiesce(WRITES_TIMEOUT_MS);
	if (unfinished)
		platform_log("leaving: %d file writes still under way after %d ms", unfinished, WRITES_TIMEOUT_MS);
}

/* after WiiLibs' heaps are up (the watcher's stack is one of them) */
void wii_system_start(void)
{
	wii_rumble_start();
	/* (the hooks run newest first: the motors, then the files) */
	if (wl_platform_shutdown_hook(files_hook, NULL) != WL_OK || wl_platform_shutdown_hook(motors_hook, NULL) != WL_OK)
		platform_log("the shutdown hooks could not be set");
	if (wl_platform_system_watch_start(WATCH_PERIOD_MS, wii_rumble_tick, NULL) != WL_OK)
		platform_log("the reset and power buttons are not watched (see the log)");
}
