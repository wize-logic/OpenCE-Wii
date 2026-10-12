/*
WII_STRAP.H

The wrist strap reminder (display.wii_strap_reminder) as a state machine apart
from the game, the console and the screen, so that tools/wii_input_check.py
runs it on the host. It is given the controllers each time the game reads
them. At the start it shows a plain screen, ignoring the controllers for the
first second (a button held from the loader does nothing), and goes on when A
is pressed after that; the game is given no controller while the screen is up,
and until the A that went on is let go. wii_home_game.c draws it with WiiLibs'
overlay. port/wii/README.md ("The controllers") says what the person sees.
*/

#ifndef __WII_STRAP_H
#define __WII_STRAP_H

#include "platform.h"

#include "wl/platform/platform.h"

/* the controllers are ignored for this long after the screen first shows */
#define WII_STRAP_IGNORE_US 1000000u
/* the A that goes on is kept from the game until it is let go, or this long */
#define WII_STRAP_SWALLOW_US 1500000u

enum wii_strap_state
{
	WII_STRAP_OFF,
	WII_STRAP_WAITING,
	WII_STRAP_UP,
	WII_STRAP_SWALLOWING,
	WII_STRAP_DONE
};

struct wii_strap
{
	enum wii_strap_state state;
	uint64_t shown_us;
	uint64_t gone_us;
	BOOL accept_was;
	/* a Nunchuk is connected, as of the last look */
	BOOL nunchuk;
};

/* enabled: the setting. The screen shows at the first look. */
void wii_strap_init(struct wii_strap *strap, BOOL enabled);

/* One look at the controllers (raw is what WiiLibs read; extra_nav the
navigation of a scripted controller, wii_home.h). Returns whether the screen
is up. */
BOOL wii_strap_step(struct wii_strap *strap, const WlRawInput *raw, unsigned extra_nav, uint64_t now_us);

/* the screen is up: the game sees no controller and no rumble */
BOOL wii_strap_up(const struct wii_strap *strap);
/* and for a moment after, while the A that went on is let go */
BOOL wii_strap_blocks_input(const struct wii_strap *strap);
/* A would go on now (the first second is over) */
BOOL wii_strap_ready(const struct wii_strap *strap, uint64_t now_us);

/* the words, a line each: 0 is the title, 1 and 2 ask for the strap, 3 is
the line for a Nunchuk (NULL when none is connected), 4 asks for room, 5 is
the prompt (drawn dim until wii_strap_ready); NULL past the end */
const char *wii_strap_line(const struct wii_strap *strap, int index);

#endif
