/*
WII_HOME_CHECK.C

A host check of the game's side of the HOME menu (tools/wii_input_check.py
builds and runs it with the mapping's check): wii_home_config.c's settings run
through WiiLibs' own state machine (wl/platform/home_menu.c, whose cases are in
WiiLibs' tests/platform/test_platform_home_menu.c) against fake controllers, a
fake game and a clock. What is checked is what is Halo's: the items (Resume,
Reset to title, Return to the loader), the confirmation before the loader, the
Classic Controller's lower button (b) accepting and its right one (a) going
back, the scripted controller's gamepad and HOME, the words of the notices,
that every word is in the overlay's font, and that Reset to title resumes the
game and then goes to its menu.
*/

#include <stdio.h>
#include <string.h>

#include "wii_home_config.h"
#include "wl/platform/overlay.h"

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

/* overlay.c draws through these; the check only asks it about text */
void wl_gx_set_cull(WlGxCull cull) { (void)cull; }
void wl_gx_set_blend(WlGxBlend blend) { (void)blend; }
void wl_gx_set_zmode(bool test, WlGxCompare func, bool write) { (void)test; (void)func; (void)write; }
void wl_gx_set_alpha_test(WlGxCompare func, uint8_t ref) { (void)func; (void)ref; }
void wl_gx_set_recipe(WlGxRecipe recipe) { (void)recipe; }
void wl_gx_draw(WlGxPrim prim, const WlGxVertex *verts, uint32_t count) { (void)prim; (void)verts; (void)count; }

/* ---------- a fake game and console */

static struct
{
	int paused_calls, resumed_calls;
	BOOL paused;
	int resets, leaves;
	int order;
	int resumed_at, reset_at;
	char log[64][112];
	int log_count;
} fake;

static void fake_set_paused(void *user, bool paused)
{
	(void)user;
	fake.paused = paused;
	if (paused)
		fake.paused_calls++;
	else
	{
		fake.resumed_calls++;
		fake.resumed_at = ++fake.order;
	}
}

static void fake_reset(void *user)
{
	(void)user;
	fake.resets++;
	fake.reset_at = ++fake.order;
}

static void fake_leave(void *user)
{
	(void)user;
	fake.leaves++;
}

static void fake_log(void *user, const char *text)
{
	(void)user;
	if (fake.log_count < 64)
		snprintf(fake.log[fake.log_count++], sizeof(fake.log[0]), "%s", text);
}

static const WlHomeOps ops = { NULL, fake_set_paused, NULL, fake_reset, fake_leave, NULL, fake_log };

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
	WlHomeMenu home;
	WlRawInput raw;
	WlHomeWorld world;
	uint64_t now;
	unsigned extra_nav;
	BOOL home_extra;
} rig;

static void start(void)
{
	WlHomeConfig config = wii_home_config();

	memset(&fake, 0, sizeof(fake));
	memset(&rig, 0, sizeof(rig));
	wl_home_menu_init(&rig.home, &config);
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
	wl_home_menu_step(&rig.home, &rig.raw, rig.extra_nav, rig.home_extra, &rig.world, rig.now, &ops);
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

static void check_items_and_words(void)
{
	start();
	open_menu();
	CHECK(wl_home_menu_open(&rig.home) && fake.paused, "HOME opens the menu and pauses the game");
	CHECK(rig.home.item_count == 3 && rig.home.items[0] == WL_HOME_ITEM_RESUME && rig.home.items[1] == WL_HOME_ITEM_RESET &&
		rig.home.items[2] == WL_HOME_ITEM_LOADER, "Resume, Reset to title, Return to the loader");
	CHECK(!strcmp(wl_home_menu_item_label(&rig.home, WL_HOME_ITEM_RESUME), "Resume") &&
		!strcmp(wl_home_menu_item_label(&rig.home, WL_HOME_ITEM_RESET), "Reset to title") &&
		!strcmp(wl_home_menu_item_label(&rig.home, WL_HOME_ITEM_LOADER), "Return to the loader"), "the items' words");
	CHECK(!strcmp(wl_home_menu_confirm_text(&rig.home), "Unsaved progress will be lost") &&
		!strcmp(wl_home_menu_choice_label(&rig.home, WL_HOME_CHOICE_CANCEL), "Cancel") &&
		!strcmp(wl_home_menu_choice_label(&rig.home, WL_HOME_CHOICE_LEAVE), "Return to the loader"),
		"the question's words");

	/* no level running: no Reset to title */
	start();
	rig.world.in_play = FALSE;
	open_menu();
	CHECK(rig.home.item_count == 2 && rig.home.items[1] == WL_HOME_ITEM_LOADER && !fake.paused,
		"in the menus: Resume and the loader, nothing paused");
}

static void check_reset_and_loader(void)
{
	/* Reset to title: the game runs again, then goes to its menu */
	start();
	open_menu();
	press_remote(WL_WM_DOWN);
	press_remote(WL_WM_A);
	CHECK(fake.resets == 1 && fake.resumed_calls == 1 && fake.leaves == 0 && fake.resumed_at < fake.reset_at,
		"Reset to title resumes the game, then goes to its menu, once");
	CHECK(logged("reset to the title"), "and says so in the log");

	/* the loader asks first, with Cancel chosen */
	start();
	open_menu();
	press_remote(WL_WM_UP);
	press_remote(WL_WM_A);
	CHECK(rig.home.state == WL_HOME_CONFIRM && rig.home.selection == WL_HOME_CHOICE_CANCEL && fake.leaves == 0,
		"the loader asks first");
	press_remote(WL_WM_DOWN);
	press_remote(WL_WM_A);
	CHECK(fake.leaves == 1 && fake.resets == 0 && fake.resumed_calls == 0 && logged("returning to the loader"),
		"and leaves once the answer is yes, without running the game");
}

static void check_classic_controller(void)
{
	/* its lower button, b, accepts and the right one, a, goes back (xinput_wii_map.c) */
	start();
	rig.raw.remotes[0].extension = WL_EXT_CLASSIC;
	press_remote(WL_CC_HOME);
	CHECK(wl_home_menu_open(&rig.home), "a Classic Controller's HOME opens it");
	press_remote(WL_CC_DOWN);
	CHECK(rig.home.selection == 1, "its D-pad moves");
	press_remote(WL_CC_A);
	CHECK(!wl_home_menu_open(&rig.home) && fake.resets == 0, "a goes back");
	open_menu();
	press_remote(WL_CC_DOWN);
	press_remote(WL_CC_B);
	CHECK(fake.resets == 1, "b accepts");
}

static void check_script(void)
{
	XINPUT_GAMEPAD pad;

	memset(&pad, 0, sizeof(pad));
	pad.wButtons = XINPUT_GAMEPAD_DPAD_DOWN;
	CHECK(wii_home_pad_navigation(&pad) == WL_HOME_NAV_DOWN, "script D-pad down");
	memset(&pad, 0, sizeof(pad));
	pad.bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
	CHECK(wii_home_pad_navigation(&pad) == WL_HOME_NAV_ACCEPT, "script A");
	memset(&pad, 0, sizeof(pad));
	pad.bAnalogButtons[XINPUT_GAMEPAD_B] = 255;
	pad.wButtons = XINPUT_GAMEPAD_DPAD_UP;
	CHECK(wii_home_pad_navigation(&pad) == (WL_HOME_NAV_BACK | WL_HOME_NAV_UP), "script B and up");
	memset(&pad, 0, sizeof(pad));
	pad.wButtons = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_DPAD_LEFT;
	pad.bAnalogButtons[XINPUT_GAMEPAD_X] = 255;
	CHECK(wii_home_pad_navigation(&pad) == 0, "no other button is navigation");

	start();
	rig.home_extra = TRUE;
	looks(2);
	rig.home_extra = FALSE;
	CHECK(wl_home_menu_open(&rig.home), "a script's HOME opens it");
	rig.extra_nav = WL_HOME_NAV_DOWN;
	looks(2);
	rig.extra_nav = 0;
	looks(2);
	CHECK(rig.home.selection == 1, "a script's down moves it");
	rig.extra_nav = WL_HOME_NAV_ACCEPT;
	looks(2);
	rig.extra_nav = 0;
	looks(2);
	CHECK(fake.resets == 1, "a script's A chooses Reset to title");
}

static void check_notices(void)
{
	char text[256];

	/* a Remote alone in play, with a player on it, and a weak battery */
	start();
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	rig.raw.remotes[0].battery = 8;
	looks(120);
	CHECK(wl_home_menu_notice_text(&rig.home, rig.now, text, sizeof(text)) > 0 &&
		strstr(text, "Wii Remote 1: Connect a Nunchuk or Classic Controller to move\n") &&
		strstr(text, "Wii Remote 1: Battery low (8%)\n"), "the notices' words: '%s'", text);
	CHECK(logged("Wii Remote 1 has no Nunchuk or Classic Controller") && logged("Wii Remote 1 battery low (8%)"),
		"and the log lines tests/home_menu.txt looks for");
}

static void check_words_are_in_the_font(void)
{
	WlHomeConfig menu = wii_home_config();
	WlStrapConfig strap = wii_strap_config();
	char text[256];
	int index;

	CHECK(wl_overlay_text_drawable(menu.title) && wl_overlay_text_drawable(menu.resume_label) &&
		wl_overlay_text_drawable(menu.reset_label) && wl_overlay_text_drawable(menu.loader_label) &&
		wl_overlay_text_drawable(menu.confirm_text) && wl_overlay_text_drawable(menu.cancel_label) &&
		wl_overlay_text_drawable(menu.batteries_heading) && wl_overlay_text_drawable(menu.hint_menu) &&
		wl_overlay_text_drawable(menu.hint_confirm), "the menu's words are in the overlay's font");
	start();
	rig.raw.remotes[0].extension = WL_EXT_NONE;
	rig.raw.remotes[0].battery = 5;
	looks(120);
	CHECK(wl_home_menu_notice_text(&rig.home, rig.now, text, sizeof(text)) > 0 && wl_overlay_text_drawable(text),
		"and the notices'");
	CHECK(wl_overlay_text_drawable(strap.title) && wl_overlay_text_drawable(strap.prompt), "the reminder's title and prompt");
	for (index = 0; index < WL_STRAP_MAX_LINES && strap.body[index].text; index++)
		CHECK(wl_overlay_text_drawable(strap.body[index].text), "the reminder's line %d is in the font", index);
}

int main(void)
{
	check_items_and_words();
	check_reset_and_loader();
	check_classic_controller();
	check_script();
	check_notices();
	check_words_are_in_the_font();
	printf("wii_home_check: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
