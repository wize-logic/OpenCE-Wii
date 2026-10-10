/*
TEST_INPUT.C

debug.test_input "bot:<seed>": a scripted player for the automated tests
(port/linux/game/network_test.c), different for each seed: it walks and
strafes in circles, turns, fires every few seconds, jumps now and then and
throws a grenade every seven seconds; "look:<seed>" stands still, only
turning and looking up and down (where remote players aim and whether they
stand). Each port's controller code adds it to controller 1
(port/linux/src/xinput_sdl.c, port/wii/src/xinput_wii.c).
*/

#include "test_input.h"
#include "port_config.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int test_input_checked;
static int test_input_seed = -1;
static int test_input_looking;
static int test_input_holding_action;
/* (when the action button began to be held: 0 until the next state) */
static unsigned long long test_input_holding_action_since;

static void test_input_read_settings(void)
{
	const char *setting;

	if (test_input_checked)
		return;
	test_input_checked = 1;
	setting = config_string("debug.test_input");
	if (!strncmp(setting, "bot:", 4))
		test_input_seed = atoi(setting + 4);
	else if (!strcmp(setting, "bot"))
		test_input_seed = 0;
	else if (!strncmp(setting, "look:", 5))
	{
		test_input_seed = atoi(setting + 5);
		test_input_looking = 1;
	}
}

BOOL test_input_active(void)
{
	test_input_read_settings();
	return test_input_seed >= 0;
}

void test_input_hold_action(int hold)
{
	if (hold && !test_input_holding_action)
		test_input_holding_action_since = 0;
	test_input_holding_action = hold;
}

/* the game's (source/game/game_time.c): its ticks, for debug.fixed_ticks */
unsigned char game_in_progress(void);
long game_time_get(void);

void test_input_gamepad(XINPUT_GAMEPAD *pad, unsigned long long milliseconds)
{
	static int fixed_ticks = -1;
	double t;

	if (!test_input_active())
		return;
	/* (with debug.fixed_ticks, the game's ticks are the clock in a game: the
	same input at the same tick on every machine, for a lockstep check) */
	if (fixed_ticks < 0)
		fixed_ticks = config_boolean("debug.fixed_ticks") ? 1 : 0;
	if (fixed_ticks && game_in_progress())
		milliseconds = (unsigned long long)game_time_get() * 1000ULL / 30ULL + 1;
	if (test_input_holding_action)
	{
		/* (standing still, the button held from a second on) */
		if (!test_input_holding_action_since)
			test_input_holding_action_since = milliseconds ? milliseconds : 1;
		if (milliseconds - test_input_holding_action_since >= 1000)
			pad->bAnalogButtons[XINPUT_GAMEPAD_X] = 255;
		return;
	}
	t = (double)milliseconds / 1000.0 + test_input_seed * 1.7;
	if (test_input_looking)
	{
		pad->sThumbRX = (SHORT)(sin(t * 0.5) * 14000.0);
		pad->sThumbRY = (SHORT)(sin(t * 0.3) * 32000.0);
		return;
	}
	pad->sThumbLY = (SHORT)(sin(t * 0.9) * 32000.0);
	pad->sThumbLX = (SHORT)(cos(t * 0.6 + test_input_seed) * 20000.0);
	pad->sThumbRX = (SHORT)(sin(t * 0.4) * 14000.0);
	if (fmod(t, 3.0) < 0.3)
		pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 255;
	if (fmod(t, 5.0) < 0.1)
		pad->bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
	if (fmod(t, 7.0) < 0.2)
		pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = 255;
}
