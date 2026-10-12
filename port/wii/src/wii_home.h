/*
WII_HOME.H

The HOME button's menu and the Wii's notices (a Remote with nothing to move
with, a weak battery) as a state machine apart from the game, the console and
the screen, so that tools/wii_input_check.py runs it on the host. It is given
the controllers' state each time the game reads them, and tells its ops what
to do: pause the game, fade the sound, leave. wii_home_game.c makes the ops
of the game and draws the overlay. port/wii/README.md ("The controllers")
says what the person sees.
*/

#ifndef __WII_HOME_H
#define __WII_HOME_H

#include "platform.h"

#include "wl/platform/platform.h"

#define WII_HOME_PORTS 4

/* sound out and back in */
#define WII_HOME_FADE_MS 300
/* a Remote with no Nunchuk or Classic Controller is told so after this long
(a Nunchuk plugged in takes a moment to be seen), and a notice stays this long */
#define WII_HOME_NO_EXTENSION_US 1500000u
#define WII_HOME_NOTICE_US 7000000u
/* batteries are looked at this often, not each frame */
#define WII_HOME_BATTERY_CHECK_US 2000000u
/* a Remote's battery below this percent is low, and above the second is
good again (the notice is given once per fall) */
#define WII_HOME_BATTERY_LOW 10
#define WII_HOME_BATTERY_GOOD 15
/* buttons held when the menu closes do nothing in the game until they are let
go, or this long */
#define WII_HOME_SWALLOW_US 1500000u
/* a stick counts as a press of up or down beyond this (of 127) */
#define WII_HOME_STICK 64

enum wii_home_state
{
	WII_HOME_CLOSED,
	WII_HOME_MENU,
	WII_HOME_CONFIRM
};

enum wii_home_item
{
	WII_HOME_RESUME,
	WII_HOME_RESET,
	WII_HOME_LOADER,
	WII_HOME_ITEMS
};

/* the confirmation before the loader: what is chosen */
enum wii_home_choice
{
	WII_HOME_CANCEL,
	WII_HOME_LEAVE,
	WII_HOME_CHOICES
};

enum wii_home_notice
{
	WII_NOTICE_BATTERY,
	WII_NOTICE_NO_EXTENSION,
	WII_NOTICES
};

/* the navigation a controller gives the menu */
enum
{
	WII_NAV_UP = 1,
	WII_NAV_DOWN = 2,
	WII_NAV_ACCEPT = 4,
	WII_NAV_BACK = 8
};

/* what the machine asks of the rest of the port; each may be NULL */
struct wii_home_ops
{
	void *user;
	/* the game's time stopped, or running again */
	void (*set_paused)(void *user, BOOL paused);
	/* the sound's master gain (0 to 1) reached over the time */
	void (*audio_fade)(void *user, float gain, unsigned milliseconds);
	/* the game's main menu */
	void (*reset_to_title)(void *user);
	/* the Homebrew Channel (does not return on the console) */
	void (*leave_for_loader)(void *user);
	/* something is to be drawn, or nothing is any more */
	void (*visible)(void *user, BOOL visible);
	/* a line for the log */
	void (*log)(void *user, const char *text);
};

/* what only the game knows */
struct wii_home_world
{
	/* a level is running (no main menu is up) */
	BOOL in_play;
	/* the game's time is already stopped (its own pause menu) */
	BOOL game_paused;
	/* a player is on the controller of that port */
	BOOL local_player[WII_HOME_PORTS];
};

struct wii_home
{
	enum wii_home_state state;
	int items[WII_HOME_ITEMS];
	int item_count;
	/* the item chosen, or the choice while confirming */
	int selection;
	unsigned previous_nav;
	BOOL previous_home;
	BOOL we_paused;
	BOOL swallowing;
	uint64_t swallow_since_us;
	BOOL shown;

	/* the controllers as of the last look (for the menu's battery list) */
	BOOL present[WII_HOME_PORTS];
	unsigned char extension[WII_HOME_PORTS];
	unsigned char battery[WII_HOME_PORTS];

	/* notices */
	uint64_t next_battery_check_us;
	BOOL battery_known[WII_HOME_PORTS];
	BOOL battery_told[WII_HOME_PORTS];
	uint64_t no_extension_since_us[WII_HOME_PORTS];
	BOOL no_extension_told[WII_HOME_PORTS];
	uint64_t notice_until_us[WII_NOTICES][WII_HOME_PORTS];
	unsigned char notice_percent[WII_HOME_PORTS];
};

void wii_home_init(struct wii_home *home);

/* One look at the controllers: raw is what WiiLibs read, extra_nav the
navigation of a controller the console does not have (the scripted one,
test_input.c), home_extra a HOME press that is not on a controller (a
script's), world what the game says. */
void wii_home_step(struct wii_home *home, const WlRawInput *raw, unsigned extra_nav, BOOL home_extra,
	const struct wii_home_world *world, uint64_t now_us, const struct wii_home_ops *ops);

/* the menu is up: the game sees no controller, rumble is off */
BOOL wii_home_open(const struct wii_home *home);
/* and for a moment after, while a button held for the menu is let go */
BOOL wii_home_blocks_input(const struct wii_home *home);

/* what the controllers give the menu (an OR over all of them) */
unsigned wii_home_navigation(const WlRawInput *raw);
/* and what the scripted controller's gamepad gives it (test_input.c) */
unsigned wii_home_pad_navigation(const XINPUT_GAMEPAD *pad);
/* whether a controller's HOME is down */
BOOL wii_home_pressed(const WlRawInput *raw);

/* text for the menu and the notices */
const char *wii_home_item_label(int item);
const char *wii_home_choice_label(int choice);
const char *wii_home_confirm_text(void);
/* the notices still showing at that time, a line each, with a newline
after each; the length of the text (0 for none) */
unsigned wii_home_notice_text(const struct wii_home *home, uint64_t now_us, char *text, unsigned size);

#endif
