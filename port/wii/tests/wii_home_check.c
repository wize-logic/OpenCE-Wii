/*
WII_HOME_CHECK.C

A host check of port/wii/src/wii_home.c (tools/wii_input_check.py builds and
runs it with the mapping's check): the HOME menu's state machine and the
Wii's notices against fake controllers, a fake game and a clock. HOME opens
the menu (pausing the game once, unless it was paused already) and closes it
on the next press or on B; the items, the confirmation before the loader, the
buttons held while it closes kept from the game, the navigation each kind of
controller gives it, the notices (a Remote with nothing to move with, a weak
battery: once, debounced, not polled every frame) and that nothing is drawn
or asked while the menu is closed and no notice shows.
*/

#include <stdio.h>
#include <string.h>

#include "wii_home.h"

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

/* ---------- a fake game and console */

static struct
{
	int paused_calls, resumed_calls;
	BOOL paused;
	int fades;
	float gain;
	unsigned fade_ms;
	int resets, leaves;
	int visible_calls;
	BOOL visible;
	char log[64][112];
	int log_count;
} fake;

static void fake_set_paused(void *user, BOOL paused)
{
	(void)user;
	fake.paused = paused;
	if (paused)
		fake.paused_calls++;
	else
		fake.resumed_calls++;
}

static void fake_audio_fade(void *user, float gain, unsigned ms)
{
	(void)user;
	fake.fades++;
	fake.gain = gain;
	fake.fade_ms = ms;
}

static void fake_reset(void *user)
{
	(void)user;
	fake.resets++;
}

static void fake_leave(void *user)
{
	(void)user;
	fake.leaves++;
}

static void fake_visible(void *user, BOOL visible)
{
	(void)user;
	fake.visible_calls++;
	fake.visible = visible;
}

static void fake_log(void *user, const char *text)
{
	(void)user;
	if (fake.log_count < 64)
		snprintf(fake.log[fake.log_count++], sizeof(fake.log[0]), "%s", text);
}

static const struct wii_home_ops ops = { NULL, fake_set_paused, fake_audio_fade, fake_reset, fake_leave, fake_visible, fake_log };

static BOOL logged(const char *part)
{
	int index;

	for (index = 0; index < fake.log_count; index++)
	{
		if (strstr(fake.log[index], part))
			return TRUE;
	}
	return FALSE;
}

/* ---------- a rig: the machine, the controllers, the game, the time */

static struct
{
	struct wii_home home;
	WlRawInput raw;
	struct wii_home_world world;
	uint64_t now;
	unsigned extra_nav;
	BOOL home_extra;
} rig;

static void start(void)
{
	memset(&fake, 0, sizeof(fake));
	memset(&rig, 0, sizeof(rig));
	wii_home_init(&rig.home);
	rig.raw.remotes[0].connected = TRUE;
	rig.raw.remotes[0].extension = WL_EXT_NUNCHUK;
	rig.raw.remotes[0].battery = 80;
	rig.world.in_play = TRUE;
	rig.world.local_player[0] = TRUE;
	rig.now = 1000000;
}

/* a look 33 ms later */
static void look(void)
{
	rig.now += 33000;
	wii_home_step(&rig.home, &rig.raw, rig.extra_nav, rig.home_extra, &rig.world, rig.now, &ops);
}

static void looks(int count)
{
	while (count-- > 0)
		look();
}

/* a press: the buttons down for two looks, then up for two */
static void press_remote(uint32_t buttons)
{
	rig.raw.remotes[0].buttons = buttons;
	looks(2);
	rig.raw.remotes[0].buttons = 0;
	looks(2);
}

static void open_menu(void)
{
	press_remote(WL_WM_HOME);
}

/* ---------- the checks */

static void check_closed_costs_nothing(void)
{
	start();
	looks(300);
	CHECK(!wii_home_open(&rig.home) && !wii_home_blocks_input(&rig.home), "closed at the start");
	CHECK(fake.visible_calls == 0 && fake.fades == 0 && fake.paused_calls == 0 && fake.log_count == 0,
		"a closed menu asks for nothing: %d visible, %d fades, %d pauses, %d log lines", fake.visible_calls, fake.fades,
		fake.paused_calls, fake.log_count);
	/* the game's buttons are not the menu's while it is closed */
	rig.raw.remotes[0].buttons = WL_WM_A | WL_WM_UP | WL_WM_B | WL_WM_DOWN;
	looks(10);
	CHECK(!wii_home_open(&rig.home) && fake.visible_calls == 0, "A, B and the D-pad open nothing");
	/* a GameCube controller has no HOME: its Start and every other button open nothing */
	rig.raw.remotes[0].buttons = 0;
	rig.raw.pads[1].connected = TRUE;
	rig.raw.pads[1].buttons = 0xFFFF;
	looks(10);
	CHECK(!wii_home_open(&rig.home), "no GameCube button opens it");
	/* (and HOME of a Remote not connected is nothing) */
	rig.raw.pads[1].connected = FALSE;
	rig.raw.remotes[2].connected = FALSE;
	rig.raw.remotes[2].buttons = WL_WM_HOME;
	looks(5);
	CHECK(!wii_home_open(&rig.home), "a Remote that is not there has no HOME");
}

static void check_open_and_close(void)
{
	start();
	rig.raw.remotes[0].buttons = WL_WM_HOME;
	looks(1);
	CHECK(wii_home_open(&rig.home) && wii_home_blocks_input(&rig.home), "HOME opens the menu on the first look");
	CHECK(fake.paused_calls == 1 && fake.paused, "the game is paused, once");
	CHECK(fake.fades == 1 && fake.gain == 0.0f && fake.fade_ms == 300, "the sound fades out over 300 ms");
	CHECK(fake.visible && fake.visible_calls == 1, "the overlay is asked for");
	CHECK(rig.home.state == WII_HOME_MENU && rig.home.selection == 0 && rig.home.item_count == 3 &&
		rig.home.items[0] == WII_HOME_RESUME && rig.home.items[1] == WII_HOME_RESET &&
		rig.home.items[2] == WII_HOME_LOADER, "Resume, Reset to title, Return to the loader, Resume chosen");
	/* held, it is one press */
	looks(30);
	CHECK(rig.home.state == WII_HOME_MENU && fake.paused_calls == 1 && fake.resumed_calls == 0,
		"HOME held does not toggle the menu");
	rig.raw.remotes[0].buttons = 0;
	looks(3);
	CHECK(wii_home_open(&rig.home), "letting go does not close it");
	/* the second press closes it */
	rig.raw.remotes[0].buttons = WL_WM_HOME;
	looks(1);
	CHECK(!wii_home_open(&rig.home), "HOME again closes it");
	CHECK(fake.resumed_calls == 1 && !fake.paused, "the game runs again, once");
	CHECK(fake.fades == 2 && fake.gain == 1.0f && fake.fade_ms == 300, "the sound fades back in over 300 ms");
	CHECK(!fake.visible && fake.visible_calls == 2, "the overlay is let go");
	CHECK(wii_home_blocks_input(&rig.home), "HOME still down: the game still sees nothing");
	rig.raw.remotes[0].buttons = 0;
	looks(1);
	CHECK(!wii_home_blocks_input(&rig.home), "and everything the game sees once the button is up");
}

static void check_back(void)
{
	start();
	open_menu();
	press_remote(WL_WM_B);
	CHECK(!wii_home_open(&rig.home) && fake.resumed_calls == 1 && fake.gain == 1.0f, "B closes it, resuming the game");

	start();
	open_menu();
	press_remote(WL_WM_1);
	CHECK(!wii_home_open(&rig.home), "so does 1");
}

static void check_navigation(void)
{
	start();
	open_menu();
	press_remote(WL_WM_DOWN);
	CHECK(rig.home.selection == 1, "down moves to Reset to title");
	press_remote(WL_WM_DOWN);
	CHECK(rig.home.selection == 2, "down moves to Return to the loader");
	press_remote(WL_WM_DOWN);
	CHECK(rig.home.selection == 0, "down wraps to Resume");
	press_remote(WL_WM_UP);
	CHECK(rig.home.selection == 2, "up wraps to the last");
	/* held, it moves once */
	rig.raw.remotes[0].buttons = WL_WM_UP;
	looks(30);
	CHECK(rig.home.selection == 1, "a held direction moves once");
	rig.raw.remotes[0].buttons = 0;
	looks(2);
	/* the Nunchuk's stick */
	rig.raw.remotes[0].stick[0][1] = 100;
	looks(2);
	rig.raw.remotes[0].stick[0][1] = 0;
	looks(2);
	CHECK(rig.home.selection == 0, "the Nunchuk stick up moves up");
	rig.raw.remotes[0].stick[0][1] = -100;
	looks(2);
	rig.raw.remotes[0].stick[0][1] = 20;
	looks(2);
	CHECK(rig.home.selection == 1, "down on the stick moves down, a small push does nothing");
	/* a stick of a Remote with no extension is not read */
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	rig.raw.remotes[0].stick[0][1] = -100;
	looks(4);
	CHECK(rig.home.selection == 1, "no stick without a Nunchuk");
	rig.raw.remotes[0].stick[0][1] = 0;

	/* the Classic Controller: its own bits, b accepts and a goes back */
	rig.raw.remotes[0].extension = WL_EXT_CLASSIC;
	press_remote(WL_CC_DOWN);
	CHECK(rig.home.selection == 2, "the Classic Controller's D-pad moves");
	press_remote(WL_CC_UP);
	press_remote(WL_CC_UP);
	CHECK(rig.home.selection == 0, "and up");
	rig.raw.remotes[0].buttons = 0;
	press_remote(WL_CC_A);
	CHECK(!wii_home_open(&rig.home), "the Classic Controller's a goes back");

	/* the Nunchuk's Z (0x10000) is not the Classic Controller's up */
	start();
	open_menu();
	rig.raw.remotes[0].buttons = WL_NC_Z;
	looks(3);
	rig.raw.remotes[0].buttons = 0;
	looks(3);
	CHECK(rig.home.selection == 0, "the Nunchuk's Z is not a direction");

	/* a GameCube controller steers it (not opens it) */
	start();
	open_menu();
	rig.raw.pads[2].connected = TRUE;
	rig.raw.pads[2].buttons = WL_PAD_DOWN;
	looks(2);
	rig.raw.pads[2].buttons = 0;
	looks(2);
	CHECK(rig.home.selection == 1, "a GameCube D-pad moves it");
	rig.raw.pads[2].stick[0][1] = -120;
	looks(2);
	rig.raw.pads[2].stick[0][1] = 0;
	looks(2);
	CHECK(rig.home.selection == 2, "and its stick");
	rig.raw.pads[2].buttons = WL_PAD_B;
	looks(2);
	rig.raw.pads[2].buttons = 0;
	looks(2);
	CHECK(!wii_home_open(&rig.home), "and B goes back");

	/* the scripted controller (test_input.c) */
	{
		XINPUT_GAMEPAD pad;

		memset(&pad, 0, sizeof(pad));
		pad.wButtons = XINPUT_GAMEPAD_DPAD_DOWN;
		CHECK(wii_home_pad_navigation(&pad) == WII_NAV_DOWN, "script D-pad down");
		memset(&pad, 0, sizeof(pad));
		pad.bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
		CHECK(wii_home_pad_navigation(&pad) == WII_NAV_ACCEPT, "script A");
		memset(&pad, 0, sizeof(pad));
		pad.bAnalogButtons[XINPUT_GAMEPAD_B] = 255;
		pad.wButtons = XINPUT_GAMEPAD_DPAD_UP;
		CHECK(wii_home_pad_navigation(&pad) == (WII_NAV_BACK | WII_NAV_UP), "script B and up");
		start();
		rig.home_extra = TRUE;
		looks(2);
		rig.home_extra = FALSE;
		CHECK(wii_home_open(&rig.home), "a script's HOME opens it");
		rig.extra_nav = WII_NAV_DOWN;
		looks(2);
		rig.extra_nav = 0;
		looks(2);
		CHECK(rig.home.selection == 1, "a script's down moves it");
		rig.extra_nav = WII_NAV_ACCEPT;
		looks(2);
		rig.extra_nav = 0;
		looks(2);
		CHECK(fake.resets == 1, "a script's A chooses Reset to title");
	}
}

static void check_items(void)
{
	/* Resume */
	start();
	open_menu();
	press_remote(WL_WM_A);
	CHECK(!wii_home_open(&rig.home) && fake.resumed_calls == 1 && fake.resets == 0 && fake.leaves == 0,
		"Resume closes it");
	CHECK(fake.gain == 1.0f, "and the sound comes back");

	/* Reset to title: the game runs again, then goes to its menu */
	start();
	open_menu();
	press_remote(WL_WM_DOWN);
	press_remote(WL_WM_A);
	CHECK(!wii_home_open(&rig.home) && fake.resets == 1 && fake.resumed_calls == 1 && fake.leaves == 0,
		"Reset to title resumes the game and goes to the menu once");
	CHECK(logged("reset to the title"), "and says so in the log");

	/* the loader asks first */
	start();
	open_menu();
	press_remote(WL_WM_DOWN);
	press_remote(WL_WM_DOWN);
	press_remote(WL_WM_A);
	CHECK(rig.home.state == WII_HOME_CONFIRM && rig.home.selection == WII_HOME_CANCEL && fake.leaves == 0 &&
		wii_home_open(&rig.home), "the loader asks first, with Cancel chosen");
	CHECK(strstr(wii_home_confirm_text(), "Unsaved progress will be lost") != NULL, "the warning says what is lost");
	press_remote(WL_WM_A);
	CHECK(rig.home.state == WII_HOME_MENU && rig.home.selection == 2 && fake.leaves == 0, "Cancel is back in the menu, on the loader");
	press_remote(WL_WM_A);
	press_remote(WL_WM_B);
	CHECK(rig.home.state == WII_HOME_MENU && fake.leaves == 0, "B in the question is back in the menu");
	press_remote(WL_WM_A);
	press_remote(WL_WM_DOWN);
	CHECK(rig.home.selection == WII_HOME_LEAVE, "down is Return to the loader");
	press_remote(WL_WM_DOWN);
	CHECK(rig.home.selection == WII_HOME_CANCEL, "and wraps");
	press_remote(WL_WM_UP);
	press_remote(WL_WM_A);
	CHECK(fake.leaves == 1, "choosing it leaves, once");
	CHECK(logged("returning to the loader"), "after logging it");
	CHECK(fake.resets == 0, "and does not reset");

	/* HOME in the question resumes */
	start();
	open_menu();
	press_remote(WL_WM_UP);
	press_remote(WL_WM_A);
	CHECK(rig.home.state == WII_HOME_CONFIRM, "up from Resume is the loader");
	press_remote(WL_WM_HOME);
	CHECK(!wii_home_open(&rig.home) && fake.leaves == 0 && fake.resumed_calls == 1, "HOME in the question resumes");
}

static void check_pause_rules(void)
{
	/* the game was paused by its own menu: left as it was */
	start();
	rig.world.game_paused = TRUE;
	fake.paused = TRUE;
	open_menu();
	CHECK(wii_home_open(&rig.home) && fake.paused_calls == 0, "a game paused already is not paused again");
	press_remote(WL_WM_HOME);
	CHECK(!wii_home_open(&rig.home) && fake.resumed_calls == 0 && fake.paused, "and is not resumed by the menu");
	CHECK(fake.gain == 1.0f && fake.fades == 2, "the sound is the menu's either way");

	/* no level running: no Reset to title, no pause */
	start();
	rig.world.in_play = FALSE;
	open_menu();
	CHECK(wii_home_open(&rig.home) && fake.paused_calls == 0, "no level: nothing paused");
	CHECK(rig.home.item_count == 2 && rig.home.items[0] == WII_HOME_RESUME && rig.home.items[1] == WII_HOME_LOADER,
		"Resume and the loader only");
	press_remote(WL_WM_A);
	CHECK(!wii_home_open(&rig.home) && fake.resumed_calls == 0, "Resume closes it, resuming nothing");

	/* the console leaving does not run the game again on the way out */
	start();
	open_menu();
	press_remote(WL_WM_UP);
	press_remote(WL_WM_A);
	press_remote(WL_WM_DOWN);
	press_remote(WL_WM_A);
	CHECK(fake.leaves == 1 && fake.resumed_calls == 0, "leaving for the loader does not run the game");
}

static void check_swallow(void)
{
	/* A chose Resume and is still down: the game does not jump */
	start();
	open_menu();
	rig.raw.remotes[0].buttons = WL_WM_A;
	looks(2);
	CHECK(!wii_home_open(&rig.home) && wii_home_blocks_input(&rig.home), "closed with A down: the game sees nothing yet");
	looks(10);
	CHECK(wii_home_blocks_input(&rig.home), "for as long as it is down");
	rig.raw.remotes[0].buttons = 0;
	looks(1);
	CHECK(!wii_home_blocks_input(&rig.home), "and no longer");

	/* a button stuck down does not hold the game off for ever */
	start();
	open_menu();
	rig.raw.remotes[0].buttons = WL_WM_A;
	looks(2);
	rig.now += WII_HOME_SWALLOW_US;
	looks(1);
	CHECK(!wii_home_blocks_input(&rig.home), "after a second and a half the game has its controller back");

	/* another controller's button counts as held too */
	start();
	open_menu();
	rig.raw.pads[3].connected = TRUE;
	rig.raw.pads[3].buttons = WL_PAD_A;
	rig.raw.remotes[0].buttons = WL_WM_A;
	looks(2);
	rig.raw.remotes[0].buttons = 0;
	looks(2);
	CHECK(wii_home_blocks_input(&rig.home), "a GameCube button held keeps it off as well");
	rig.raw.pads[3].buttons = 0;
	looks(1);
	CHECK(!wii_home_blocks_input(&rig.home), "until it is let go");

	/* a button down as HOME is pressed does not choose anything */
	start();
	rig.raw.remotes[0].buttons = WL_WM_A | WL_WM_HOME;
	looks(3);
	CHECK(wii_home_open(&rig.home) && rig.home.state == WII_HOME_MENU && fake.resumed_calls == 0,
		"A held as the menu opens does not choose Resume");
}

static void check_home_variants(void)
{
	/* a Classic Controller's own HOME */
	start();
	rig.raw.remotes[0].extension = WL_EXT_CLASSIC;
	press_remote(WL_CC_HOME);
	CHECK(wii_home_open(&rig.home), "a Classic Controller's HOME opens it");

	/* its bit means nothing on a Remote without one */
	start();
	rig.raw.remotes[0].extension = WL_EXT_NUNCHUK;
	press_remote(WL_CC_HOME);
	CHECK(!wii_home_open(&rig.home), "that bit is not HOME on a Nunchuk's Remote");

	/* any Remote's */
	start();
	rig.raw.remotes[3].connected = TRUE;
	rig.raw.remotes[3].extension = WL_EXT_NONE;
	rig.raw.remotes[3].buttons = WL_WM_HOME;
	looks(2);
	CHECK(wii_home_open(&rig.home), "any Remote's HOME opens it");
	/* ... and any Remote's closes it */
	rig.raw.remotes[3].buttons = 0;
	looks(2);
	rig.raw.remotes[0].buttons = WL_WM_HOME;
	looks(2);
	CHECK(!wii_home_open(&rig.home), "any Remote's HOME closes it");
}

static void check_menu_batteries(void)
{
	start();
	rig.raw.remotes[1].connected = TRUE;
	rig.raw.remotes[1].extension = WL_EXT_CLASSIC;
	rig.raw.remotes[1].battery = 35;
	open_menu();
	CHECK(rig.home.present[0] && rig.home.present[1] && !rig.home.present[2] && !rig.home.present[3],
		"the menu knows which Remotes are there");
	CHECK(rig.home.battery[0] == 80 && rig.home.battery[1] == 35, "and their batteries");
	rig.raw.remotes[1].battery = 12;
	looks(1);
	CHECK(rig.home.battery[1] == 12, "as they change");
}

static void check_no_extension_notice(void)
{
	char text[256];
	int index;

	/* a Remote alone in play, with a player on it */
	start();
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	looks(20); /* 0.66 s */
	CHECK(!rig.home.no_extension_told[0] && !fake.visible, "nothing said at once: a Nunchuk plugged in takes a moment");
	looks(30); /* 1.65 s */
	CHECK(fake.visible && rig.home.no_extension_told[0], "said after a moment and a half");
	CHECK(wii_home_notice_text(&rig.home, rig.now, text, sizeof(text)) > 0 &&
		strstr(text, "Wii Remote 1") && strstr(text, "Connect a Nunchuk or Classic Controller to move"),
		"the words: connect a Nunchuk or Classic Controller to move: '%s'", text);
	CHECK(!wii_home_open(&rig.home) && !wii_home_blocks_input(&rig.home) && fake.paused_calls == 0,
		"a notice does not pause the game or take the controller");
	CHECK(fake.visible_calls == 1, "the overlay asked for once");
	/* it goes after its time, and is not said again while it lasts */
	for (index = 0; index < 400; index++)
		look();
	CHECK(!fake.visible && fake.visible_calls == 2, "the notice goes after a few seconds");
	CHECK(wii_home_notice_text(&rig.home, rig.now, text, sizeof(text)) == 0, "with no text");
	looks(100);
	CHECK(fake.visible_calls == 2, "and is not said again while nothing changes");
	/* a Nunchuk plugged in, and pulled out: said again */
	rig.raw.remotes[0].extension = WL_EXT_NUNCHUK;
	looks(5);
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	looks(60);
	CHECK(fake.visible && fake.visible_calls == 3, "pulled out again: said again");

	/* a Nunchuk that is plugged in within the moment is not complained of */
	start();
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	looks(30); /* 1 s */
	rig.raw.remotes[0].extension = WL_EXT_NUNCHUK;
	looks(3);
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	looks(30);
	CHECK(!fake.visible, "the moment starts afresh when something was plugged in");

	/* not in play: the menus work with a Remote alone */
	start();
	rig.world.in_play = FALSE;
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	looks(120);
	CHECK(!fake.visible, "no notice in the menus");

	/* no player on that Remote */
	start();
	rig.world.local_player[0] = FALSE;
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	looks(120);
	CHECK(!fake.visible, "no notice for a Remote with no player");

	/* a Classic Controller, a Motion Plus, or a Remote that is gone */
	start();
	rig.raw.remotes[0].extension = WL_EXT_CLASSIC;
	looks(120);
	rig.raw.remotes[0].extension = WL_EXT_MOTIONPLUS;
	looks(120);
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	rig.raw.remotes[0].connected = FALSE;
	looks(120);
	CHECK(!fake.visible, "no notice for a Remote with something plugged in, or none there");

	/* it comes with the menu, and goes on after the menu closes */
	start();
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	looks(50);
	CHECK(fake.visible, "notice up");
	open_menu();
	CHECK(fake.visible && fake.visible_calls == 1, "the menu opening does not ask for the overlay again");
	press_remote(WL_WM_HOME);
	CHECK(fake.visible, "the notice is still up after the menu closes");
}

static void check_battery_notice(void)
{
	char text[256];
	int index;
	int told_at = -1;

	start();
	/* well charged for five seconds */
	for (index = 0; index < 150; index++)
		look();
	CHECK(!fake.visible, "a good battery says nothing");
	rig.raw.remotes[0].battery = 8;
	for (index = 0; index < 200 && told_at < 0; index++)
	{
		look();
		if (fake.visible)
			told_at = index;
	}
	/* looked at every two seconds (60 looks), not on the look it fell */
	CHECK(told_at > 0 && told_at <= 62, "told within two seconds of the fall, and not at once (look %d)", told_at);
	CHECK(wii_home_notice_text(&rig.home, rig.now, text, sizeof(text)) > 0 && strstr(text, "Wii Remote 1") &&
		strstr(text, "Battery low (8%)"), "the words name the Remote and the level: '%s'", text);
	/* once, however long it stays low */
	for (index = 0; index < 1500; index++)
		look();
	CHECK(fake.visible_calls == 2 && !fake.visible, "told once: up, and down after its seconds");
	/* a little recovery does not rearm it, a full one does */
	rig.raw.remotes[0].battery = 12;
	for (index = 0; index < 300; index++)
		look();
	rig.raw.remotes[0].battery = 9;
	for (index = 0; index < 300; index++)
		look();
	CHECK(fake.visible_calls == 2, "9%% after 12%% is the same low battery");
	rig.raw.remotes[0].battery = 60;
	for (index = 0; index < 300; index++)
		look();
	rig.raw.remotes[0].battery = 7;
	for (index = 0; index < 300; index++)
		look();
	CHECK(fake.visible_calls == 4, "new batteries and a new fall: told again");

	/* a 0 is a Remote that has not said (its first status report is on its way) */
	start();
	rig.raw.remotes[0].battery = 0;
	looks(300);
	CHECK(!fake.visible, "0%% at the start is not known yet");
	rig.raw.remotes[0].battery = 60;
	looks(100);
	rig.raw.remotes[0].battery = 0;
	looks(100);
	CHECK(fake.visible, "a battery that was known and then reads 0 is flat");

	/* the second Remote's, by its number */
	start();
	rig.raw.remotes[2].connected = TRUE;
	rig.raw.remotes[2].extension = WL_EXT_NUNCHUK;
	rig.raw.remotes[2].battery = 4;
	looks(70);
	CHECK(fake.visible && wii_home_notice_text(&rig.home, rig.now, text, sizeof(text)) > 0 &&
		strstr(text, "Wii Remote 3: Battery low (4%)"), "Remote 3's battery: '%s'", text);

	/* a Remote that is gone and returns is read afresh */
	start();
	rig.raw.remotes[0].battery = 60;
	looks(100);
	rig.raw.remotes[0].connected = FALSE;
	looks(100);
	rig.raw.remotes[0].connected = TRUE;
	rig.raw.remotes[0].battery = 0;
	looks(150);
	CHECK(!fake.visible, "a reconnected Remote's 0 is not yet known");

	/* both notices at once, in a text of two lines */
	start();
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	rig.raw.remotes[0].battery = 3;
	looks(80);
	{
		unsigned length = wii_home_notice_text(&rig.home, rig.now, text, sizeof(text));
		int lines = 0;

		for (index = 0; index < (int)length; index++)
			lines += text[index] == '\n';
		CHECK(lines == 2 && strstr(text, "Nunchuk") && strstr(text, "Battery low"),
			"both at once, a line each: %d lines '%s'", lines, text);
	}
}

static void check_not_polled_every_frame(void)
{
	/* a battery that fell between two looks of the battery is not seen until the next: the
	look is every two seconds */
	start();
	looks(10);
	rig.raw.remotes[0].battery = 5;
	looks(10);
	CHECK(!fake.visible, "a fall is not seen on the next look, only on the next battery look");
}

int main(void)
{
	check_closed_costs_nothing();
	check_open_and_close();
	check_back();
	check_navigation();
	check_items();
	check_pause_rules();
	check_swallow();
	check_home_variants();
	check_menu_batteries();
	check_no_extension_notice();
	check_battery_notice();
	check_not_polled_every_frame();
	printf("wii_home_check: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
