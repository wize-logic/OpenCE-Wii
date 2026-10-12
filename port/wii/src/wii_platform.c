/*
WII_PLATFORM.C

The Wii's answers to the platform services the desktop builds take from
SDL (port/linux/src/sdl_platform.c): the log, messages and quitting, and the
desktop's own settings (display modes, window sizes, audio devices, the
clipboard, rebinding keys), which a console has none of. The log goes to
WiiLibs' sink: Dolphin's OSREPORT log, the console on the screen in debug
builds, and a file on the SD card once one is mounted.
*/

#include "platform.h"

#include <stdarg.h>
#include <stdio.h>

#include "wl/platform/platform.h"
#include "wl/platform/stop.h"

/* (xinput_wii.c) */
void wii_rumble_stop(void);

/* ---------- the log

Each line carries the seconds since the first one, for seeing where the
time goes while nothing is drawn (loading). */

void platform_log(const char *format, ...)
{
	static uint64_t first_us;
	char line[512];
	va_list arguments;
	uint64_t now = wl_platform_time_us();

	if (!first_us)
		first_us = now;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	WL_LOG_I("halo", "%7.2f %s", (double)(now - first_us) / 1e6, line);
}

/* ---------- messages and quitting */

/* (a box of its own on the desktop: the log only, until the Wii has a way
to show one over the game. The callers' messages are about multiplayer, which
the Wii does not play, and none stops the game; one that does is
wii_stop_no_maps below, on WiiLibs' stop screen) */
void platform_show_message(const char *title, const char *message)
{
	platform_log("%s: %s", title, message);
}

/* the game cannot run without its maps: the stop screen (WiiLibs', on the
console's text screen) says where to put them and waits for HOME or START,
then returns to the loader. Does not come back. */
void wii_stop_no_maps(void)
{
	wl_platform_stop_screen(
		"The game's maps were not found.\n"
		"\n"
		"Put the converted Xbox maps in sd:/apps/halo/maps\n"
		"(or usb:/apps/halo/maps). They are made from the\n"
		"Xbox's maps by tools/wii_convert_maps.py.\n"
		"\n"
		"SD card: %s. USB drive: %s.",
		wl_platform_fs_mounted(WL_DEVICE_SD) ? "found" : "not found",
		wl_platform_fs_mounted(WL_DEVICE_USB) ? "found" : "not found");
}

/* the menus' Quit: back to the loader (the Homebrew Channel). WiiLibs' exit
winds the game down first (wii_system.c): a save finishes, the audio and the
motors stop, the log and the SD card are written out. */
void platform_request_quit(void)
{
	/* (the controllers' motors stopped first, whatever else is under way) */
	wii_rumble_stop();
	wl_platform_input_shutdown();
	wl_platform_exit(WL_EXIT_LOADER);
}

/* the desktop's first start copies the maps out of a disc image; the Wii's
come converted on its SD card (port/wii/README.md) */
BOOL platform_offer_game_data(const char *destination)
{
	(void)destination;
	return FALSE;
}

/* ---------- the desktop's settings, which a console has none of

(declared in sdl_platform.h, which the Wii cannot include: it is SDL's) */

#define PLATFORM_AUDIO_DEVICE_NAME_SIZE 128

int platform_display_resolutions(long *widths, long *heights, int maximum)
{
	(void)widths;
	(void)heights;
	(void)maximum;
	return 0;
}

int platform_window_sizes(long *widths, long *heights, int maximum)
{
	(void)widths;
	(void)heights;
	(void)maximum;
	return 0;
}

int platform_audio_devices(int recording, char (*names)[PLATFORM_AUDIO_DEVICE_NAME_SIZE], int maximum)
{
	(void)recording;
	(void)names;
	(void)maximum;
	return 0;
}

void platform_display_apply(void)
{
}

/* the game ticks and draws at 30 Hz, as on the Xbox: no frames between the
ticks to interpolate */
int halo_interpolation_enabled(void)
{
	return 0;
}

/* no keyboard and mouse controls to rebind */
void platform_binding_capture_begin(void)
{
}

int platform_binding_capture_poll(int *input)
{
	(void)input;
	return 3;
}

void platform_scoreboard_scroll(int open, long *notches, long *pages)
{
	(void)open;
	*notches = 0;
	*pages = 0;
}

int platform_clipboard_get(char *text, int size)
{
	if (size > 0)
		text[0] = '\0';
	return 0;
}

void platform_clipboard_set(const char *text)
{
	(void)text;
}
