/*
WII_STRAP_CHECK.C

A host check of port/wii/src/wii_strap.c (tools/wii_input_check.py builds and
runs it with wii_home.c): the wrist strap reminder's state machine against
fake controllers and a clock. It shows at the first look and only when the
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

#include "wii_home.h"
#include "wii_strap.h"

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

#define START_US 5000000u

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
	struct wii_strap strap;
	WlRawInput raw = none();

	wii_strap_init(&strap, FALSE);
	remote(&raw, 0, WL_EXT_NONE, WL_WM_A);
	CHECK(!wii_strap_step(&strap, &raw, 0, START_US), "off: never shows");
	CHECK(!wii_strap_step(&strap, &raw, 0, START_US + 3000000u), "off: still never");
	CHECK(!wii_strap_blocks_input(&strap), "off: the game has its controllers");
}

static void check_timing(void)
{
	struct wii_strap strap;
	WlRawInput idle = none();
	WlRawInput press = none();

	wii_strap_init(&strap, TRUE);
	CHECK(!wii_strap_up(&strap), "before the first look nothing is up");
	remote(&idle, 0, WL_EXT_NONE, 0);
	remote(&press, 0, WL_EXT_NONE, WL_WM_A);

	CHECK(wii_strap_step(&strap, &idle, 0, START_US), "up at the first look");
	CHECK(wii_strap_blocks_input(&strap), "the game sees no controller");
	CHECK(!wii_strap_ready(&strap, START_US + 10000u), "not ready at once");
	/* a press in the first second does nothing, even a fresh one */
	CHECK(wii_strap_step(&strap, &press, 0, START_US + 200000u), "a press at 0.2 s: still up");
	CHECK(wii_strap_step(&strap, &idle, 0, START_US + 400000u), "released");
	CHECK(wii_strap_step(&strap, &press, 0, START_US + WII_STRAP_IGNORE_US - 1u), "a press just before the second: still up");
	CHECK(wii_strap_step(&strap, &idle, 0, START_US + WII_STRAP_IGNORE_US - 1u + 1000u), "released after it");
	CHECK(wii_strap_ready(&strap, START_US + WII_STRAP_IGNORE_US), "ready at the second");
	/* a press after it goes on, and the game is kept from the A until it is let go */
	CHECK(!wii_strap_step(&strap, &press, 0, START_US + WII_STRAP_IGNORE_US + 50000u), "a fresh press after the second goes on");
	CHECK(!wii_strap_up(&strap), "and the screen is gone");
	CHECK(wii_strap_blocks_input(&strap), "but the A is kept from the game");
	CHECK(!wii_strap_step(&strap, &press, 0, START_US + WII_STRAP_IGNORE_US + 500000u), "held on");
	CHECK(wii_strap_blocks_input(&strap), "still kept");
	CHECK(!wii_strap_step(&strap, &idle, 0, START_US + WII_STRAP_IGNORE_US + 600000u), "let go");
	CHECK(!wii_strap_blocks_input(&strap), "the game has its controllers again");
	/* once */
	CHECK(!wii_strap_step(&strap, &press, 0, START_US + 9000000u), "it does not come back");
	CHECK(!wii_strap_blocks_input(&strap), "and the game keeps its controllers");
}

static void check_held_a(void)
{
	struct wii_strap strap;
	WlRawInput held = none();
	WlRawInput idle = none();

	remote(&held, 0, WL_EXT_NONE, WL_WM_A);
	remote(&idle, 0, WL_EXT_NONE, 0);

	/* an A held from the loader, kept down through the second: it does not go on */
	wii_strap_init(&strap, TRUE);
	CHECK(wii_strap_step(&strap, &held, 0, START_US), "A held at the start: up");
	CHECK(wii_strap_step(&strap, &held, 0, START_US + 1500000u), "and still up with it held past the second");
	CHECK(wii_strap_step(&strap, &held, 0, START_US + 5000000u), "and long after");
	CHECK(wii_strap_step(&strap, &idle, 0, START_US + 5100000u), "let go: still up");
	CHECK(!wii_strap_step(&strap, &held, 0, START_US + 5200000u), "then a press goes on");

	/* the A that went on, held for good, is kept from the game only for 1.5 s */
	CHECK(wii_strap_blocks_input(&strap), "kept");
	CHECK(!wii_strap_step(&strap, &held, 0, START_US + 5200000u + WII_STRAP_SWALLOW_US), "held: gone after the limit");
	CHECK(!wii_strap_blocks_input(&strap), "the game has its controllers after the limit");
}

static void check_controllers(void)
{
	struct wii_strap strap;
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
		wii_strap_init(&strap, TRUE);
		wii_strap_step(&strap, &idle, 0, START_US);
		wii_strap_step(&strap, &idle, 0, START_US + 1100000u);
		wii_strap_step(&strap, &raw, 0, START_US + 1200000u);
		CHECK(wii_strap_up(&strap) == !accepts, "controller case %d: %s", which, accepts ? "A goes on" : "no A, stays up");
	}
	/* the scripted controller's A */
	wii_strap_init(&strap, TRUE);
	wii_strap_step(&strap, &idle, 0, START_US);
	wii_strap_step(&strap, &idle, 0, START_US + 1100000u);
	wii_strap_step(&strap, &idle, WII_NAV_ACCEPT, START_US + 1200000u);
	CHECK(!wii_strap_up(&strap), "a script's A goes on");
}

static void check_words(void)
{
	struct wii_strap strap;
	WlRawInput raw = none();
	int index;

	wii_strap_init(&strap, TRUE);
	remote(&raw, 0, WL_EXT_NONE, 0);
	wii_strap_step(&strap, &raw, 0, START_US);
	CHECK(wii_strap_line(&strap, 0) != NULL && strstr(wii_strap_line(&strap, 0), "WRIST STRAP"), "the title names the strap");
	CHECK(wii_strap_line(&strap, 3) == NULL, "no Nunchuk line without a Nunchuk");
	remote(&raw, 0, WL_EXT_NUNCHUK, 0);
	wii_strap_step(&strap, &raw, 0, START_US + 100000u);
	CHECK(wii_strap_line(&strap, 3) != NULL && strstr(wii_strap_line(&strap, 3), "NUNCHUK"), "a line for a Nunchuk when one is connected");
	remote(&raw, 0, WL_EXT_CLASSIC, 0);
	wii_strap_step(&strap, &raw, 0, START_US + 200000u);
	CHECK(wii_strap_line(&strap, 3) == NULL, "and not once it is a Classic Controller");
	CHECK(wii_strap_line(&strap, 4) != NULL && wii_strap_line(&strap, 5) != NULL, "room and the prompt");
	CHECK(wii_strap_line(&strap, 6) == NULL, "nothing past the end");
	/* the font is capitals, digits and a little punctuation (wl/platform/overlay.h) */
	for (index = 0; index < 6; index++)
	{
		const char *line = wii_strap_line(&strap, index);
		const char *c;

		for (c = line ? line : ""; *c; c++)
			CHECK((*c >= 'A' && *c <= 'Z') || *c == ' ' || *c == '.' || *c == ',' || *c == ':', "line %d has a character the font may lack: '%c'", index, *c);
	}
}

int main(void)
{
	check_off();
	check_timing();
	check_held_a();
	check_controllers();
	check_words();
	printf("wii_strap_check: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
