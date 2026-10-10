/*
XINPUT_WII_MAP.H

The Wii controllers' buttons and sticks as the Xbox gamepad state the game
reads (xinput_wii_map.c), apart from the Wii's libraries and the game so that
tools/wii_input_check.py runs it on the host. port/wii/README.md, "The
controllers", lists what each button does.
*/

#ifndef __XINPUT_WII_MAP_H
#define __XINPUT_WII_MAP_H

#include "platform.h"

#include "wl/platform/platform.h"

/* The pointer: no turn while it is within this fraction of the half screen's
width or height from the middle; a turn lost for no longer than the hold
below is carried on with the last position. */
#define WII_POINTER_DEAD_ZONE 0.35f
#define WII_POINTER_HOLD_US 150000u
/* the fastest turns, at the screen's edge, when the profile's look rates are
not known: the game's default look sensitivity (player_ui.c) */
#define WII_POINTER_DEFAULT_YAW_RADIANS_PER_SECOND 2.6f
#define WII_POINTER_DEFAULT_PITCH_RADIANS_PER_SECOND 1.4f

/* Each takes the controller's state and fills a zeroed gamepad. menu is
whether the game's menus are up: the D-pad's second jobs in play (the other
buttons it presses) would make a menu's own D-pad presses mean more than they
should, so they are left out, and the Remote's B, which fires in play, is the
menus' back button. */
void wii_map_classic(const WlRawRemote *remote, XINPUT_GAMEPAD *pad, BOOL menu);
void wii_map_remote(const WlRawRemote *remote, XINPUT_GAMEPAD *pad, BOOL menu);
void wii_map_gamecube(const WlRawPad *gamecube, XINPUT_GAMEPAD *pad, BOOL menu);

/* The turn for the pointer at (x, y), -1 to 1 across the screen (x right, y
down), over `seconds`, with the fastest rates at its edge: radians to add to
the view's yaw and pitch (a pointer to the right turns the view right, one
above the middle looks up). */
void wii_map_pointer_look(float x, float y, float seconds, float yaw_rate, float pitch_rate, float *yaw,
	float *pitch);

#endif
