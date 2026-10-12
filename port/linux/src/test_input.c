/*
TEST_INPUT.C

debug.test_input "bot:<seed>": a scripted player for the automated tests
(port/linux/game/network_test.c), different for each seed: it walks and
strafes in circles, turns, fires every few seconds, jumps now and then and
throws a grenade every seven seconds; "look:<seed>" stands still, only
turning and looking up and down (where remote players aim and whether they
stand). Each port's controller code adds it to controller 1
(port/linux/src/xinput_sdl.c, port/wii/src/xinput_wii.c).

"script:<file>" plays controller 1 from a file of steps, one to a line, as
a person would: through the menus, into a level and out again (the file is
read from the data folder, unless its path is absolute). The game tells it
what the menus show and how far a game has run each frame
(test_input_observe, from port/linux/game/network_test.c), and it logs each
screen and focused widget as they change, and each step as it begins. A
step that waits gives up after its seconds (120 unless given), which stops
the script with a line in the log saying where it was.

	# a comment
	wait <seconds>                  nothing pressed for that long
	screen <text> [<seconds>]       until the screen's tag name holds the text
	focus <text> [<seconds>]        until the focused widget's tag name holds it
	play [<seconds>]                until a game runs with no menu shown
	ticks <count>                   until the game has run that many more ticks
	checkpoint [<seconds>]          until the game saves a checkpoint
	afoot [<seconds>]               until the player is in a game, riding nothing
	press <buttons> [<count>]       press and release, count times
	select <text> [up|down|left|right] [<count>]
	                                the D-pad (down) until the focused widget's
	                                tag name holds the text, at most count (16)
	hold <buttons> <seconds> [afoot]
	                                held for that long, or until the player
	                                rides nothing
	stick <lx> <ly> <rx> <ry> <seconds>
	                                the sticks, each -1 to 1, for that long
	walk <x> <y> [<seconds>]        the left stick toward that point of the
	                                level until within 0.1 of it (a world
	                                unit, about 3 metres), without turning
	bot <seconds> [checkpoint]      the bot's play (bot:1) for that long, or
	                                until the game saves a checkpoint
	objective [<seconds>]           the level played to its end, as a person
	                                would: where its scripts wait for him,
	                                fighting on the way (level_run.c, which
	                                lists its aids); until the next level
	                                begins, or the game ends (3600 seconds of
	                                the game's time unless given)
	destroy <name> ...              objects the objective then destroys where
	                                it finds them, by their names in the level
	                                (d40's manifolds' detectors), up to 7 a line
	repeat <count> ... end          the steps between, count times over
	log <text>                      a line in the log, with where the player
	                                is and faces in a game
	quit                            the game quits, as its menus' Quit does

The other controllers (split screen, co-op; they cannot be told from a
person's second, third and fourth controller by the game):

	player <n>                      the steps that follow (press, select, hold,
	                                stick, walk, bot <seconds>, objective)
	                                drive controller n, 1 to 4, until the next
	                                player line (the script starts with 1)
	<n>: <step>                     the one step on controller n: "2: press a"
	connect <n> [real]              controller n, 2 to 4, appears to the game
	                                (the Xbox's insertion, XGetDeviceChanges
	                                and XInputOpen), with nothing pressed unless
	                                a step presses it; real: as the machine
	                                has it
	disconnect <n>                  controller n is gone, even if one is there
	bot <n> <seed>                  controller n plays as the bot of that seed
	                                (as bot:<seed>, on the game's ticks in a
	                                game) alongside the steps, until
	bot <n> off                     (it is meant for a game: the bot presses
	                                A and the triggers in the menus too)

The steps wait for and read the first player's screen (the menus of the
other players are not observed): they are written in the order a person
would do them, one controller at a time.

The Wii's (they change the controllers' raw input before the port reads it,
the same way the console's would; port/wii/README.md, "The controllers"):

	home                            HOME pressed and let go, on any Remote
	remote <port> off|on|real       Wii Remote 0 to 3 gone, there (with
	                                nothing pressed, if none is), as reported
	extension <port> none|nunchuk|classic|real
	                                what is plugged into the Remote
	battery <port> <percent>|real   the Remote's battery
	event reset|power               the console's reset or power button as
	                                far as the watcher thread: logged, and
	                                nothing leaves

While the HOME menu is up the script's D-pad up and down, a and b steer it
(press down, press a): the game sees none of the script's buttons then.

Buttons are a, b, x, y, black, white, lt, rt, up, down, left, right, start,
back, lthumb and rthumb, joined with + to press several at once. The text a
screen or focus waits for is matched without regard to case. Times are
those of the clock the port passes in, counting at most a quarter of a
second between two reads of the controller (a level's loading counts as
little), but hold, stick and bot begun in a game count the game's ticks,
30 to the second, so that a slower machine moves as far; a press lasts four
of the game's reads of the controller and at least 100 milliseconds, as
does the pause after it, so that slow frames see it.
*/

#include "test_input.h"
#include "port_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform's (sdl_platform.c, port/wii/src/wii_platform.c) */
void platform_request_quit(void);
/* level_run.c's (port/linux/game): the objective */
void level_run_begin(void);
void level_run_stop(void);
void level_run_destroy(const char *name);
int level_run_controller(float *sticks, unsigned long *buttons);

/* level_run.c's buttons */
enum
{
	_level_run_button_fire = 1 << 0,
	_level_run_button_action = 1 << 1,
	_level_run_button_jump = 1 << 2,
	_level_run_button_back = 1 << 3,
	_level_run_button_zoom = 1 << 4,
	_level_run_button_grenade = 1 << 5,
};

enum
{
	_test_input_none,
	_test_input_bot,
	_test_input_look,
	_test_input_script,
};

static int test_input_checked;
static int test_input_mode = _test_input_none;
static int test_input_seed;
static char test_input_script_path[256];
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
	{
		test_input_mode = _test_input_bot;
		test_input_seed = atoi(setting + 4);
	}
	else if (!strcmp(setting, "bot"))
	{
		test_input_mode = _test_input_bot;
	}
	else if (!strncmp(setting, "look:", 5))
	{
		test_input_mode = _test_input_look;
		test_input_seed = atoi(setting + 5);
	}
	else if (!strncmp(setting, "script:", 7) && setting[7])
	{
		test_input_mode = _test_input_script;
		snprintf(test_input_script_path, sizeof(test_input_script_path), "%s", setting + 7);
	}
	else if (setting[0])
	{
		platform_log("test_input: debug.test_input \"%s\" is not bot:, look: or script:", setting);
	}
}

BOOL test_input_active(void)
{
	test_input_read_settings();
	return test_input_mode != _test_input_none;
}

BOOL test_input_scripted(void)
{
	test_input_read_settings();
	return test_input_mode == _test_input_script;
}

void test_input_hold_action(int hold)
{
	if (hold && !test_input_holding_action)
		test_input_holding_action_since = 0;
	test_input_holding_action = hold;
}

/* the game's tick in a game, else -1 (port/linux/game/network_test.c sets
it from the main loop): for debug.fixed_ticks */
long network_test_game_tick = -1;

/* the bot's (or with looking, look:'s) buttons and sticks at that time */
static void test_input_bot(XINPUT_GAMEPAD *pad, unsigned long long milliseconds, int seed, int looking)
{
	double t = (double)milliseconds / 1000.0 + seed * 1.7;

	if (looking)
	{
		pad->sThumbRX = (SHORT)(sin(t * 0.5) * 14000.0);
		pad->sThumbRY = (SHORT)(sin(t * 0.3) * 32000.0);
		return;
	}
	pad->sThumbLY = (SHORT)(sin(t * 0.9) * 32000.0);
	pad->sThumbLX = (SHORT)(cos(t * 0.6 + seed) * 20000.0);
	pad->sThumbRX = (SHORT)(sin(t * 0.4) * 14000.0);
	if (fmod(t, 3.0) < 0.3)
		pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 255;
	if (fmod(t, 5.0) < 0.1)
		pad->bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
	if (fmod(t, 7.0) < 0.2)
		pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = 255;
}

/* ---------- the script */

#define SCRIPT_CONTROLLERS 4
#define SCRIPT_MAXIMUM_STEPS 1024
#define SCRIPT_MAXIMUM_NESTING 8
#define SCRIPT_TEXT_SIZE 128
#define SCRIPT_OBSERVED_SIZE 256
#define SCRIPT_DEFAULT_TIMEOUT 120.0
#define SCRIPT_PRESS_POLLS 4
#define SCRIPT_PRESS_MILLISECONDS 100
#define SCRIPT_SELECT_MAXIMUM 16
#define SCRIPT_OBJECTIVE_TIMEOUT 3600.0
/* (seconds of the script's clock out of a game, the objective's level
having ended in the menus: the campaign's end) */
#define SCRIPT_OBJECTIVE_MENU_SECONDS 60.0

enum
{
	_step_wait,
	_step_screen,
	_step_focus,
	_step_play,
	_step_ticks,
	_step_checkpoint,
	_step_afoot,
	_step_press,
	_step_select,
	_step_hold,
	_step_stick,
	_step_walk,
	_step_bot,
	_step_objective,
	_step_destroy,
	_step_log,
	_step_quit,
	/* the other controllers' (connect, disconnect, bot <n>) */
	_step_controller,
	_step_bot_player,
	/* the Wii's verbs (script_parse_wii) */
	_step_wii,
	/* (only while the file is read: repeat <count> ... end) */
	_step_repeat,
	_step_end,
};

struct script_step
{
	int kind;
	int line;
	/* the line as written, for the log */
	char text[SCRIPT_TEXT_SIZE];
	/* the text waited for, or logged */
	char argument[SCRIPT_TEXT_SIZE];
	/* seconds, a count of ticks or presses; the sticks */
	double number;
	double sticks[4];
	unsigned short digital;
	unsigned char analog;
	/* the controller the step drives, 0 to 3 */
	unsigned char player;
};

static struct
{
	int loaded;
	int count;
	/* (SCRIPT_MAXIMUM_STEPS of them, allocated with the script) */
	struct script_step *steps;
	/* where it is: the step, and what it began at */
	int current;
	int started;
	unsigned long long started_clock;
	/* the steps' clock: the time between reads of the controller, each at
	most a quarter of a second, so that a level's loading (no reads for a
	while) does not run out a wait before the game has shown what it loaded */
	unsigned long long clock, last_read;
	long started_tick;
	long started_checkpoints;
	/* (the objective: when the game was last seen running) */
	unsigned long long in_game_clock;
	/* a press: whether held, the reads and time since it changed, and the
	presses made */
	int pressed;
	int phase_polls;
	unsigned long long phase_milliseconds;
	int presses;
	int finished;
	/* what the game showed at its last frame (test_input_observe) */
	char screen[SCRIPT_OBSERVED_SIZE];
	char focus[SCRIPT_OBSERVED_SIZE];
	long tick;
	long checkpoints;
	int observed;
	/* the first player's unit (test_input.h), if placed */
	int placed;
	float place[6];
	/* the controllers (connect, disconnect, bot <n>): 0 as the machine has
	them, 1 gone, 2 there; the bots' seeds plus one (0 for none) */
	int connected[SCRIPT_CONTROLLERS];
	int bots[SCRIPT_CONTROLLERS];
	/* what the current step presses, and on which controller: made on the
	first controller's read, given to that controller's */
	XINPUT_GAMEPAD output;
	int output_player;
} script;

static const struct
{
	const char *name;
	unsigned short digital;
	int analog;
} script_buttons[] =
{
	{ "a", 0, XINPUT_GAMEPAD_A },
	{ "b", 0, XINPUT_GAMEPAD_B },
	{ "x", 0, XINPUT_GAMEPAD_X },
	{ "y", 0, XINPUT_GAMEPAD_Y },
	{ "black", 0, XINPUT_GAMEPAD_BLACK },
	{ "white", 0, XINPUT_GAMEPAD_WHITE },
	{ "lt", 0, XINPUT_GAMEPAD_LEFT_TRIGGER },
	{ "rt", 0, XINPUT_GAMEPAD_RIGHT_TRIGGER },
	{ "up", XINPUT_GAMEPAD_DPAD_UP, -1 },
	{ "down", XINPUT_GAMEPAD_DPAD_DOWN, -1 },
	{ "left", XINPUT_GAMEPAD_DPAD_LEFT, -1 },
	{ "right", XINPUT_GAMEPAD_DPAD_RIGHT, -1 },
	{ "start", XINPUT_GAMEPAD_START, -1 },
	{ "back", XINPUT_GAMEPAD_BACK, -1 },
	{ "lthumb", XINPUT_GAMEPAD_LEFT_THUMB, -1 },
	{ "rthumb", XINPUT_GAMEPAD_RIGHT_THUMB, -1 },
};

static char script_lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* whether text holds part, in any case */
static int script_contains(const char *text, const char *part)
{
	size_t length = strlen(part), at, i;

	if (strlen(text) < length)
		return 0;
	for (at = 0; at + length <= strlen(text); at++)
	{
		for (i = 0; i < length && script_lower(text[at + i]) == script_lower(part[i]); i++)
			;
		if (i == length)
			return 1;
	}
	return 0;
}

/* the buttons of "a+start" and the like; 0 for a name it does not know */
static int script_parse_buttons(const char *text, struct script_step *step)
{
	char name[32];
	const char *end;
	size_t length, index;

	while (*text)
	{
		end = strchr(text, '+');
		length = end ? (size_t)(end - text) : strlen(text);
		if (!length || length >= sizeof(name))
			return 0;
		memcpy(name, text, length);
		name[length] = 0;
		for (index = 0; index < sizeof(script_buttons) / sizeof(script_buttons[0]); index++)
		{
			if (!strcmp(script_buttons[index].name, name))
				break;
		}
		if (index == sizeof(script_buttons) / sizeof(script_buttons[0]))
			return 0;
		step->digital |= script_buttons[index].digital;
		if (script_buttons[index].analog >= 0)
			step->analog |= (unsigned char)(1 << script_buttons[index].analog);
		text += length;
		if (*text == '+')
			text++;
	}
	return 1;
}

/* ---------- the Wii's verbs

home, remote, extension, battery and event: parsed into a step, run at once
(script_wii_step), and kept as the state test_input_remote_override and
test_input_home_pressed give the port. They are read on every platform so
that one script serves all, but only the Wii's input uses them. */

#define WII_PORTS 4
#define WII_HOME_PRESS_READS 8

enum
{
	_wii_home,
	_wii_remote,
	_wii_extension,
	_wii_battery,
	_wii_event
};

static struct
{
	/* 0 as reported, else the override plus one */
	signed char connected[WII_PORTS];
	signed char extension[WII_PORTS];
	short battery[WII_PORTS];
	int home_reads;
} wii_script;

/* the step's own fields: sticks[0] the verb, [1] the port, [2] the value (-1 as reported) */
static int script_parse_wii(char **words, int count, struct script_step *step)
{
	int port = 0;
	const char *value = count >= 3 ? words[2] : "";

	if (!strcmp(words[0], "home") && count == 1)
	{
		step->sticks[0] = _wii_home;
		return 1;
	}
	if (!strcmp(words[0], "event") && count == 2)
	{
		if (strcmp(words[1], "reset") && strcmp(words[1], "power"))
			return 0;
		step->sticks[0] = _wii_event;
		step->sticks[2] = !strcmp(words[1], "power");
		return 1;
	}
	if (count != 3 || (words[1][0] < '0' || words[1][0] > '3' || words[1][1]))
		return 0;
	port = words[1][0] - '0';
	step->sticks[1] = port;
	step->sticks[2] = -1;
	if (!strcmp(words[0], "remote"))
	{
		step->sticks[0] = _wii_remote;
		if (!strcmp(value, "off"))
			step->sticks[2] = 0;
		else if (!strcmp(value, "on"))
			step->sticks[2] = 1;
		else if (strcmp(value, "real"))
			return 0;
	}
	else if (!strcmp(words[0], "extension"))
	{
		step->sticks[0] = _wii_extension;
		if (!strcmp(value, "none"))
			step->sticks[2] = 0;
		else if (!strcmp(value, "nunchuk"))
			step->sticks[2] = 1;
		else if (!strcmp(value, "classic"))
			step->sticks[2] = 2;
		else if (strcmp(value, "real"))
			return 0;
	}
	else if (!strcmp(words[0], "battery"))
	{
		step->sticks[0] = _wii_battery;
		if (strcmp(value, "real"))
		{
			char *end;
			long percent = strtol(value, &end, 10);

			if (*end || percent < 0 || percent > 100)
				return 0;
			step->sticks[2] = (double)percent;
		}
	}
	else
	{
		return 0;
	}
	return 1;
}

#ifdef HALO_WII
/* the reset or power button as the watcher thread sees it, without leaving
(port/wii/src/wii_system.c) */
int wii_system_simulate(int power);
#endif

static void script_wii_step(struct script_step const *step)
{
	int verb = (int)step->sticks[0], port = (int)step->sticks[1], value = (int)step->sticks[2];

	switch (verb)
	{
	case _wii_home:
		wii_script.home_reads = WII_HOME_PRESS_READS;
		break;
	case _wii_remote:
		wii_script.connected[port] = (signed char)(value < 0 ? 0 : value + 1);
		break;
	case _wii_extension:
		wii_script.extension[port] = (signed char)(value < 0 ? 0 : value + 1);
		break;
	case _wii_battery:
		wii_script.battery[port] = (short)(value < 0 ? 0 : value + 1);
		break;
	case _wii_event:
#ifdef HALO_WII
		if (wii_system_simulate(value))
			platform_log("test_input: the watcher thread is not running: no %s button", value ? "power" : "reset");
#endif
		break;
	}
#ifndef HALO_WII
	platform_log("test_input: %s: the Wii's verbs change only the Wii's controllers", step->text);
#endif
}

BOOL test_input_remote_override(int port, struct test_input_remote *remote)
{
	if (port < 0 || port >= WII_PORTS)
		return FALSE;
	remote->connected = wii_script.connected[port] - 1;
	remote->extension = wii_script.extension[port] - 1;
	remote->battery = wii_script.battery[port] - 1;
	return wii_script.connected[port] || wii_script.extension[port] || wii_script.battery[port];
}

BOOL test_input_home_pressed(void)
{
	if (wii_script.home_reads <= 0)
		return FALSE;
	wii_script.home_reads--;
	return TRUE;
}

/* the controller the steps are for, as the last "player <n>" line said */
static int script_parse_player;

/* the controller of "2" in "connect 2", "player 2" and the like, from
minimum to 4; 0 for any other text */
static int script_parse_controller(const char *text, int minimum)
{
	if (text[0] < '0' + minimum || text[0] > '0' + SCRIPT_CONTROLLERS || text[1])
		return 0;
	return text[0] - '0';
}

/* one line into a step: 0 for a blank line or a comment, -1 for a fault */
static int script_parse_line(char *line, int number, struct script_step *step)
{
	char *words[8];
	char *cursor;
	int count = 0, index;
	int player = script_parse_player;

	cursor = strchr(line, '#');
	if (cursor)
		*cursor = 0;
	for (cursor = line + strlen(line); cursor > line && (unsigned char)cursor[-1] <= ' '; cursor--)
		cursor[-1] = 0;
	for (cursor = line; *cursor && (unsigned char)*cursor <= ' '; cursor++)
		;
	if (!*cursor)
		return 0;
	memset(step, 0, sizeof(*step));
	step->line = number;
	/* ("2: press a": the one step is the controller's) */
	if (cursor[0] >= '1' && cursor[0] <= '0' + SCRIPT_CONTROLLERS && cursor[1] == ':')
	{
		player = cursor[0] - '1';
		for (cursor += 2; *cursor && (unsigned char)*cursor <= ' '; cursor++)
			;
		if (!*cursor)
			return -1;
	}
	step->player = (unsigned char)player;
	snprintf(step->text, sizeof(step->text), "%s", cursor);
	/* (log keeps the rest of its line as it is) */
	if (!strncmp(cursor, "log", 3) && (unsigned char)cursor[3] <= ' ')
	{
		step->kind = _step_log;
		for (cursor += 3; *cursor && (unsigned char)*cursor <= ' '; cursor++)
			;
		snprintf(step->argument, sizeof(step->argument), "%s", cursor);
		return 1;
	}
	while (*cursor && count < 8)
	{
		words[count++] = cursor;
		while (*cursor && (unsigned char)*cursor > ' ')
			cursor++;
		while (*cursor && (unsigned char)*cursor <= ' ')
			*cursor++ = 0;
	}
	step->number = SCRIPT_DEFAULT_TIMEOUT;
	if (!strcmp(words[0], "wait") && count == 2)
	{
		step->kind = _step_wait;
		step->number = atof(words[1]);
	}
	else if ((!strcmp(words[0], "screen") || !strcmp(words[0], "focus")) && (count == 2 || count == 3))
	{
		step->kind = words[0][0] == 's' ? _step_screen : _step_focus;
		snprintf(step->argument, sizeof(step->argument), "%s", words[1]);
		if (count == 3)
			step->number = atof(words[2]);
	}
	else if ((!strcmp(words[0], "play") || !strcmp(words[0], "checkpoint")) && count <= 2)
	{
		step->kind = words[0][0] == 'p' ? _step_play : _step_checkpoint;
		if (count == 2)
			step->number = atof(words[1]);
	}
	else if (!strcmp(words[0], "ticks") && count == 2)
	{
		step->kind = _step_ticks;
		step->number = atof(words[1]);
	}
	else if (!strcmp(words[0], "press") && (count == 2 || count == 3) && script_parse_buttons(words[1], step))
	{
		step->kind = _step_press;
		step->number = count == 3 ? atof(words[2]) : 1.0;
	}
	else if (!strcmp(words[0], "select") && count >= 2 && count <= 4)
	{
		step->kind = _step_select;
		snprintf(step->argument, sizeof(step->argument), "%s", words[1]);
		step->number = SCRIPT_SELECT_MAXIMUM;
		if (!script_parse_buttons(count >= 3 ? words[2] : "down", step) || step->analog)
			return -1;
		if (count == 4)
			step->number = atof(words[3]);
	}
	else if (!strcmp(words[0], "hold") && (count == 3 || (count == 4 && !strcmp(words[3], "afoot"))) &&
		script_parse_buttons(words[1], step))
	{
		step->kind = _step_hold;
		step->number = atof(words[2]);
		if (count == 4)
			snprintf(step->argument, sizeof(step->argument), "%s", words[3]);
	}
	else if (!strcmp(words[0], "afoot") && count <= 2)
	{
		step->kind = _step_afoot;
		if (count == 2)
			step->number = atof(words[1]);
	}
	else if (!strcmp(words[0], "stick") && count == 6)
	{
		step->kind = _step_stick;
		for (index = 0; index < 4; index++)
			step->sticks[index] = atof(words[1 + index]);
		step->number = atof(words[5]);
	}
	else if (!strcmp(words[0], "walk") && (count == 3 || count == 4))
	{
		step->kind = _step_walk;
		step->sticks[0] = atof(words[1]);
		step->sticks[1] = atof(words[2]);
		if (count == 4)
			step->number = atof(words[3]);
	}
	else if (!strcmp(words[0], "player") && count == 2 && script_parse_controller(words[1], 1))
	{
		/* (no step: the lines that follow are the controller's) */
		script_parse_player = script_parse_controller(words[1], 1) - 1;
		return 0;
	}
	else if ((!strcmp(words[0], "connect") && (count == 2 || (count == 3 && !strcmp(words[2], "real")))) ||
		(!strcmp(words[0], "disconnect") && count == 2))
	{
		int controller = script_parse_controller(words[1], 2);

		if (!controller)
			return -1;
		step->kind = _step_controller;
		step->sticks[0] = controller - 1;
		step->sticks[1] = words[0][0] == 'd' ? 1 : count == 3 ? 0 : 2;
	}
	else if (!strcmp(words[0], "bot") && count == 3 && strcmp(words[2], "checkpoint"))
	{
		/* (bot <n> <seed>|off, not bot <seconds> checkpoint) */
		char *end;
		long seed = strtol(words[2], &end, 10);
		int controller = script_parse_controller(words[1], 1);

		if (!controller || (strcmp(words[2], "off") && (*end || seed < 0 || seed > 1000000)))
			return -1;
		step->kind = _step_bot_player;
		step->sticks[0] = controller - 1;
		step->sticks[1] = !strcmp(words[2], "off") ? 0 : (double)seed + 1;
	}
	else if (!strcmp(words[0], "bot") && (count == 2 || (count == 3 && !strcmp(words[2], "checkpoint"))))
	{
		step->kind = _step_bot;
		step->number = atof(words[1]);
		if (count == 3)
			snprintf(step->argument, sizeof(step->argument), "%s", words[2]);
	}
	else if (!strcmp(words[0], "destroy") && count >= 2)
	{
		/* (the names, a space between each) */
		step->kind = _step_destroy;
		for (index = 1; index < count; index++)
		{
			size_t used = strlen(step->argument);

			snprintf(step->argument + used, sizeof(step->argument) - used, "%s%s", index > 1 ? " " : "", words[index]);
		}
	}
	else if (!strcmp(words[0], "objective") && count <= 2)
	{
		step->kind = _step_objective;
		step->number = count == 2 ? atof(words[1]) : SCRIPT_OBJECTIVE_TIMEOUT;
	}
	else if (!strcmp(words[0], "repeat") && count == 2 && atoi(words[1]) >= 1)
	{
		step->kind = _step_repeat;
		step->number = atoi(words[1]);
	}
	else if (!strcmp(words[0], "end") && count == 1)
	{
		step->kind = _step_end;
	}
	else if (!strcmp(words[0], "quit") && count == 1)
	{
		step->kind = _step_quit;
	}
	else if (script_parse_wii(words, count, step))
	{
		step->kind = _step_wii;
	}
	else
	{
		return -1;
	}
	return 1;
}

static void script_load(void)
{
	char path[512];
	char line[256];
	FILE *file;
	int number = 0, result, depth = 0;
	int repeat_start[SCRIPT_MAXIMUM_NESTING], repeat_count[SCRIPT_MAXIMUM_NESTING];

	script.loaded = 1;
	script_parse_player = 0;
	/* (in the data folder, unless absolute: "/...", or a drive's "sd:/...") */
	if (test_input_script_path[0] == '/' || strchr(test_input_script_path, ':'))
		snprintf(path, sizeof(path), "%s", test_input_script_path);
	else
		snprintf(path, sizeof(path), "%s/%s", platform_data_root(), test_input_script_path);
	file = fopen(path, "r");
	if (!file)
	{
		platform_log("test_input: no script at %s", path);
		script.finished = 1;
		return;
	}
	script.steps = malloc(SCRIPT_MAXIMUM_STEPS * sizeof(*script.steps));
	while (script.steps && fgets(line, sizeof(line), file))
	{
		struct script_step *step = &script.steps[script.count];

		number++;
		if (script.count == SCRIPT_MAXIMUM_STEPS)
		{
			platform_log("test_input: %s: more than %d steps", path, SCRIPT_MAXIMUM_STEPS);
			script.count = 0;
			break;
		}
		result = script_parse_line(line, number, step);
		if (result < 0 || (result && step->kind == _step_repeat && depth == SCRIPT_MAXIMUM_NESTING) ||
			(result && step->kind == _step_end && !depth))
		{
			platform_log("test_input: %s:%d: not a step: %s", path, number, line);
			script.count = 0;
			break;
		}
		if (!result)
			continue;
		if (step->kind == _step_repeat)
		{
			/* (the steps to repeat are those that follow, until its end) */
			repeat_start[depth] = script.count;
			repeat_count[depth] = (int)step->number;
			depth++;
		}
		else if (step->kind == _step_end)
		{
			/* (the steps since its repeat, again and again) */
			int start = repeat_start[--depth], length = script.count - start, copy;

			if (script.count + (repeat_count[depth] - 1) * length > SCRIPT_MAXIMUM_STEPS)
			{
				platform_log("test_input: %s:%d: more than %d steps", path, number, SCRIPT_MAXIMUM_STEPS);
				script.count = 0;
				break;
			}
			for (copy = 1; copy < repeat_count[depth]; copy++)
			{
				memcpy(&script.steps[script.count], &script.steps[start], length * sizeof(*script.steps));
				script.count += length;
			}
		}
		else
		{
			script.count++;
		}
	}
	if (depth && script.count)
	{
		platform_log("test_input: %s: a repeat with no end", path);
		script.count = 0;
	}
	fclose(file);
	platform_log("test_input: script %s, %d steps", path, script.count);
	if (!script.count)
		script.finished = 1;
}

static void script_stop(const char *why)
{
	struct script_step const *step = &script.steps[script.current];

	platform_log("test_input: script stopped at line %d (%s): %s; screen \"%s\", focus \"%s\", tick %ld",
		step->line, step->text, why, script.screen, script.focus, script.tick);
	script.finished = 1;
}

static void script_next(void)
{
	script.current++;
	script.started = 0;
	if (script.current >= script.count)
	{
		platform_log("test_input: script finished");
		script.finished = 1;
	}
}

/* the step's buttons held */
static void script_buttons_held(XINPUT_GAMEPAD *pad, struct script_step const *step)
{
	int analog;

	pad->wButtons |= step->digital;
	for (analog = 0; analog < 8; analog++)
	{
		if (step->analog & (1 << analog))
			pad->bAnalogButtons[analog] = 255;
	}
}

/* a press (with releasing, after it) of the step's buttons; nonzero once
released for long enough */
static int script_press(XINPUT_GAMEPAD *pad, struct script_step const *step, unsigned long long milliseconds)
{
	if (script.phase_polls >= SCRIPT_PRESS_POLLS &&
		milliseconds - script.phase_milliseconds >= SCRIPT_PRESS_MILLISECONDS)
	{
		if (!script.pressed)
		{
			script.phase_polls = 0;
			script.pressed = 1;
			return 1;
		}
		script.pressed = 0;
		script.phase_polls = 0;
		script.phase_milliseconds = milliseconds;
	}
	if (!script.phase_polls && script.pressed)
		script.phase_milliseconds = milliseconds;
	script.phase_polls++;
	if (script.pressed)
		script_buttons_held(pad, step);
	return 0;
}

static SHORT script_stick(double value)
{
	if (value > 1.0)
		value = 1.0;
	if (value < -1.0)
		value = -1.0;
	return (SHORT)(value * 32767.0);
}

/* (test_input_observe_menus_inhibited's) */
static int script_menus_inhibited;

static void script_gamepad(XINPUT_GAMEPAD *pad, unsigned long long milliseconds)
{
	struct script_step const *step;
	double elapsed;

	if (!script.loaded)
		script_load();
	if (script.last_read && milliseconds > script.last_read)
		script.clock += milliseconds - script.last_read < 250 ? milliseconds - script.last_read : 250;
	script.last_read = milliseconds;
	/* (nothing before the game's first frame has said what it shows) */
	while (!script.finished && script.observed)
	{
		step = &script.steps[script.current];
		script.output_player = step->player;
		if (!script.started)
		{
			script.started = 1;
			script.started_clock = script.clock;
			script.started_tick = script.tick;
			script.started_checkpoints = script.checkpoints;
			script.pressed = 1;
			script.phase_polls = 0;
			script.phase_milliseconds = milliseconds;
			script.presses = 0;
			if (step->player)
				platform_log("test_input: line %d (%.1f s): controller %d: %s", step->line, (double)script.clock / 1000.0,
					step->player + 1, step->text);
			else
				platform_log("test_input: line %d (%.1f s): %s", step->line, (double)script.clock / 1000.0, step->text);
			if (step->kind == _step_objective)
			{
				level_run_begin();
				script.in_game_clock = script.clock;
			}
		}
		elapsed = (double)(script.clock - script.started_clock) / 1000.0;
		/* (a menu that takes no input yet, as while the saved games are first
		enumerated, gets no presses: a person would press again) */
		if ((step->kind == _step_press || step->kind == _step_select || step->kind == _step_hold ||
				step->kind == _step_stick) &&
			script.screen[0] && script_menus_inhibited)
		{
			if (elapsed >= 60.0)
				script_stop("the menus took no input for a minute");
			return;
		}
		switch (step->kind)
		{
		case _step_wait:
			if (elapsed < step->number)
				return;
			break;
		case _step_screen:
		case _step_focus:
		case _step_play:
		case _step_checkpoint:
		case _step_afoot:
		{
			int reached;

			if (step->kind == _step_screen)
				reached = script.screen[0] && script_contains(script.screen, step->argument);
			else if (step->kind == _step_focus)
				reached = script.focus[0] && script_contains(script.focus, step->argument);
			else if (step->kind == _step_play)
				reached = script.tick >= 0 && !script.screen[0];
			else if (step->kind == _step_afoot)
				reached = script.placed && !script.place[5];
			else
				reached = script.checkpoints > script.started_checkpoints;
			if (!reached)
			{
				if (elapsed >= step->number)
					script_stop("timed out");
				return;
			}
			break;
		}
		case _step_ticks:
			/* (from the first tick seen, if the step began outside a game) */
			if (script.started_tick < 0)
				script.started_tick = script.tick;
			if (script.tick < 0 || script.tick - script.started_tick < (long)step->number)
				return;
			break;
		case _step_press:
			if (script_press(pad, step, milliseconds))
				script.presses++;
			if (script.presses < (int)step->number)
				return;
			break;
		case _step_select:
			/* (the focus looked at before each press, the last released) */
			if (script.pressed && !script.phase_polls)
			{
				if (script.focus[0] && script_contains(script.focus, step->argument))
					break;
				if (script.presses >= (int)step->number)
				{
					script_stop("not found");
					return;
				}
			}
			if (script_press(pad, step, milliseconds))
				script.presses++;
			return;
		case _step_hold:
		case _step_stick:
		case _step_bot:
			/* (bot <seconds> checkpoint: until the game saves one, which it
			must within the time) */
			if (step->kind == _step_bot && step->argument[0] && script.checkpoints > script.started_checkpoints)
				break;
			/* (hold <buttons> <seconds> afoot: no longer once the player is out
			of its seat) */
			if (step->kind == _step_hold && step->argument[0] && script.placed && !script.place[5])
				break;
			/* (begun in a game and still in it: the game's time, so that a
			slower machine moves as far, its ticks the same) */
			if (script.started_tick >= 0 && script.tick >= script.started_tick)
				elapsed = (double)(script.tick - script.started_tick) / 30.0;
			if (elapsed >= step->number)
			{
				if (step->kind == _step_bot && step->argument[0])
					script_stop("timed out");
				else
					break;
				return;
			}
			if (step->kind == _step_hold)
			{
				script_buttons_held(pad, step);
			}
			else if (step->kind == _step_stick)
			{
				pad->sThumbLX = script_stick(step->sticks[0]);
				pad->sThumbLY = script_stick(step->sticks[1]);
				pad->sThumbRX = script_stick(step->sticks[2]);
				pad->sThumbRY = script_stick(step->sticks[3]);
			}
			else
			{
				test_input_bot(pad, milliseconds, 1, 0);
			}
			return;
		case _step_objective:
		{
			float sticks[4];
			unsigned long buttons;

			if (!level_run_controller(sticks, &buttons))
			{
				platform_log("test_input: the objective's level ended (tick %ld)", script.tick);
				break;
			}
			/* (out of a game for long: the campaign ended in the menus) */
			if (script.tick < 0)
			{
				if ((double)(script.clock - script.in_game_clock) / 1000.0 >= SCRIPT_OBJECTIVE_MENU_SECONDS)
				{
					platform_log("test_input: the objective's game ended");
					level_run_stop();
					break;
				}
				return;
			}
			script.in_game_clock = script.clock;
			if (script.started_tick < 0)
				script.started_tick = script.tick;
			/* (a screen over the game, a10's help: A, as a person dismisses it) */
			if (script.screen[0])
			{
				struct script_step accept;

				memset(&accept, 0, sizeof(accept));
				accept.analog = (unsigned char)(1 << XINPUT_GAMEPAD_A);
				script_press(pad, &accept, milliseconds);
				return;
			}
			/* (the game's time, as hold's) */
			if (script.tick >= script.started_tick && (double)(script.tick - script.started_tick) / 30.0 >= step->number)
			{
				level_run_stop();
				script_stop("timed out");
				return;
			}
			pad->sThumbLX = script_stick(sticks[0]);
			pad->sThumbLY = script_stick(sticks[1]);
			pad->sThumbRX = script_stick(sticks[2]);
			pad->sThumbRY = script_stick(sticks[3]);
			if (buttons & _level_run_button_fire)
				pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 255;
			if (buttons & _level_run_button_grenade)
				pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = 255;
			if (buttons & _level_run_button_action)
				pad->bAnalogButtons[XINPUT_GAMEPAD_X] = 255;
			if (buttons & _level_run_button_jump)
				pad->bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
			if (buttons & _level_run_button_back)
				pad->bAnalogButtons[XINPUT_GAMEPAD_B] = 255;
			if (buttons & _level_run_button_zoom)
				pad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
			return;
		}
		case _step_walk:
		{
			double dx, dy, distance, forward, right;

			if (!script.placed)
			{
				script_stop("no player");
				return;
			}
			dx = step->sticks[0] - script.place[0];
			dy = step->sticks[1] - script.place[1];
			distance = sqrt(dx * dx + dy * dy);
			if (distance < 0.1)
				break;
			if (elapsed >= step->number)
			{
				script_stop("timed out");
				return;
			}
			/* (the way there in the unit's own frame: its facing, and its right,
			the facing turned a quarter clockwise seen from above) */
			forward = (dx * script.place[3] + dy * script.place[4]) / distance;
			right = (dx * script.place[4] - dy * script.place[3]) / distance;
			/* (slower within a unit, so as not to overshoot) */
			if (distance < 1.0)
			{
				forward *= 0.4 + 0.6 * distance;
				right *= 0.4 + 0.6 * distance;
			}
			pad->sThumbLY = script_stick(forward);
			pad->sThumbLX = script_stick(right);
			return;
		}
		case _step_destroy:
		{
			char names[SCRIPT_TEXT_SIZE];
			char *name;

			snprintf(names, sizeof(names), "%s", step->argument);
			for (name = strtok(names, " "); name; name = strtok(NULL, " "))
				level_run_destroy(name);
			break;
		}
		case _step_log:
			if (script.placed)
			{
				platform_log("test_input: %s (at %.2f %.2f %.2f, facing %.0f degrees%s, tick %ld)", step->argument,
					script.place[0], script.place[1], script.place[2],
					atan2(script.place[4], script.place[3]) * 180.0 / 3.14159265358979, script.place[5] ? ", riding" : "",
					script.tick);
			}
			else
			{
				platform_log("test_input: %s", step->argument);
			}
			break;
		case _step_quit:
			script_next();
			platform_request_quit();
			return;
		case _step_controller:
			script.connected[(int)step->sticks[0]] = (int)step->sticks[1];
			break;
		case _step_bot_player:
			script.bots[(int)step->sticks[0]] = (int)step->sticks[1];
			break;
		case _step_wii:
			script_wii_step(step);
			break;
		case _step_repeat:
		case _step_end:
			break;
		}
		script_next();
	}
}

/* (the game's word on whether its menus take input: network_test.c, from
ui_widget.c's processing_inhibited) */
void test_input_observe_menus_inhibited(int inhibited)
{
	script_menus_inhibited = inhibited;
}

void test_input_observe(const char *screen, const char *focus, long tick, long checkpoints, const float *place)
{
	if (!test_input_scripted())
		return;
	if (!script.observed || strcmp(screen, script.screen) || strcmp(focus, script.focus))
	{
		platform_log("test_input: screen \"%s\", focus \"%s\" (tick %ld)", screen, focus, tick);
		snprintf(script.screen, sizeof(script.screen), "%s", screen);
		snprintf(script.focus, sizeof(script.focus), "%s", focus);
	}
	if (script.observed && (script.tick < 0) != (tick < 0))
		platform_log("test_input: %s (tick %ld)", tick < 0 ? "out of a game" : "in a game", tick);
	script.tick = tick;
	script.checkpoints = checkpoints;
	script.placed = place != NULL;
	if (place)
		memcpy(script.place, place, sizeof(script.place));
	script.observed = 1;
}

/* what the steps press, on the controller they are for, and a bot's play,
added to what the machine reports of the controller */
static void script_controller(int port, XINPUT_GAMEPAD *pad, unsigned long long milliseconds)
{
	int analog;

	if (port == 0)
	{
		/* (the steps run on the first controller's read, which the game
		makes before the others': the others take what it made) */
		memset(&script.output, 0, sizeof(script.output));
		script.output_player = 0;
		script_gamepad(&script.output, milliseconds);
	}
	if (script.output_player == port)
	{
		const XINPUT_GAMEPAD *made = &script.output;

		pad->wButtons |= made->wButtons;
		for (analog = 0; analog < 8; analog++)
		{
			if (made->bAnalogButtons[analog] > pad->bAnalogButtons[analog])
				pad->bAnalogButtons[analog] = made->bAnalogButtons[analog];
		}
		if (made->sThumbLX)
			pad->sThumbLX = made->sThumbLX;
		if (made->sThumbLY)
			pad->sThumbLY = made->sThumbLY;
		if (made->sThumbRX)
			pad->sThumbRX = made->sThumbRX;
		if (made->sThumbRY)
			pad->sThumbRY = made->sThumbRY;
	}
	if (script.bots[port])
	{
		/* (in a game, the game's ticks: the same input at the same tick) */
		test_input_bot(pad, script.tick >= 0 ? (unsigned long long)script.tick * 1000ULL / 30ULL + 1 : milliseconds,
			script.bots[port] - 1, 0);
	}
}

int test_input_controller(int port)
{
	if (port < 1 || port >= SCRIPT_CONTROLLERS || !test_input_scripted())
		return -1;
	return script.connected[port] - 1;
}

void test_input_gamepad(XINPUT_GAMEPAD *pad, unsigned long long milliseconds)
{
	test_input_gamepad_port(0, pad, milliseconds);
}

void test_input_gamepad_port(int port, XINPUT_GAMEPAD *pad, unsigned long long milliseconds)
{
	static int fixed_ticks = -1;

	if (!test_input_active() || port < 0 || port >= SCRIPT_CONTROLLERS)
		return;
	if (test_input_mode == _test_input_script)
	{
		script_controller(port, pad, milliseconds);
		return;
	}
	if (port != 0)
		return;
	/* (with debug.fixed_ticks, the game's ticks are the clock in a game: the
	same input at the same tick on every machine, for a lockstep check) */
	if (fixed_ticks < 0)
		fixed_ticks = config_boolean("debug.fixed_ticks") ? 1 : 0;
	if (fixed_ticks)
	{
		/* (no input until a game's tick is known: a frame of the menu's
		clock would differ from machine to machine) */
		if (network_test_game_tick < 0)
			return;
		milliseconds = (unsigned long long)network_test_game_tick * 1000ULL / 30ULL + 1;
	}
	if (test_input_holding_action)
	{
		/* (standing still, the button held from a second on) */
		if (!test_input_holding_action_since)
			test_input_holding_action_since = milliseconds ? milliseconds : 1;
		if (milliseconds - test_input_holding_action_since >= 1000)
			pad->bAnalogButtons[XINPUT_GAMEPAD_X] = 255;
		return;
	}
	test_input_bot(pad, milliseconds, test_input_seed, test_input_mode == _test_input_look);
}
