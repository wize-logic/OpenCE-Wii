/*
WII_HOME.C

The HOME button's menu and the Wii's notices: see wii_home.h.

HOME (a Wii Remote's, or a Classic Controller's own) opens a small menu
over the game: Resume, Reset to title and Return to the loader (asked about
first: unsaved progress is lost). While it is up the game is paused as its
own pause menu pauses it, the sound fades out, and the game sees no
controller; HOME again, or B, closes it. The GameCube controller has no HOME
button. The menu is steered by whichever controller has a hand on it: the
D-pad or the stick, A to choose, B to go back.

The notices need no menu: a Remote with no Nunchuk or Classic Controller on
a player in play cannot move him, and a Remote's battery has fallen low.
Each is given once (the first again only when the Remote has had something
else to move with, the second when the battery has been good again) and
stays on the screen a few seconds.
*/

#include "wii_home.h"

#include <stdio.h>
#include <string.h>

/* ---------- what the controllers give the menu */

static unsigned remote_navigation(const WlRawRemote *remote)
{
	uint32_t held = remote->buttons;
	unsigned nav = 0;
	BOOL classic = remote->extension == WL_EXT_CLASSIC;

	if (held & WL_WM_UP)
		nav |= WII_NAV_UP;
	if (held & WL_WM_DOWN)
		nav |= WII_NAV_DOWN;
	if (held & WL_WM_A)
		nav |= WII_NAV_ACCEPT;
	if (held & (WL_WM_B | WL_WM_1))
		nav |= WII_NAV_BACK;
	if (classic)
	{
		/* (its lower button, b, accepts, and the right one, a, goes back,
		as the game's menus have them: xinput_wii_map.c) */
		if (held & WL_CC_UP)
			nav |= WII_NAV_UP;
		if (held & WL_CC_DOWN)
			nav |= WII_NAV_DOWN;
		if (held & WL_CC_B)
			nav |= WII_NAV_ACCEPT;
		if (held & WL_CC_A)
			nav |= WII_NAV_BACK;
	}
	if (classic || remote->extension == WL_EXT_NUNCHUK)
	{
		if (remote->stick[0][1] > WII_HOME_STICK)
			nav |= WII_NAV_UP;
		else if (remote->stick[0][1] < -WII_HOME_STICK)
			nav |= WII_NAV_DOWN;
	}
	return nav;
}

unsigned wii_home_navigation(const WlRawInput *raw)
{
	unsigned nav = 0;
	int port;

	for (port = 0; port < WII_HOME_PORTS; port++)
	{
		if (raw->remotes[port].connected)
			nav |= remote_navigation(&raw->remotes[port]);
		if (raw->pads[port].connected)
		{
			const WlRawPad *pad = &raw->pads[port];

			if (pad->buttons & WL_PAD_UP)
				nav |= WII_NAV_UP;
			if (pad->buttons & WL_PAD_DOWN)
				nav |= WII_NAV_DOWN;
			if (pad->buttons & WL_PAD_A)
				nav |= WII_NAV_ACCEPT;
			if (pad->buttons & WL_PAD_B)
				nav |= WII_NAV_BACK;
			if (pad->stick[0][1] > WII_HOME_STICK)
				nav |= WII_NAV_UP;
			else if (pad->stick[0][1] < -WII_HOME_STICK)
				nav |= WII_NAV_DOWN;
		}
	}
	return nav;
}

unsigned wii_home_pad_navigation(const XINPUT_GAMEPAD *pad)
{
	unsigned nav = 0;

	if (pad->wButtons & XINPUT_GAMEPAD_DPAD_UP)
		nav |= WII_NAV_UP;
	if (pad->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)
		nav |= WII_NAV_DOWN;
	if (pad->bAnalogButtons[XINPUT_GAMEPAD_A])
		nav |= WII_NAV_ACCEPT;
	if (pad->bAnalogButtons[XINPUT_GAMEPAD_B])
		nav |= WII_NAV_BACK;
	return nav;
}

BOOL wii_home_pressed(const WlRawInput *raw)
{
	int port;

	for (port = 0; port < WII_HOME_PORTS; port++)
	{
		const WlRawRemote *remote = &raw->remotes[port];

		if (remote->connected &&
			((remote->buttons & WL_WM_HOME) || (remote->extension == WL_EXT_CLASSIC && (remote->buttons & WL_CC_HOME))))
			return TRUE;
	}
	return FALSE;
}

/* any button held on any controller */
static BOOL buttons_held(const WlRawInput *raw, unsigned extra_nav)
{
	int port;

	if (extra_nav)
		return TRUE;
	for (port = 0; port < WII_HOME_PORTS; port++)
	{
		if ((raw->remotes[port].connected && raw->remotes[port].buttons) ||
			(raw->pads[port].connected && raw->pads[port].buttons))
			return TRUE;
	}
	return FALSE;
}

/* ---------- the machine */

void wii_home_init(struct wii_home *home)
{
	memset(home, 0, sizeof(*home));
}

BOOL wii_home_open(const struct wii_home *home)
{
	return home->state != WII_HOME_CLOSED;
}

BOOL wii_home_blocks_input(const struct wii_home *home)
{
	return home->state != WII_HOME_CLOSED || home->swallowing;
}

const char *wii_home_item_label(int item)
{
	switch (item)
	{
	case WII_HOME_RESUME:
		return "Resume";
	case WII_HOME_RESET:
		return "Reset to title";
	case WII_HOME_LOADER:
		return "Return to the loader";
	}
	return "";
}

const char *wii_home_choice_label(int choice)
{
	return choice == WII_HOME_LEAVE ? "Return to the loader" : "Cancel";
}

const char *wii_home_confirm_text(void)
{
	return "Unsaved progress will be lost";
}

static void say(const struct wii_home_ops *ops, const char *text)
{
	if (ops->log)
		ops->log(ops->user, text);
}

static void fade(const struct wii_home_ops *ops, float gain)
{
	if (ops->audio_fade)
		ops->audio_fade(ops->user, gain, WII_HOME_FADE_MS);
}

static BOOL notice_showing(const struct wii_home *home, uint64_t now_us)
{
	int kind, port;

	for (kind = 0; kind < WII_NOTICES; kind++)
	{
		for (port = 0; port < WII_HOME_PORTS; port++)
		{
			if (home->notice_until_us[kind][port] > now_us)
				return TRUE;
		}
	}
	return FALSE;
}

static void open_menu(struct wii_home *home, const struct wii_home_world *world, uint64_t now_us,
	const struct wii_home_ops *ops)
{
	char text[96];
	int count = 0;

	home->items[count++] = WII_HOME_RESUME;
	if (world->in_play)
		home->items[count++] = WII_HOME_RESET;
	home->items[count++] = WII_HOME_LOADER;
	home->item_count = count;
	home->selection = 0;
	home->state = WII_HOME_MENU;
	home->swallowing = FALSE;
	/* (the game's own pause is left as it is: only a pause made here is undone) */
	home->we_paused = world->in_play && !world->game_paused;
	if (home->we_paused && ops->set_paused)
		ops->set_paused(ops->user, TRUE);
	fade(ops, 0.0f);
	snprintf(text, sizeof(text), "home: menu opened (%s)",
		!world->in_play ? "no level running" : world->game_paused ? "the game was paused already" : "game paused");
	say(ops, text);
	(void)now_us;
}

/* the menu away: the game running again if the menu stopped it, the sound
back, and the buttons that closed it kept from the game until let go */
static void close_menu(struct wii_home *home, uint64_t now_us, const struct wii_home_ops *ops, BOOL resume_game)
{
	if (home->we_paused && resume_game && ops->set_paused)
		ops->set_paused(ops->user, FALSE);
	home->we_paused = FALSE;
	fade(ops, 1.0f);
	home->state = WII_HOME_CLOSED;
	home->swallowing = TRUE;
	home->swallow_since_us = now_us;
	say(ops, "home: menu closed");
}

static void choose(struct wii_home *home, uint64_t now_us, const struct wii_home_ops *ops)
{
	if (home->state == WII_HOME_CONFIRM)
	{
		if (home->selection == WII_HOME_LEAVE)
		{
			say(ops, "home: returning to the loader");
			/* (the game stays as it is: the console leaves) */
			if (ops->leave_for_loader)
				ops->leave_for_loader(ops->user);
			close_menu(home, now_us, ops, FALSE);
		}
		else
		{
			home->state = WII_HOME_MENU;
			home->selection = home->item_count - 1;
			say(ops, "home: staying");
		}
		return;
	}
	switch (home->items[home->selection])
	{
	case WII_HOME_RESUME:
		close_menu(home, now_us, ops, TRUE);
		break;
	case WII_HOME_RESET:
		/* (the game's time running again before it goes to its menu, which
		sets its own) */
		close_menu(home, now_us, ops, TRUE);
		say(ops, "home: reset to the title");
		if (ops->reset_to_title)
			ops->reset_to_title(ops->user);
		break;
	case WII_HOME_LOADER:
		home->state = WII_HOME_CONFIRM;
		home->selection = WII_HOME_CANCEL;
		say(ops, "home: asking before the loader");
		break;
	}
}

static void notice(struct wii_home *home, int kind, int port, uint64_t now_us, const struct wii_home_ops *ops)
{
	char text[96];

	home->notice_until_us[kind][port] = now_us + WII_HOME_NOTICE_US;
	home->notice_percent[port] = home->battery[port];
	if (kind == WII_NOTICE_BATTERY)
		snprintf(text, sizeof(text), "home: notice: Wii Remote %d battery low (%d%%)", port + 1, home->notice_percent[port]);
	else
		snprintf(text, sizeof(text), "home: notice: Wii Remote %d has no Nunchuk or Classic Controller", port + 1);
	say(ops, text);
}

/* the notices that wait on time */
static void notices(struct wii_home *home, const WlRawInput *raw, const struct wii_home_world *world, uint64_t now_us,
	const struct wii_home_ops *ops)
{
	BOOL battery_check = now_us >= home->next_battery_check_us;
	int port;

	if (battery_check)
		home->next_battery_check_us = now_us + WII_HOME_BATTERY_CHECK_US;
	for (port = 0; port < WII_HOME_PORTS; port++)
	{
		const WlRawRemote *remote = &raw->remotes[port];

		home->present[port] = remote->connected;
		home->extension[port] = remote->extension;
		home->battery[port] = remote->battery;
		if (!remote->connected)
		{
			/* (a Remote that comes back is read afresh: the battery it
			first reports may be 0 before its first status report) */
			home->battery_known[port] = FALSE;
			home->no_extension_since_us[port] = 0;
			home->no_extension_told[port] = FALSE;
			continue;
		}
		if (battery_check)
		{
			if (remote->battery > 0)
				home->battery_known[port] = TRUE;
			if (home->battery_known[port])
			{
				if (remote->battery >= WII_HOME_BATTERY_GOOD)
				{
					home->battery_told[port] = FALSE;
				}
				else if (remote->battery < WII_HOME_BATTERY_LOW && !home->battery_told[port])
				{
					home->battery_told[port] = TRUE;
					notice(home, WII_NOTICE_BATTERY, port, now_us, ops);
				}
			}
		}
		/* a Remote alone moves nobody: in play, with a player on it, and
		for a while (a Nunchuk plugged in is read as none for a moment) */
		if (world->in_play && world->local_player[port] && remote->extension == WL_EXT_NONE)
		{
			if (!home->no_extension_since_us[port])
				home->no_extension_since_us[port] = now_us ? now_us : 1;
			else if (!home->no_extension_told[port] && now_us - home->no_extension_since_us[port] >= WII_HOME_NO_EXTENSION_US)
			{
				home->no_extension_told[port] = TRUE;
				notice(home, WII_NOTICE_NO_EXTENSION, port, now_us, ops);
			}
		}
		else
		{
			home->no_extension_since_us[port] = 0;
			home->no_extension_told[port] = FALSE;
		}
	}
}

void wii_home_step(struct wii_home *home, const WlRawInput *raw, unsigned extra_nav, BOOL home_extra,
	const struct wii_home_world *world, uint64_t now_us, const struct wii_home_ops *ops)
{
	unsigned nav = wii_home_navigation(raw) | extra_nav;
	unsigned pressed = nav & ~home->previous_nav;
	BOOL home_down = wii_home_pressed(raw) || home_extra;
	BOOL home_edge = home_down && !home->previous_home;
	BOOL visible;

	home->previous_nav = nav;
	home->previous_home = home_down;

	if (home->state == WII_HOME_CLOSED)
	{
		if (home_edge)
		{
			open_menu(home, world, now_us, ops);
		}
		else if (home->swallowing && (!buttons_held(raw, extra_nav) ||
			now_us - home->swallow_since_us >= WII_HOME_SWALLOW_US))
		{
			home->swallowing = FALSE;
		}
	}
	else if (home_edge)
	{
		close_menu(home, now_us, ops, TRUE);
	}
	else if (pressed & WII_NAV_BACK)
	{
		if (home->state == WII_HOME_CONFIRM)
		{
			home->state = WII_HOME_MENU;
			home->selection = home->item_count - 1;
		}
		else
		{
			close_menu(home, now_us, ops, TRUE);
		}
	}
	else if (pressed & (WII_NAV_UP | WII_NAV_DOWN))
	{
		int count = home->state == WII_HOME_CONFIRM ? WII_HOME_CHOICES : home->item_count;

		home->selection = (home->selection + (pressed & WII_NAV_UP ? count - 1 : 1)) % count;
	}
	else if (pressed & WII_NAV_ACCEPT)
	{
		choose(home, now_us, ops);
	}

	notices(home, raw, world, now_us, ops);

	visible = home->state != WII_HOME_CLOSED || notice_showing(home, now_us);
	if (visible != home->shown)
	{
		home->shown = visible;
		if (ops->visible)
			ops->visible(ops->user, visible);
	}
}

unsigned wii_home_notice_text(const struct wii_home *home, uint64_t now_us, char *text, unsigned size)
{
	unsigned length = 0;
	int port;

	if (size)
		text[0] = '\0';
	for (port = 0; port < WII_HOME_PORTS; port++)
	{
		char line[128];
		int written;

		if (home->notice_until_us[WII_NOTICE_NO_EXTENSION][port] > now_us)
		{
			written = snprintf(line, sizeof(line), "Wii Remote %d: Connect a Nunchuk or Classic Controller to move\n",
				port + 1);
			if (written > 0 && length + (unsigned)written < size)
			{
				memcpy(text + length, line, (unsigned)written + 1);
				length += (unsigned)written;
			}
		}
		if (home->notice_until_us[WII_NOTICE_BATTERY][port] > now_us)
		{
			written = snprintf(line, sizeof(line), "Wii Remote %d: Battery low (%d%%)\n", port + 1,
				home->notice_percent[port]);
			if (written > 0 && length + (unsigned)written < size)
			{
				memcpy(text + length, line, (unsigned)written + 1);
				length += (unsigned)written;
			}
		}
	}
	return length;
}
