/*
XINPUT_WII_MAP.C

The Wii controllers as the Xbox gamepad (xinput_wii_map.h). The sticks and
triggers come from WiiLibs' input (wl/platform/wii/input_wii.c) already over
their whole travel (-127 to 127, 0 to 255; wl/platform/input_shape.h), and
the game makes its own dead zones (input_xbox.c), so no shaping is done here.

Halo's Xbox defaults (player_ui.c, the standard preset): the left stick moves
and the right looks, A jumps, B melees, X is the action and reload, Y switches
weapons, the left trigger throws a grenade, the right fires, Black switches
grenades, White is the flashlight, a click of the left stick crouches and of
the right zooms, Back shows the scores and Start pauses. The menus use A, B,
Back, the D-pad and the left stick.
*/

#include "xinput_wii_map.h"

#include "wl/platform/input_shape.h"

static void button(XINPUT_GAMEPAD *pad, BOOL held, int analog)
{
	if (held)
		pad->bAnalogButtons[analog] = 0xFF;
}

static void digital(XINPUT_GAMEPAD *pad, BOOL held, WORD flag)
{
	if (held)
		pad->wButtons |= flag;
}

/* a trigger: the analogue reading, and the whole travel when its digital
button says it is pressed (the Classic Controller Pro has only the button) */
static void trigger(XINPUT_GAMEPAD *pad, BOOL held, int analog, unsigned char reading)
{
	pad->bAnalogButtons[analog] = held ? 0xFF : reading;
}

static void sticks(XINPUT_GAMEPAD *pad, const signed char left[2], const signed char right[2])
{
	if (left)
	{
		pad->sThumbLX = wl_input_axis16(left[0]);
		pad->sThumbLY = wl_input_axis16(left[1]);
	}
	if (right)
	{
		pad->sThumbRX = wl_input_axis16(right[0]);
		pad->sThumbRY = wl_input_axis16(right[1]);
	}
}

/*
The Classic Controller (and the Pro): its buttons where the Xbox controller's
are, by position (its lower button b is A, the right one a is B, the left one
y is X, the upper x is Y). It has no stick clicks, so in play the D-pad's down
crouches and its up zooms (as well as being the D-pad), and ZL and ZR are
Black and White.
*/
void wii_map_classic(const WlRawRemote *remote, XINPUT_GAMEPAD *pad, BOOL menu)
{
	uint32_t held = remote->buttons;

	button(pad, held & WL_CC_B, XINPUT_GAMEPAD_A);
	button(pad, held & WL_CC_A, XINPUT_GAMEPAD_B);
	button(pad, held & WL_CC_Y, XINPUT_GAMEPAD_X);
	button(pad, held & WL_CC_X, XINPUT_GAMEPAD_Y);
	button(pad, held & WL_CC_ZL, XINPUT_GAMEPAD_BLACK);
	button(pad, held & WL_CC_ZR, XINPUT_GAMEPAD_WHITE);
	trigger(pad, held & WL_CC_L, XINPUT_GAMEPAD_LEFT_TRIGGER, remote->trigger[0]);
	trigger(pad, held & WL_CC_R, XINPUT_GAMEPAD_RIGHT_TRIGGER, remote->trigger[1]);
	digital(pad, held & WL_CC_UP, XINPUT_GAMEPAD_DPAD_UP);
	digital(pad, held & WL_CC_DOWN, XINPUT_GAMEPAD_DPAD_DOWN);
	digital(pad, held & WL_CC_LEFT, XINPUT_GAMEPAD_DPAD_LEFT);
	digital(pad, held & WL_CC_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT);
	if (!menu)
	{
		digital(pad, held & WL_CC_UP, XINPUT_GAMEPAD_RIGHT_THUMB);
		digital(pad, held & WL_CC_DOWN, XINPUT_GAMEPAD_LEFT_THUMB);
	}
	digital(pad, held & WL_CC_PLUS, XINPUT_GAMEPAD_START);
	digital(pad, held & WL_CC_MINUS, XINPUT_GAMEPAD_BACK);
	sticks(pad, remote->stick[0], remote->stick[1]);
}

/*
The Wii Remote, with a Nunchuk (without one it has no stick: its buttons and
pointer work). B fires, Z throws a grenade, A jumps, C crouches, 1 melees and
2 zooms; in play the D-pad's up switches weapons, its down is the action and
reload, its left Black and its right White (as well as being the D-pad). The
pointer aims (xinput_wii.c). In the menus B, which fires in play, is also the
back button (Remote B is the one under the index finger; 1 is out of reach of
a thumb on A).
*/
void wii_map_remote(const WlRawRemote *remote, XINPUT_GAMEPAD *pad, BOOL menu)
{
	uint32_t held = remote->buttons;

	button(pad, held & WL_WM_A, XINPUT_GAMEPAD_A);
	button(pad, held & WL_WM_1, XINPUT_GAMEPAD_B);
	button(pad, held & WL_NC_Z, XINPUT_GAMEPAD_LEFT_TRIGGER);
	button(pad, held & WL_WM_B, XINPUT_GAMEPAD_RIGHT_TRIGGER);
	digital(pad, held & WL_NC_C, XINPUT_GAMEPAD_LEFT_THUMB);
	digital(pad, held & WL_WM_2, XINPUT_GAMEPAD_RIGHT_THUMB);
	digital(pad, held & WL_WM_PLUS, XINPUT_GAMEPAD_START);
	digital(pad, held & WL_WM_MINUS, XINPUT_GAMEPAD_BACK);
	digital(pad, held & WL_WM_UP, XINPUT_GAMEPAD_DPAD_UP);
	digital(pad, held & WL_WM_DOWN, XINPUT_GAMEPAD_DPAD_DOWN);
	digital(pad, held & WL_WM_LEFT, XINPUT_GAMEPAD_DPAD_LEFT);
	digital(pad, held & WL_WM_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT);
	if (menu)
	{
		button(pad, held & WL_WM_B, XINPUT_GAMEPAD_B);
	}
	else
	{
		button(pad, held & WL_WM_UP, XINPUT_GAMEPAD_Y);
		button(pad, held & WL_WM_DOWN, XINPUT_GAMEPAD_X);
		button(pad, held & WL_WM_LEFT, XINPUT_GAMEPAD_BLACK);
		button(pad, held & WL_WM_RIGHT, XINPUT_GAMEPAD_WHITE);
	}
	if (remote->extension == WL_EXT_NUNCHUK)
		sticks(pad, remote->stick[0], NULL);
}

/*
The GameCube controller: A, B, X and Y are the Xbox's, the C stick looks, the
triggers are analogue and Z is Black. It has few buttons, so in play the
D-pad's up zooms, its down crouches, its left shows the scores (Back) and its
right is White (as well as being the D-pad); Start pauses.
*/
void wii_map_gamecube(const WlRawPad *gamecube, XINPUT_GAMEPAD *pad, BOOL menu)
{
	uint16_t held = gamecube->buttons;

	button(pad, held & WL_PAD_A, XINPUT_GAMEPAD_A);
	button(pad, held & WL_PAD_B, XINPUT_GAMEPAD_B);
	button(pad, held & WL_PAD_X, XINPUT_GAMEPAD_X);
	button(pad, held & WL_PAD_Y, XINPUT_GAMEPAD_Y);
	button(pad, held & WL_PAD_Z, XINPUT_GAMEPAD_BLACK);
	trigger(pad, held & WL_PAD_L, XINPUT_GAMEPAD_LEFT_TRIGGER, gamecube->trigger[0]);
	trigger(pad, held & WL_PAD_R, XINPUT_GAMEPAD_RIGHT_TRIGGER, gamecube->trigger[1]);
	digital(pad, held & WL_PAD_UP, XINPUT_GAMEPAD_DPAD_UP);
	digital(pad, held & WL_PAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN);
	digital(pad, held & WL_PAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT);
	digital(pad, held & WL_PAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT);
	if (!menu)
	{
		digital(pad, held & WL_PAD_UP, XINPUT_GAMEPAD_RIGHT_THUMB);
		digital(pad, held & WL_PAD_DOWN, XINPUT_GAMEPAD_LEFT_THUMB);
		digital(pad, held & WL_PAD_LEFT, XINPUT_GAMEPAD_BACK);
		button(pad, held & WL_PAD_RIGHT, XINPUT_GAMEPAD_WHITE);
	}
	digital(pad, held & WL_PAD_START, XINPUT_GAMEPAD_START);
	sticks(pad, gamecube->stick[0], gamecube->stick[1]);
}

/* ---------- the Nunchuk's thrust as melee */

enum
{
	MELEE_IDLE,
	MELEE_PENDING,
	MELEE_SHOWN
};

void wii_melee_press_flag(struct wii_melee_press *press, uint64_t now_us)
{
	press->state = MELEE_PENDING;
	press->flagged_us = now_us;
}

BOOL wii_melee_press_read(struct wii_melee_press *press, unsigned long poll, uint64_t now_us, BOOL menu)
{
	if (menu)
	{
		press->state = MELEE_IDLE;
		return FALSE;
	}
	if (press->state == MELEE_PENDING)
	{
		if (now_us - press->flagged_us > WII_MELEE_PENDING_US)
		{
			press->state = MELEE_IDLE;
			return FALSE;
		}
		press->state = MELEE_SHOWN;
		press->shown_poll = poll;
		return TRUE;
	}
	if (press->state == MELEE_SHOWN)
	{
		if (poll == press->shown_poll)
			return TRUE;
		press->state = MELEE_IDLE;
	}
	return FALSE;
}

void wii_map_melee(XINPUT_GAMEPAD *pad)
{
	button(pad, TRUE, XINPUT_GAMEPAD_B);
}

/* the pointer's turn: nothing within the box, then a ramp that reaches the
fastest turn at the screen's edge (wl_pointer_turn) */
void wii_map_pointer_look(float x, float y, float seconds, float yaw_rate, float pitch_rate, float *yaw,
	float *pitch)
{
	*yaw = -wl_pointer_turn(x, WII_POINTER_DEAD_ZONE) * yaw_rate * seconds;
	*pitch = -wl_pointer_turn(y, WII_POINTER_DEAD_ZONE) * pitch_rate * seconds;
}

/* the play radius the setting asks for: within 0 to the most, else the default */
float wii_pointer_steadiness(double setting)
{
	if (!(setting >= 0.0))
		return WII_POINTER_STEADINESS_DEFAULT;
	if (setting > (double)WII_POINTER_STEADINESS_MAX)
		return WII_POINTER_STEADINESS_MAX;
	return (float)setting;
}

/* ---------- the rumble */

unsigned wii_rumble_strength(unsigned left, unsigned right)
{
	unsigned strength = left > right ? left : right;

	return strength > WII_RUMBLE_FULL ? WII_RUMBLE_FULL : strength;
}

BOOL wii_rumble_duty(struct wii_rumble *rumble, unsigned strength, uint64_t now_us)
{
	BOOL wanted;

	if (strength == 0)
	{
		/* (off at once, whenever the last switch) */
		rumble->carry = 0;
		if (rumble->on)
		{
			rumble->on = FALSE;
			rumble->switched_us = now_us;
		}
		return FALSE;
	}
	if (strength > WII_RUMBLE_FULL)
		strength = WII_RUMBLE_FULL;
	rumble->carry += (long)strength;
	wanted = rumble->carry >= (long)(WII_RUMBLE_FULL / 2 + 1);
	if (wanted != rumble->on && (!rumble->switched_us || now_us - rumble->switched_us >= WII_RUMBLE_SWITCH_US))
	{
		rumble->on = wanted;
		rumble->switched_us = now_us;
	}
	/* (what the motor really did this tick is what is taken off the carry:
	a switch that had to wait is made up for by the ticks after it) */
	if (rumble->on)
		rumble->carry -= (long)WII_RUMBLE_FULL;
	return rumble->on;
}
