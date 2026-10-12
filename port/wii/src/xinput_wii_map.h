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
/* The pointer's play radius (input.pointer_steadiness): the pointer holds
still while its reading is within this share of the half screen of the held
position. wii_pointer_steadiness takes the setting as it is written (a
number out of range or not a number is the default, 0.03; 0 is none). */
#define WII_POINTER_STEADINESS_DEFAULT 0.03f
#define WII_POINTER_STEADINESS_MAX 0.25f
float wii_pointer_steadiness(double setting);
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

/* The rumble. The game gives each Xbox controller two motors, each 0 to
65535; a Wii controller's has one, on or off. The strength is the stronger of
the two, and the motor is switched on and off from tick to tick so that it
is on for a fraction of the ticks equal to the strength (the part of the
strength the motor has not yet been given carries from one tick to the next,
so half strength is on one tick in two and nine tenths on nine in ten). Zero switches it off at once. Otherwise it
is not switched again within WII_RUMBLE_SWITCH_US of its last switch, so
that it takes at most a hundred switches a second whatever the ticks' rate:
the Remote is sent a report for each. */
#define WII_RUMBLE_FULL 65535u
#define WII_RUMBLE_SWITCH_US 10000u

struct wii_rumble
{
	long carry;
	BOOL on;
	uint64_t switched_us;
};

unsigned wii_rumble_strength(unsigned left, unsigned right);
/* one tick at now_us with the strength wanted: whether the motor is on */
BOOL wii_rumble_duty(struct wii_rumble *rumble, unsigned strength, uint64_t now_us);

#endif
