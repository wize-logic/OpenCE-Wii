/*
WII_HOME_GAME.C

WiiLibs' HOME menu, Wii notices and wrist strap reminder
(wl/platform/home_shell.h) joined to the game: see wii_home_game.h.

WiiLibs does the console's part: the sound fade (wl_platform_audio_fade, 300
ms out and back), the overlay it draws over the finished frame
(wl_gx_set_overlay: the Direct3D device on GX, d3d8_gx.c, is not involved, its
Present ends the frame with WiiLibs' wl_gx_end_frame, which calls the hook),
hooked only while there is something to draw so that a closed menu costs the
frame nothing. The controllers are looked at where the game reads them
(xinput_wii.c), a few comparisons a read.

This file gives the shell the game's hooks:
- The game is paused as its own pause menu pauses it, game_time_set_paused
  (the sound it pauses itself is not touched: the master fade silences the
  output instead).
- Reset to title is the game's own way to its main menu
  (main_goto_main_menu); Return to the loader is the menus' Quit
  (platform_request_quit).
- The world: a level is running, the game is paused already, and a player is
  on a Remote (asked of the game only for the Remotes that could be told they
  have nothing to move with).
- The reminder (display.wii_strap_reminder) is wanted if the setting is on,
  read at the first look.
The words and the Classic Controller's buttons are wii_home_config.c's.
*/

#include "platform.h"

#include <string.h>

#include "wl/platform/home_shell.h"
#include "wl/platform/platform.h"

#include "port_config.h"
#include "wii_home_config.h"
#include "wii_home_game.h"

/* (ui_widget.c, game_time.c, main.c, players.c, wii_platform.c) */
extern unsigned char main_menu_is_active(void);
extern unsigned char game_time_get_paused(void);
extern void game_time_set_paused(unsigned char paused);
extern unsigned char game_in_progress(void);
extern unsigned char game_time_initialized(void);
extern void main_goto_main_menu(void);
extern unsigned char local_player_exists(short controller_index);
void platform_request_quit(void);

static WlHomeShell shell;
static BOOL shell_started;

/* ---------- the shell's hooks */

static void hook_set_paused(void *user, bool paused)
{
	(void)user;
	game_time_set_paused(paused ? 1 : 0);
}

static void hook_reset_to_title(void *user)
{
	(void)user;
	main_goto_main_menu();
}

static void hook_leave_for_loader(void *user)
{
	(void)user;
	platform_request_quit();
}

static void hook_log(void *user, const char *text)
{
	(void)user;
	platform_log("%s", text);
}

static void hook_world(void *user, const WlRawInput *raw, WlHomeWorld *world)
{
	int port;

	(void)user;
	/* (the controllers are read from the game's start, before its time exists:
	game_in_progress and game_time_get_paused assert it does) */
	if (game_time_initialized())
	{
		world->in_play = game_in_progress() && !main_menu_is_active();
		world->paused = game_time_get_paused() ? true : false;
	}
	for (port = 0; world->in_play && port < WL_HOME_PORTS; port++)
	{
		if (raw->remotes[port].connected && raw->remotes[port].extension == WL_EXT_NONE)
			world->local_player[port] = local_player_exists((short)port) ? true : false;
	}
}

static bool hook_strap_enabled(void *user)
{
	(void)user;
	return config_boolean("display.wii_strap_reminder") ? true : false;
}

static const WlHomeShellOps shell_ops =
{
	NULL,
	hook_set_paused,
	hook_reset_to_title,
	hook_leave_for_loader,
	hook_log,
	hook_world,
	hook_strap_enabled
};

/* ---------- the poll */

void wii_home_game_poll(const WlRawInput *raw, uint64_t now_us, unsigned extra_nav, BOOL home_extra)
{
	if (!shell_started)
	{
		WlHomeConfig menu = wii_home_config();
		WlStrapConfig strap = wii_strap_config();

		shell_started = TRUE;
		wl_home_shell_init(&shell, &menu, &strap, &shell_ops);
	}
	wl_home_shell_poll(&shell, raw, now_us, extra_nav, home_extra ? true : false);
}

BOOL wii_home_game_open(void)
{
	return wl_home_shell_open(&shell) ? TRUE : FALSE;
}

BOOL wii_home_game_blocks_input(void)
{
	return wl_home_shell_blocks_input(&shell) ? TRUE : FALSE;
}
