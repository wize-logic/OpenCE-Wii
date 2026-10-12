/*
WII_HOME_GAME.C

The HOME menu and the Wii's notices (wii_home.c) joined to the game: see
wii_home_game.h.

- The game is paused as its own pause menu pauses it, game_time_set_paused
  (the sound it pauses itself is not touched: WiiLibs' master fade silences
  the output instead, wl_platform_audio_fade, 300 ms out and back).
- The menu is drawn by WiiLibs at the end of the frame, over the finished
  picture, from wl_gx_set_overlay's hook (the Direct3D device on GX,
  d3d8_gx.c, is not involved: its Present ends the frame with WiiLibs'
  wl_gx_end_frame, which calls the hook). The hook is set only while there
  is something to draw, so a closed menu costs the frame nothing; the
  controllers are looked at where the game reads them (xinput_wii.c), a few
  comparisons a read.
- Reset to title is the game's own way to its main menu
  (main_goto_main_menu); Return to the loader is the menus' Quit
  (platform_request_quit).
*/

#include "platform.h"

#include <stdio.h>
#include <string.h>

#include "wl/platform/overlay.h"
#include "wl/platform/platform.h"

#include "port_config.h"
#include "wii_home.h"
#include "wii_home_game.h"
#include "wii_strap.h"

/* (ui_widget.c, game_time.c, main.c, players.c, wii_platform.c) */
extern unsigned char main_menu_is_active(void);
extern unsigned char game_time_get_paused(void);
extern void game_time_set_paused(unsigned char paused);
extern unsigned char game_in_progress(void);
extern unsigned char game_time_initialized(void);
extern void main_goto_main_menu(void);
extern unsigned char local_player_exists(short controller_index);
void platform_request_quit(void);

static struct wii_home home;
static BOOL home_started;
/* the wrist strap reminder (display.wii_strap_reminder), shown once at the start */
static struct wii_strap strap;
/* what has the overlay hooked in: the HOME menu or notice, the reminder */
static BOOL home_visible;
static BOOL strap_visible;
static BOOL overlay_hooked;
/* (the rumble service reads these on the watcher thread) */
static volatile int home_open_flag;
static volatile int home_blocking_flag;
static volatile int strap_blocking_flag;

/* ---------- the ops */

static void ops_set_paused(void *user, BOOL paused)
{
	(void)user;
	game_time_set_paused(paused ? 1 : 0);
}

static void ops_audio_fade(void *user, float gain, unsigned milliseconds)
{
	(void)user;
	wl_platform_audio_fade(gain, milliseconds);
}

static void ops_reset_to_title(void *user)
{
	(void)user;
	main_goto_main_menu();
}

static void ops_leave_for_loader(void *user)
{
	(void)user;
	platform_request_quit();
}

static void home_draw(void *user, uint32_t width, uint32_t height);

/* (nothing is hooked into the frame while there is nothing to draw) */
static void overlay_update(void)
{
	BOOL wanted = home_visible || strap_visible;

	if (wanted != overlay_hooked)
	{
		overlay_hooked = wanted;
		wl_gx_set_overlay(wanted ? home_draw : NULL, NULL);
	}
}

static void ops_visible(void *user, BOOL visible)
{
	(void)user;
	home_visible = visible;
	overlay_update();
}

static void ops_log(void *user, const char *text)
{
	(void)user;
	platform_log("%s", text);
}

static const struct wii_home_ops home_ops =
{
	NULL,
	ops_set_paused,
	ops_audio_fade,
	ops_reset_to_title,
	ops_leave_for_loader,
	ops_visible,
	ops_log
};

/* ---------- the poll */

void wii_home_game_poll(const WlRawInput *raw, uint64_t now_us, unsigned extra_nav, BOOL home_extra)
{
	struct wii_home_world world;
	int port;

	if (!home_started)
	{
		home_started = TRUE;
		wii_home_init(&home);
		wii_strap_init(&strap, config_boolean("display.wii_strap_reminder") ? TRUE : FALSE);
	}
	/* (the reminder, while it is up, takes the controllers: HOME does nothing
	under it, and the game sees nothing) */
	if (strap.state == WII_STRAP_WAITING || strap.state == WII_STRAP_UP || strap.state == WII_STRAP_SWALLOWING)
	{
		BOOL up = wii_strap_step(&strap, raw, extra_nav, now_us);

		if (up != strap_visible)
		{
			strap_visible = up;
			overlay_update();
		}
		strap_blocking_flag = wii_strap_blocks_input(&strap);
		if (up)
		{
			home_open_flag = 1;
			home_blocking_flag = 1;
			return;
		}
	}
	memset(&world, 0, sizeof(world));
	/* (the controllers are read from the game's start, before its time exists:
	game_in_progress and game_time_get_paused assert it does) */
	if (game_time_initialized())
	{
		world.in_play = game_in_progress() && !main_menu_is_active();
		world.game_paused = game_time_get_paused() ? TRUE : FALSE;
	}
	/* (asked of the game only for the Remotes that could be told they have
	nothing to move with) */
	for (port = 0; world.in_play && port < WII_HOME_PORTS; port++)
	{
		if (raw->remotes[port].connected && raw->remotes[port].extension == WL_EXT_NONE)
			world.local_player[port] = local_player_exists((short)port) ? TRUE : FALSE;
	}
	wii_home_step(&home, raw, extra_nav, home_extra, &world, now_us, &home_ops);
	home_open_flag = wii_home_open(&home);
	home_blocking_flag = wii_home_blocks_input(&home);
}

BOOL wii_home_game_open(void)
{
	return home_open_flag ? TRUE : FALSE;
}

BOOL wii_home_game_blocks_input(void)
{
	return home_blocking_flag || strap_blocking_flag ? TRUE : FALSE;
}

/* ---------- the overlay: capitals only, in WiiLibs' small font */

#define COLOR_DIM 0x000000B8u
#define COLOR_PANEL 0x10182CE0u
#define COLOR_FRAME 0x7FA6FFFFu
#define COLOR_SELECTED 0x2B5FD8F0u
#define COLOR_TEXT 0xFFFFFFFFu
#define COLOR_GREY 0xB4B8C4FFu
#define COLOR_WARNING 0xFFD25AFFu
#define COLOR_GOOD 0x58D26CFFu
#define COLOR_LOW 0xE8504CFFu
#define COLOR_GAUGE_BACK 0x303848FFu

static int text_centered(uint32_t width, int y, int scale, uint32_t rgba, const char *text)
{
	int x = ((int)width - wl_overlay_text_width(scale, text)) / 2;

	wl_overlay_text(x, y, scale, rgba, text);
	return x;
}

static void draw_menu(uint32_t width, uint32_t height)
{
	int panel_w = 420;
	int panel_x = ((int)width - panel_w) / 2;
	int panel_y = 56;
	int row_h = 42;
	int y;
	int index;
	int port;
	char line[64];

	wl_overlay_rect(0, 0, (int)width, (int)height, COLOR_DIM);
	wl_overlay_rect(panel_x, panel_y, panel_w, (int)height - 2 * panel_y, COLOR_PANEL);
	wl_overlay_frame(panel_x, panel_y, panel_w, (int)height - 2 * panel_y, 2, COLOR_FRAME);
	text_centered(width, panel_y + 20, 4, COLOR_TEXT, "HOME");
	y = panel_y + 80;
	if (home.state == WII_HOME_CONFIRM)
	{
		text_centered(width, y, 2, COLOR_WARNING, wii_home_confirm_text());
		y += 44;
		for (index = 0; index < WII_HOME_CHOICES; index++)
		{
			const char *label = wii_home_choice_label(index);

			if (index == home.selection)
			{
				wl_overlay_rect(panel_x + 16, y - 6, panel_w - 32, row_h - 6, COLOR_SELECTED);
				wl_overlay_text(panel_x + 28, y + 2, 3, COLOR_TEXT, ">");
			}
			text_centered(width, y + 2, 3, index == home.selection ? COLOR_TEXT : COLOR_GREY, label);
			y += row_h;
		}
	}
	else
	{
		for (index = 0; index < home.item_count; index++)
		{
			const char *label = wii_home_item_label(home.items[index]);

			if (index == home.selection)
			{
				wl_overlay_rect(panel_x + 16, y - 6, panel_w - 32, row_h - 6, COLOR_SELECTED);
				wl_overlay_text(panel_x + 28, y + 2, 3, COLOR_TEXT, ">");
			}
			text_centered(width, y + 2, 3, index == home.selection ? COLOR_TEXT : COLOR_GREY, label);
			y += row_h;
		}
	}

	/* the Remotes' batteries */
	y = (int)height - panel_y - 150;
	text_centered(width, y, 2, COLOR_GREY, "WII REMOTE BATTERIES");
	y += 28;
	for (port = 0; port < WII_HOME_PORTS; port++)
	{
		int gauge_x = panel_x + 150;
		float fraction = home.battery[port] / 100.0f;

		if (!home.present[port])
			continue;
		snprintf(line, sizeof(line), "REMOTE %d", port + 1);
		wl_overlay_text(panel_x + 24, y, 2, COLOR_TEXT, line);
		wl_overlay_gauge(gauge_x, y, 160, 14, fraction,
			home.battery[port] < WII_HOME_BATTERY_LOW ? COLOR_LOW : COLOR_GOOD, COLOR_GAUGE_BACK);
		snprintf(line, sizeof(line), "%d%%", home.battery[port]);
		wl_overlay_text(gauge_x + 172, y, 2, COLOR_TEXT, line);
		y += 22;
	}
	text_centered(width, (int)height - panel_y - 30, 2, COLOR_GREY,
		home.state == WII_HOME_CONFIRM ? "A: CHOOSE   B: BACK" : "A: CHOOSE   B OR HOME: RESUME");
}

static void draw_notices(uint32_t width)
{
	char text[256];
	unsigned length = wii_home_notice_text(&home, wl_platform_time_us(), text, sizeof(text));
	int box_w;
	int box_h;
	int y = 14;

	if (!length)
		return;
	/* (the last newline is not a line) */
	text[length - 1] = '\0';
	box_w = wl_overlay_text_width(2, text) + 32;
	box_h = wl_overlay_text_height(2, text) + 24;
	wl_overlay_rect(((int)width - box_w) / 2, y, box_w, box_h, COLOR_PANEL);
	wl_overlay_frame(((int)width - box_w) / 2, y, box_w, box_h, 2, COLOR_WARNING);
	wl_overlay_text(((int)width - box_w) / 2 + 16, y + 12, 2, COLOR_WARNING, text);
}

/* the reminder: a plain screen of text on black */
static void draw_strap(uint32_t width, uint32_t height)
{
	int y = (int)height / 5;
	int index;

	wl_overlay_rect(0, 0, (int)width, (int)height, 0x000000FFu);
	text_centered(width, y, 4, COLOR_WARNING, wii_strap_line(&strap, 0));
	y += 90;
	for (index = 1; index <= 4; index++)
	{
		const char *line = wii_strap_line(&strap, index);

		if (!line)
			continue;
		text_centered(width, y, 2, COLOR_TEXT, line);
		y += 36;
	}
	text_centered(width, (int)height - 90, 3,
		wii_strap_ready(&strap, wl_platform_time_us()) ? COLOR_GOOD : COLOR_GAUGE_BACK, wii_strap_line(&strap, 5));
}

static void home_draw(void *user, uint32_t width, uint32_t height)
{
	(void)user;
	wl_overlay_begin();
	if (strap_visible)
	{
		draw_strap(width, height);
		wl_overlay_end();
		return;
	}
	if (home.state != WII_HOME_CLOSED)
		draw_menu(width, height);
	draw_notices(width);
	wl_overlay_end();
}
