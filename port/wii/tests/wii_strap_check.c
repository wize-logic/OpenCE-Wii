/*
WII_STRAP_CHECK.C

A host check of the wrist strap reminder as the game configures it
(tools/wii_input_check.py builds and runs it): wii_home_config.c's settings run
through WiiLibs' own state machine (wl/platform/strap_screen.c, whose cases are
in WiiLibs' tests/platform/test_platform_strap_screen.c) against fake
controllers and a clock. It shows at the first look and only when the
setting is on; it ignores the controllers for the first second, and an A held
from before (the loader's) or through it does not go on; a fresh press of A
after the second goes on, whichever controller gives it (a Remote's A, a
Classic Controller's lower button, a GameCube controller's A, a script's);
other buttons do nothing; the game gets no controller while it is up and until
the A that went on is let go (or 1.5 s); it shows once; and the line for a
Nunchuk appears only when one is connected.
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

#define START_US 5000000u
/* the controllers are ignored for a second; the A that goes on is kept from the game for a second and a half */
#define IGNORE_US 1000000u
#define SWALLOW_US 1500000u

static WlStrapConfig config;

static WlRawInput none(void)
{
	WlRawInput raw;

	memset(&raw, 0, sizeof(raw));
	return raw;
}

static void remote(WlRawInput *raw, int port, unsigned char extension, uint32_t buttons)
{
	raw->remotes[port].connected = TRUE;
	raw->remotes[port].extension = extension;
	raw->remotes[port].buttons = buttons;
}

static void check_off(void)
{
	WlStrapScreen strap;
	WlRawInput raw = none();

	wl_strap_screen_init(&strap, &config, FALSE);
	remote(&raw, 0, WL_EXT_NONE, WL_WM_A);
	CHECK(!wl_strap_screen_step(&strap, &raw, 0, START_US), "off: never shows");
	CHECK(!wl_strap_screen_step(&strap, &raw, 0, START_US + 3000000u), "off: still never");
	CHECK(!wl_strap_screen_blocks_input(&strap), "off: the game has its controllers");
}

static void check_timing(void)
{
	WlStrapScreen strap;
	WlRawInput idle = none();
	WlRawInput press = none();

	wl_strap_screen_init(&strap, &config, TRUE);
	CHECK(!wl_strap_screen_up(&strap), "before the first look nothing is up");
	remote(&idle, 0, WL_EXT_NONE, 0);
	remote(&press, 0, WL_EXT_NONE, WL_WM_A);

	CHECK(wl_strap_screen_step(&strap, &idle, 0, START_US), "up at the first look");
	CHECK(wl_strap_screen_blocks_input(&strap), "the game sees no controller");
	CHECK(!wl_strap_screen_ready(&strap, START_US + 10000u), "not ready at once");
	/* a press in the first second does nothing, even a fresh one */
	CHECK(wl_strap_screen_step(&strap, &press, 0, START_US + 200000u), "a press at 0.2 s: still up");
	CHECK(wl_strap_screen_step(&strap, &idle, 0, START_US + 400000u), "released");
	CHECK(wl_strap_screen_step(&strap, &press, 0, START_US + IGNORE_US - 1u), "a press just before the second: still up");
	CHECK(wl_strap_screen_step(&strap, &idle, 0, START_US + IGNORE_US - 1u + 1000u), "released after it");
	CHECK(wl_strap_screen_ready(&strap, START_US + IGNORE_US), "ready at the second");
	/* a press after it goes on, and the game is kept from the A until it is let go */
	CHECK(!wl_strap_screen_step(&strap, &press, 0, START_US + IGNORE_US + 50000u), "a fresh press after the second goes on");
	CHECK(!wl_strap_screen_up(&strap), "and the screen is gone");
	CHECK(wl_strap_screen_blocks_input(&strap), "but the A is kept from the game");
	CHECK(!wl_strap_screen_step(&strap, &press, 0, START_US + IGNORE_US + 500000u), "held on");
	CHECK(wl_strap_screen_blocks_input(&strap), "still kept");
	CHECK(!wl_strap_screen_step(&strap, &idle, 0, START_US + IGNORE_US + 600000u), "let go");
	CHECK(!wl_strap_screen_blocks_input(&strap), "the game has its controllers again");
	/* once */
	CHECK(!wl_strap_screen_step(&strap, &press, 0, START_US + 9000000u), "it does not come back");
	CHECK(!wl_strap_screen_blocks_input(&strap), "and the game keeps its controllers");
}

static void check_held_a(void)
{
	WlStrapScreen strap;
	WlRawInput held = none();
	WlRawInput idle = none();

	remote(&held, 0, WL_EXT_NONE, WL_WM_A);
	remote(&idle, 0, WL_EXT_NONE, 0);

	/* an A held from the loader, kept down through the second: it does not go on */
	wl_strap_screen_init(&strap, &config, TRUE);
	CHECK(wl_strap_screen_step(&strap, &held, 0, START_US), "A held at the start: up");
	CHECK(wl_strap_screen_step(&strap, &held, 0, START_US + 1500000u), "and still up with it held past the second");
	CHECK(wl_strap_screen_step(&strap, &held, 0, START_US + 5000000u), "and long after");
	CHECK(wl_strap_screen_step(&strap, &idle, 0, START_US + 5100000u), "let go: still up");
	CHECK(!wl_strap_screen_step(&strap, &held, 0, START_US + 5200000u), "then a press goes on");

	/* the A that went on, held for good, is kept from the game only for 1.5 s */
	CHECK(wl_strap_screen_blocks_input(&strap), "kept");
	CHECK(!wl_strap_screen_step(&strap, &held, 0, START_US + 5200000u + SWALLOW_US), "held: gone after the limit");
	CHECK(!wl_strap_screen_blocks_input(&strap), "the game has its controllers after the limit");
}

static void check_controllers(void)
{
	WlStrapScreen strap;
	WlRawInput idle = none();
	WlRawInput raw;
	int which;

	remote(&idle, 0, WL_EXT_NONE, 0);
	for (which = 0; which < 8; which++)
	{
		BOOL accepts = TRUE;

		raw = none();
		switch (which)
		{
		case 0:
			remote(&raw, 0, WL_EXT_NONE, WL_WM_A);
			break;
		case 1:
			remote(&raw, 1, WL_EXT_NUNCHUK, WL_WM_A);
			break;
		case 2:
			remote(&raw, 0, WL_EXT_CLASSIC, WL_CC_B);
			break;
		case 3:
			raw.pads[2].connected = TRUE;
			raw.pads[2].buttons = WL_PAD_A;
			break;
		case 4:
			/* the buttons that are not A: B, 1, 2, Z, C, plus, the D-pad, HOME, and a Classic a */
			remote(&raw, 0, WL_EXT_NUNCHUK, WL_WM_B | WL_WM_1 | WL_WM_2 | WL_WM_PLUS | WL_NC_Z | WL_NC_C);
			accepts = FALSE;
			break;
		case 5:
			remote(&raw, 0, WL_EXT_NONE, WL_WM_LEFT | WL_WM_RIGHT | WL_WM_HOME);
			accepts = FALSE;
			break;
		case 6:
			remote(&raw, 0, WL_EXT_CLASSIC, WL_CC_A | WL_CC_X | WL_CC_Y | WL_CC_ZL);
			accepts = FALSE;
			break;
		default:
			raw.pads[0].connected = TRUE;
			raw.pads[0].buttons = WL_PAD_B | WL_PAD_X | WL_PAD_START;
			accepts = FALSE;
			break;
		}
		wl_strap_screen_init(&strap, &config, TRUE);
		wl_strap_screen_step(&strap, &idle, 0, START_US);
		wl_strap_screen_step(&strap, &idle, 0, START_US + 1100000u);
		wl_strap_screen_step(&strap, &raw, 0, START_US + 1200000u);
		CHECK(wl_strap_screen_up(&strap) == !accepts, "controller case %d: %s", which, accepts ? "A goes on" : "no A, stays up");
	}
	/* the scripted controller's A */
	wl_strap_screen_init(&strap, &config, TRUE);
	wl_strap_screen_step(&strap, &idle, 0, START_US);
	wl_strap_screen_step(&strap, &idle, 0, START_US + 1100000u);
	wl_strap_screen_step(&strap, &idle, WL_HOME_NAV_ACCEPT, START_US + 1200000u);
	CHECK(!wl_strap_screen_up(&strap), "a script's A goes on");
}

static int line_count(const WlStrapScreen *strap)
{
	int count = 0;

	while (wl_strap_screen_line(strap, count))
		count++;
	return count;
}

static void check_words(void)
{
	WlStrapScreen strap;
	WlRawInput raw = none();
	int index;

	wl_strap_screen_init(&strap, &config, TRUE);
	remote(&raw, 0, WL_EXT_NONE, 0);
	wl_strap_screen_step(&strap, &raw, 0, START_US);
	CHECK(strstr(wl_strap_screen_title(&strap), "WRIST STRAP") != NULL, "the title names the strap");
	CHECK(line_count(&strap) == 3, "no Nunchuk line without a Nunchuk: %d lines", line_count(&strap));
	remote(&raw, 0, WL_EXT_NUNCHUK, 0);
	wl_strap_screen_step(&strap, &raw, 0, START_US + 100000u);
	CHECK(line_count(&strap) == 4 && strstr(wl_strap_screen_line(&strap, 2), "NUNCHUK"), "a line for a Nunchuk when one is connected");
	remote(&raw, 0, WL_EXT_CLASSIC, 0);
	wl_strap_screen_step(&strap, &raw, 0, START_US + 200000u);
	CHECK(line_count(&strap) == 3, "and not once it is a Classic Controller");
	CHECK(strstr(wl_strap_screen_line(&strap, 2), "ROOM") != NULL && strstr(wl_strap_screen_prompt(&strap), "CONTINUE") != NULL,
		"room and the prompt");
	CHECK(wl_strap_screen_line(&strap, 3) == NULL, "nothing past the end");
	for (index = 0; index < 4; index++)
	{
		const char *line = index == 0 ? wl_strap_screen_title(&strap) : index == 3 ? wl_strap_screen_prompt(&strap) :
			wl_strap_screen_line(&strap, index - 1);

		CHECK(line != NULL && wl_overlay_text_drawable(line), "line %d is in the overlay's font", index);
	}
}

int main(void)
{
	config = wii_strap_config();
	check_off();
	check_timing();
	check_held_a();
	check_controllers();
	check_words();
	printf("wii_strap_check: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
