/*
WII_INPUT_CHECK.C

A host check of port/wii/src/xinput_wii_map.c (tools/wii_input_check.py builds
and runs it, with WiiLibs' input_shape.c): synthetic WiiLibs input states
(WlRawRemote, WlRawPad) are mapped and the Xbox gamepad state the game would
read is compared with what port/wii/README.md, "The controllers", says. Each
physical button on its own, in play and in the menus; every Xbox control
reachable from every scheme; the sticks' axes and signs; the triggers
(analogue, digital, the Classic Controller Pro's); the pointer's turn.
*/

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "wl/platform/input_shape.h"
#include "xinput_wii_map.h"

static int failures;
static int checks;

#define CHECK(condition, ...) \
	do \
	{ \
		checks++; \
		if (!(condition)) \
		{ \
			failures++; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

/* a gamepad with only these controls down: analog bits (1 << index) and wButtons */
struct expected
{
	unsigned analog;
	unsigned buttons;
};

#define A (1u << XINPUT_GAMEPAD_A)
#define B (1u << XINPUT_GAMEPAD_B)
#define X (1u << XINPUT_GAMEPAD_X)
#define Y (1u << XINPUT_GAMEPAD_Y)
#define BLACK (1u << XINPUT_GAMEPAD_BLACK)
#define WHITE (1u << XINPUT_GAMEPAD_WHITE)
#define LT (1u << XINPUT_GAMEPAD_LEFT_TRIGGER)
#define RT (1u << XINPUT_GAMEPAD_RIGHT_TRIGGER)

#define UP XINPUT_GAMEPAD_DPAD_UP
#define DOWN XINPUT_GAMEPAD_DPAD_DOWN
#define LEFT XINPUT_GAMEPAD_DPAD_LEFT
#define RIGHT XINPUT_GAMEPAD_DPAD_RIGHT
#define START XINPUT_GAMEPAD_START
#define BACK XINPUT_GAMEPAD_BACK
#define LCLICK XINPUT_GAMEPAD_LEFT_THUMB
#define RCLICK XINPUT_GAMEPAD_RIGHT_THUMB

static unsigned analog_mask(const XINPUT_GAMEPAD *pad)
{
	unsigned mask = 0;
	int index;

	for (index = 0; index < 8; index++)
	{
		if (pad->bAnalogButtons[index])
			mask |= 1u << index;
	}
	return mask;
}

static const char *describe(const struct expected *e)
{
	static char text[8][96];
	static int next;
	char *out = text[next++ & 7];
	static const char *const analog[8] = {"A", "B", "X", "Y", "Black", "White", "LT", "RT"};
	static const char *const buttons[8] = {"up", "down", "left", "right", "Start", "Back", "Lclick", "Rclick"};
	int index;

	out[0] = '\0';
	for (index = 0; index < 8; index++)
	{
		if (e->analog & (1u << index))
			sprintf(out + strlen(out), "%s ", analog[index]);
	}
	for (index = 0; index < 8; index++)
	{
		if (e->buttons & (1u << index))
			sprintf(out + strlen(out), "%s ", buttons[index]);
	}
	return out[0] ? out : "nothing";
}

/* ---------- one physical button at a time */

struct button_case
{
	const char *name;
	unsigned long long bit;
	struct expected play;
	struct expected menu;
};

static void check_buttons(const char *scheme, const struct button_case *cases, int count,
	void (*map)(unsigned long long, XINPUT_GAMEPAD *, BOOL), unsigned *reached)
{
	int index;

	for (index = 0; index < count; index++)
	{
		int menu;

		for (menu = 0; menu < 2; menu++)
		{
			XINPUT_GAMEPAD pad;
			struct expected got;
			const struct expected *want = menu ? &cases[index].menu : &cases[index].play;

			memset(&pad, 0, sizeof(pad));
			map(cases[index].bit, &pad, menu);
			got.analog = analog_mask(&pad);
			got.buttons = pad.wButtons;
			CHECK(got.analog == want->analog && got.buttons == want->buttons, "%s %s (%s): got %s, wanted %s", scheme,
				cases[index].name, menu ? "menu" : "play", describe(&got), describe(want));
			if (!menu)
			{
				*reached |= got.analog;
				*reached |= got.buttons << 8;
			}
		}
	}
}

static WlRawRemote remote_state(int extension)
{
	WlRawRemote remote;

	memset(&remote, 0, sizeof(remote));
	remote.connected = TRUE;
	remote.extension = (uint8_t)extension;
	return remote;
}

static void map_classic_buttons(unsigned long long bits, XINPUT_GAMEPAD *pad, BOOL menu)
{
	WlRawRemote remote = remote_state(WL_EXT_CLASSIC);

	remote.buttons = (uint32_t)bits;
	wii_map_classic(&remote, pad, menu);
}

static void map_remote_buttons(unsigned long long bits, XINPUT_GAMEPAD *pad, BOOL menu)
{
	WlRawRemote remote = remote_state(WL_EXT_NUNCHUK);

	remote.buttons = (uint32_t)bits;
	wii_map_remote(&remote, pad, menu);
}

static void map_gamecube_buttons(unsigned long long bits, XINPUT_GAMEPAD *pad, BOOL menu)
{
	WlRawPad gamecube;

	memset(&gamecube, 0, sizeof(gamecube));
	gamecube.connected = TRUE;
	gamecube.buttons = (uint16_t)bits;
	wii_map_gamecube(&gamecube, pad, menu);
}

static const struct button_case classic_cases[] = {
	/* name, bit, in play, in the menus */
	{"b", WL_CC_B, {A, 0}, {A, 0}},
	{"a", WL_CC_A, {B, 0}, {B, 0}},
	{"y", WL_CC_Y, {X, 0}, {X, 0}},
	{"x", WL_CC_X, {Y, 0}, {Y, 0}},
	{"ZL", WL_CC_ZL, {BLACK, 0}, {BLACK, 0}},
	{"ZR", WL_CC_ZR, {WHITE, 0}, {WHITE, 0}},
	{"L", WL_CC_L, {LT, 0}, {LT, 0}},
	{"R", WL_CC_R, {RT, 0}, {RT, 0}},
	{"up", WL_CC_UP, {0, UP | RCLICK}, {0, UP}},
	{"down", WL_CC_DOWN, {0, DOWN | LCLICK}, {0, DOWN}},
	{"left", WL_CC_LEFT, {0, LEFT}, {0, LEFT}},
	{"right", WL_CC_RIGHT, {0, RIGHT}, {0, RIGHT}},
	{"+", WL_CC_PLUS, {0, START}, {0, START}},
	{"-", WL_CC_MINUS, {0, BACK}, {0, BACK}},
	{"HOME", WL_CC_HOME, {0, 0}, {0, 0}},
};

static const struct button_case remote_cases[] = {
	{"A", WL_WM_A, {A, 0}, {A, 0}},
	{"B", WL_WM_B, {RT, 0}, {RT | B, 0}},
	{"1", WL_WM_1, {B, 0}, {B, 0}},
	{"2", WL_WM_2, {0, RCLICK}, {0, RCLICK}},
	{"Z", WL_NC_Z, {LT, 0}, {LT, 0}},
	{"C", WL_NC_C, {0, LCLICK}, {0, LCLICK}},
	{"up", WL_WM_UP, {Y, UP}, {0, UP}},
	{"down", WL_WM_DOWN, {X, DOWN}, {0, DOWN}},
	{"left", WL_WM_LEFT, {BLACK, LEFT}, {0, LEFT}},
	{"right", WL_WM_RIGHT, {WHITE, RIGHT}, {0, RIGHT}},
	{"+", WL_WM_PLUS, {0, START}, {0, START}},
	{"-", WL_WM_MINUS, {0, BACK}, {0, BACK}},
	{"HOME", WL_WM_HOME, {0, 0}, {0, 0}},
};

static const struct button_case gamecube_cases[] = {
	{"A", WL_PAD_A, {A, 0}, {A, 0}},
	{"B", WL_PAD_B, {B, 0}, {B, 0}},
	{"X", WL_PAD_X, {X, 0}, {X, 0}},
	{"Y", WL_PAD_Y, {Y, 0}, {Y, 0}},
	{"Z", WL_PAD_Z, {BLACK, 0}, {BLACK, 0}},
	{"L", WL_PAD_L, {LT, 0}, {LT, 0}},
	{"R", WL_PAD_R, {RT, 0}, {RT, 0}},
	{"up", WL_PAD_UP, {0, UP | RCLICK}, {0, UP}},
	{"down", WL_PAD_DOWN, {0, DOWN | LCLICK}, {0, DOWN}},
	{"left", WL_PAD_LEFT, {0, LEFT | BACK}, {0, LEFT}},
	{"right", WL_PAD_RIGHT, {WHITE, RIGHT}, {0, RIGHT}},
	{"Start", WL_PAD_START, {0, START}, {0, START}},
};

#define COUNT(table) ((int)(sizeof(table) / sizeof((table)[0])))

/* every Xbox control a scheme can press in play: the eight analogue buttons, and the
D-pad, Start, Back and the clicks (bits 8 up) */
static void check_reaches_everything(const char *scheme, unsigned reached)
{
	unsigned all = 0xFFu | (0xFFu << 8);

	CHECK((reached & all) == all, "%s cannot press every Xbox control in play (reaches %02x analogue, %02x buttons)",
		scheme, reached & 0xFF, (reached >> 8) & 0xFF);
}

/* ---------- triggers and sticks */

static void check_classic_triggers(void)
{
	WlRawRemote remote = remote_state(WL_EXT_CLASSIC);
	XINPUT_GAMEPAD pad;

	/* the original Classic Controller: the analogue reading goes through */
	memset(&pad, 0, sizeof(pad));
	remote.trigger[0] = 100;
	remote.trigger[1] = 255;
	wii_map_classic(&remote, &pad, FALSE);
	CHECK(pad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] == 100, "classic left trigger %d",
		pad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER]);
	CHECK(pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] == 255, "classic right trigger %d",
		pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]);

	/* the Pro: the digital button alone is the whole travel */
	memset(&pad, 0, sizeof(pad));
	remote.trigger[0] = 0;
	remote.trigger[1] = 0;
	remote.buttons = WL_CC_L;
	wii_map_classic(&remote, &pad, FALSE);
	CHECK(pad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] == 0xFF, "pro left trigger");
	CHECK(pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] == 0, "pro right trigger leaks");

	/* a half-pressed original trigger that then clicks fully */
	memset(&pad, 0, sizeof(pad));
	remote.trigger[1] = 120;
	remote.buttons = WL_CC_R;
	wii_map_classic(&remote, &pad, FALSE);
	CHECK(pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] == 0xFF, "digital click beats the analogue reading");
}

static void check_gamecube_triggers(void)
{
	WlRawPad gamecube;
	XINPUT_GAMEPAD pad;

	memset(&gamecube, 0, sizeof(gamecube));
	memset(&pad, 0, sizeof(pad));
	gamecube.trigger[0] = 77;
	gamecube.trigger[1] = 200;
	wii_map_gamecube(&gamecube, &pad, FALSE);
	CHECK(pad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] == 77, "gamecube left trigger");
	CHECK(pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] == 200, "gamecube right trigger");
	memset(&pad, 0, sizeof(pad));
	gamecube.buttons = WL_PAD_R;
	wii_map_gamecube(&gamecube, &pad, FALSE);
	CHECK(pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] == 0xFF, "gamecube R click");
}

static void check_sticks(void)
{
	WlRawRemote remote = remote_state(WL_EXT_CLASSIC);
	WlRawPad gamecube;
	XINPUT_GAMEPAD pad;

	/* y is up in WiiLibs and in the Xbox's */
	memset(&pad, 0, sizeof(pad));
	remote.stick[0][0] = 127;
	remote.stick[0][1] = -127;
	remote.stick[1][0] = -127;
	remote.stick[1][1] = 127;
	wii_map_classic(&remote, &pad, FALSE);
	CHECK(pad.sThumbLX == 32767 && pad.sThumbLY == -32767, "classic left stick %d %d", pad.sThumbLX, pad.sThumbLY);
	CHECK(pad.sThumbRX == -32767 && pad.sThumbRY == 32767, "classic right stick %d %d", pad.sThumbRX, pad.sThumbRY);

	memset(&gamecube, 0, sizeof(gamecube));
	memset(&pad, 0, sizeof(pad));
	gamecube.stick[0][0] = -64;
	gamecube.stick[0][1] = 32;
	gamecube.stick[1][0] = 10;
	gamecube.stick[1][1] = -127;
	wii_map_gamecube(&gamecube, &pad, FALSE);
	CHECK(pad.sThumbLX == -64 * 32767 / 127 && pad.sThumbLY == 32 * 32767 / 127, "gamecube stick %d %d", pad.sThumbLX,
		pad.sThumbLY);
	CHECK(pad.sThumbRX == 10 * 32767 / 127 && pad.sThumbRY == -32767, "gamecube C stick %d %d", pad.sThumbRX,
		pad.sThumbRY);

	/* the Nunchuk's stick moves; the Remote has no right stick, and without a Nunchuk no left one */
	remote = remote_state(WL_EXT_NUNCHUK);
	remote.stick[0][0] = 127;
	remote.stick[0][1] = 127;
	remote.stick[1][0] = 100;
	remote.stick[1][1] = 100;
	memset(&pad, 0, sizeof(pad));
	wii_map_remote(&remote, &pad, FALSE);
	CHECK(pad.sThumbLX == 32767 && pad.sThumbLY == 32767, "nunchuk stick");
	CHECK(pad.sThumbRX == 0 && pad.sThumbRY == 0, "the Remote has a right stick");
	remote.extension = WL_EXT_NONE;
	memset(&pad, 0, sizeof(pad));
	wii_map_remote(&remote, &pad, FALSE);
	CHECK(pad.sThumbLX == 0 && pad.sThumbLY == 0, "a bare Remote moves");
}

/* the buttons, sticks and triggers of one state do not alter each other */
static void check_combined(void)
{
	WlRawRemote remote = remote_state(WL_EXT_NUNCHUK);
	XINPUT_GAMEPAD pad;

	remote.buttons = WL_WM_B | WL_WM_A | WL_NC_Z | WL_NC_C;
	remote.stick[0][1] = 127;
	memset(&pad, 0, sizeof(pad));
	wii_map_remote(&remote, &pad, FALSE);
	CHECK(analog_mask(&pad) == (RT | A | LT) && pad.wButtons == LCLICK && pad.sThumbLY == 32767,
		"running forward, firing, throwing a grenade, jumping and crouching: %02x %02x", analog_mask(&pad),
		pad.wButtons);
}

/* ---------- the pointer */

static void check_pointer_look(void)
{
	float yaw, pitch;
	const float yaw_rate = 2.6f, pitch_rate = 1.4f;

	wii_map_pointer_look(0.0f, 0.0f, 0.1f, yaw_rate, pitch_rate, &yaw, &pitch);
	CHECK(yaw == 0.0f && pitch == 0.0f, "the middle turns");
	wii_map_pointer_look(WII_POINTER_DEAD_ZONE, -WII_POINTER_DEAD_ZONE, 0.1f, yaw_rate, pitch_rate, &yaw, &pitch);
	CHECK(yaw == 0.0f && pitch == 0.0f, "the box's edge turns");
	wii_map_pointer_look(0.34f, 0.2f, 0.1f, yaw_rate, pitch_rate, &yaw, &pitch);
	CHECK(yaw == 0.0f && pitch == 0.0f, "inside the box turns");

	/* at the right edge: turns right (yaw down) at the full rate */
	wii_map_pointer_look(1.0f, 0.0f, 0.1f, yaw_rate, pitch_rate, &yaw, &pitch);
	CHECK(fabsf(yaw + yaw_rate * 0.1f) < 1e-6f && pitch == 0.0f, "right edge: %f %f", yaw, pitch);
	wii_map_pointer_look(-1.0f, 0.0f, 0.1f, yaw_rate, pitch_rate, &yaw, &pitch);
	CHECK(fabsf(yaw - yaw_rate * 0.1f) < 1e-6f, "left edge: %f", yaw);
	/* the pointer's y grows downward: at the top the view looks up (pitch up) */
	wii_map_pointer_look(0.0f, -1.0f, 0.1f, yaw_rate, pitch_rate, &yaw, &pitch);
	CHECK(fabsf(pitch - pitch_rate * 0.1f) < 1e-6f && yaw == 0.0f, "top edge: %f %f", yaw, pitch);
	wii_map_pointer_look(0.0f, 1.0f, 0.1f, yaw_rate, pitch_rate, &yaw, &pitch);
	CHECK(fabsf(pitch + pitch_rate * 0.1f) < 1e-6f, "bottom edge: %f", pitch);
	/* nearer the edge turns faster, and the turn is the time's share */
	{
		float slow, fast, doubled;

		wii_map_pointer_look(0.6f, 0.0f, 0.1f, yaw_rate, pitch_rate, &slow, &pitch);
		wii_map_pointer_look(0.9f, 0.0f, 0.1f, yaw_rate, pitch_rate, &fast, &pitch);
		wii_map_pointer_look(0.9f, 0.0f, 0.2f, yaw_rate, pitch_rate, &doubled, &pitch);
		CHECK(fast < slow && slow < 0.0f, "faster near the edge: %f %f", slow, fast);
		CHECK(fabsf(doubled - 2.0f * fast) < 1e-6f, "turns over twice the time");
	}
}

/* the pointer lost: the last position carries on briefly, then the turn ends */
static void check_pointer_loss(void)
{
	WlPointer pointer;
	WlRawRemote remote = remote_state(WL_EXT_NUNCHUK);
	float yaw, pitch;

	memset(&pointer, 0, sizeof(pointer));
	remote.ir_valid = TRUE;
	remote.ir_x = 640;
	remote.ir_y = 240;
	CHECK(wl_pointer_update(&pointer, &remote, 1000000, WII_POINTER_HOLD_US), "pointer tracked");
	remote.ir_valid = FALSE;
	CHECK(wl_pointer_update(&pointer, &remote, 1000000 + WII_POINTER_HOLD_US / 2, WII_POINTER_HOLD_US),
		"a short loss is held");
	wii_map_pointer_look(pointer.x, pointer.y, 0.016f, 2.6f, 1.4f, &yaw, &pitch);
	CHECK(yaw < 0.0f, "the held position still turns");
	CHECK(!wl_pointer_update(&pointer, &remote, 1000000 + 2 * WII_POINTER_HOLD_US, WII_POINTER_HOLD_US),
		"a long loss ends the tracking");
}

/* the pointer's play radius: a hand's tremor near the box's edge, which turns the view by fits and starts
without it, turns it steadily (or not at all) with it, and a deliberate move is followed at once */
static void check_pointer_steadiness(void)
{
	/* (the readings, 640 across: the box's edge is at 320 + 0.35 * 320 = 432) */
	static const int tremor[] = {434, 437, 433, 436, 435, 438, 433, 436};
	WlRawRemote remote = remote_state(WL_EXT_NUNCHUK);
	WlPointer steady;
	WlPointer loose;
	float turn_steady_min = 1e9f, turn_steady_max = -1e9f, turn_loose_min = 1e9f, turn_loose_max = -1e9f;
	float yaw, pitch;
	int i;

	CHECK(wii_pointer_steadiness(0.03) == 0.03f && wii_pointer_steadiness(0.0) == 0.0f, "the setting as written");
	CHECK(wii_pointer_steadiness(-1.0) == WII_POINTER_STEADINESS_DEFAULT, "a negative setting is the default");
	CHECK(wii_pointer_steadiness(0.0 / 0.0) == WII_POINTER_STEADINESS_DEFAULT, "a setting that is no number is the default");
	CHECK(wii_pointer_steadiness(5.0) == WII_POINTER_STEADINESS_MAX, "a large setting is the most");

	memset(&steady, 0, sizeof(steady));
	memset(&loose, 0, sizeof(loose));
	remote.ir_valid = TRUE;
	remote.ir_y = 240;
	for (i = 0; i < COUNT(tremor); i++)
	{
		remote.ir_x = (short)tremor[i];
		wl_pointer_update_steady(&steady, &remote, 1000000 + (uint64_t)i * 33000, WII_POINTER_HOLD_US, WII_POINTER_STEADINESS_DEFAULT);
		wl_pointer_update_steady(&loose, &remote, 1000000 + (uint64_t)i * 33000, WII_POINTER_HOLD_US, 0.0f);
		wii_map_pointer_look(steady.x, steady.y, 0.033f, 2.6f, 1.4f, &yaw, &pitch);
		turn_steady_min = yaw < turn_steady_min ? yaw : turn_steady_min;
		turn_steady_max = yaw > turn_steady_max ? yaw : turn_steady_max;
		wii_map_pointer_look(loose.x, loose.y, 0.033f, 2.6f, 1.4f, &yaw, &pitch);
		turn_loose_min = yaw < turn_loose_min ? yaw : turn_loose_min;
		turn_loose_max = yaw > turn_loose_max ? yaw : turn_loose_max;
	}
	CHECK(turn_steady_max == turn_steady_min, "tremor at the box's edge turns a steady %f..%f", turn_steady_min,
		turn_steady_max);
	CHECK(turn_loose_max != turn_loose_min, "without the radius the same tremor varies the turn");

	/* a deliberate move: the pointer is where the reading is, in the same read */
	remote.ir_x = 500;
	wl_pointer_update_steady(&steady, &remote, 2000000, WII_POINTER_HOLD_US, WII_POINTER_STEADINESS_DEFAULT);
	CHECK(fabsf(steady.x - (500.0f - 320.0f) / 320.0f) < 1e-6f, "a move beyond the radius is followed at once: %f", steady.x);
}

/* the rumble's duty cycle: the fraction of ticks the motor is on, the
switches a second, and that zero stops it at once */

struct rumble_run
{
	unsigned on_ticks;
	unsigned switches;
	unsigned most_switches_in_a_second;
	unsigned ticks;
};

/* `seconds` of ticks every tick_us at one strength, counting from a motor
that is off */
static struct rumble_run run_rumble(unsigned strength, unsigned tick_us, unsigned seconds)
{
	struct wii_rumble rumble;
	struct rumble_run run;
	uint64_t now = 5000000;
	uint64_t end = now + (uint64_t)seconds * 1000000;
	/* the times of the last switches, to count those in a second */
	uint64_t switch_times[4096];
	unsigned recent_first = 0;
	BOOL previous = FALSE;

	memset(&rumble, 0, sizeof(rumble));
	memset(&run, 0, sizeof(run));
	for (; now < end; now += tick_us)
	{
		BOOL on = wii_rumble_duty(&rumble, strength, now);

		run.ticks++;
		if (on)
			run.on_ticks++;
		if (on != previous)
		{
			unsigned count;

			previous = on;
			switch_times[run.switches % 4096] = now;
			run.switches++;
			while (now - switch_times[recent_first % 4096] >= 1000000)
				recent_first++;
			count = run.switches - recent_first;
			if (count > run.most_switches_in_a_second)
				run.most_switches_in_a_second = count;
		}
	}
	return run;
}

static void check_rumble(void)
{
	static const unsigned strengths[] = { 1000, 6554, 16384, 32768, 40000, 49152, 58982, 64000, 65535 };
	unsigned index;
	struct wii_rumble rumble;
	struct rumble_run run;

	CHECK(wii_rumble_strength(0, 0) == 0, "no strength");
	CHECK(wii_rumble_strength(30000, 5000) == 30000, "the left motor is the stronger");
	CHECK(wii_rumble_strength(5000, 30000) == 30000, "the right motor is the stronger");
	CHECK(wii_rumble_strength(70000, 0) == WII_RUMBLE_FULL, "clamped to the full strength");

	/* on for a fraction of the ticks equal to the strength: at the frame rate
	of 60 a second, and at the game's 30 */
	for (index = 0; index < COUNT(strengths); index++)
	{
		unsigned tick_us;

		for (tick_us = 16667; tick_us <= 33333; tick_us += 16666)
		{
			double wanted = (double)strengths[index] / WII_RUMBLE_FULL;
			double got;

			run = run_rumble(strengths[index], tick_us, 20);
			got = (double)run.on_ticks / run.ticks;
			CHECK(fabs(got - wanted) < 0.01, "strength %u at %u us: on %.4f of the ticks, wanted %.4f",
				strengths[index], tick_us, got, wanted);
			CHECK(run.most_switches_in_a_second <= 100, "strength %u at %u us: %u switches in a second",
				strengths[index], tick_us, run.most_switches_in_a_second);
		}
	}

	/* full is on for every tick, none is off for every tick */
	run = run_rumble(WII_RUMBLE_FULL, 16667, 2);
	CHECK(run.on_ticks == run.ticks && run.switches == 1, "full strength: on from the first tick, never switched again");
	run = run_rumble(0, 16667, 2);
	CHECK(run.on_ticks == 0 && run.switches == 0, "no strength: never on");

	/* half a strength every tick of 60 a second switches every tick, 60 a second */
	run = run_rumble(32768, 16667, 10);
	CHECK(run.most_switches_in_a_second >= 55 && run.most_switches_in_a_second <= 100,
		"half strength at 60 ticks a second: %u switches in a second", run.most_switches_in_a_second);

	/* however fast the ticks come (a millisecond apart, a thousand a second),
	not more than a hundred switches a second, and still about the fraction */
	for (index = 0; index < COUNT(strengths); index++)
	{
		double wanted = (double)strengths[index] / WII_RUMBLE_FULL;
		double got;

		run = run_rumble(strengths[index], 1000, 10);
		got = (double)run.on_ticks / run.ticks;
		CHECK(run.most_switches_in_a_second <= 100, "strength %u every millisecond: %u switches in a second",
			strengths[index], run.most_switches_in_a_second);
		CHECK(fabs(got - wanted) < 0.12, "strength %u every millisecond: on %.3f, wanted %.3f", strengths[index],
			got, wanted);
	}

	/* zero stops the motor at once, 1 ms after it switched on, and the carry
	is forgotten: the next strength starts afresh */
	memset(&rumble, 0, sizeof(rumble));
	CHECK(wii_rumble_duty(&rumble, WII_RUMBLE_FULL, 9000000), "full strength turns it on");
	CHECK(!wii_rumble_duty(&rumble, 0, 9001000), "zero turns it off within the switch interval");
	CHECK(rumble.carry == 0, "the carry is forgotten");
	CHECK(!wii_rumble_duty(&rumble, 0, 9002000), "and it stays off");
	/* a switch wanted within the interval of the last waits for it */
	CHECK(!wii_rumble_duty(&rumble, WII_RUMBLE_FULL, 9003000), "not switched on again within the interval");
	CHECK(wii_rumble_duty(&rumble, WII_RUMBLE_FULL, 9001000 + WII_RUMBLE_SWITCH_US), "switched on once it has passed");
}

int main(void)
{
	unsigned reached;

	reached = 0;
	check_buttons("classic", classic_cases, COUNT(classic_cases), map_classic_buttons, &reached);
	check_reaches_everything("classic", reached);
	reached = 0;
	check_buttons("remote+nunchuk", remote_cases, COUNT(remote_cases), map_remote_buttons, &reached);
	check_reaches_everything("remote+nunchuk", reached);
	reached = 0;
	check_buttons("gamecube", gamecube_cases, COUNT(gamecube_cases), map_gamecube_buttons, &reached);
	check_reaches_everything("gamecube", reached);
	check_classic_triggers();
	check_gamecube_triggers();
	check_sticks();
	check_combined();
	check_pointer_look();
	check_pointer_loss();
	check_pointer_steadiness();
	check_rumble();
	printf("wii_input_check: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
