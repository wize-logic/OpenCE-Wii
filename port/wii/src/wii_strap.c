/*
WII_STRAP.C

The wrist strap reminder's state machine (wii_strap.h).
*/

#include "wii_strap.h"

#include <string.h>

#include "wii_home.h"

void wii_strap_init(struct wii_strap *strap, BOOL enabled)
{
	memset(strap, 0, sizeof(*strap));
	strap->state = enabled ? WII_STRAP_WAITING : WII_STRAP_OFF;
}

static BOOL any_nunchuk(const WlRawInput *raw)
{
	int port;

	for (port = 0; port < WII_HOME_PORTS; port++)
	{
		if (raw->remotes[port].connected && raw->remotes[port].extension == WL_EXT_NUNCHUK)
			return TRUE;
	}
	return FALSE;
}

BOOL wii_strap_step(struct wii_strap *strap, const WlRawInput *raw, unsigned extra_nav, uint64_t now_us)
{
	BOOL accept = ((wii_home_navigation(raw) | extra_nav) & WII_NAV_ACCEPT) ? TRUE : FALSE;
	BOOL pressed = accept && !strap->accept_was;

	strap->accept_was = accept;
	switch (strap->state)
	{
	case WII_STRAP_WAITING:
		strap->state = WII_STRAP_UP;
		strap->shown_us = now_us;
		strap->nunchuk = any_nunchuk(raw);
		break;
	case WII_STRAP_UP:
		strap->nunchuk = any_nunchuk(raw);
		/* (a fresh press: an A held from before, or through the first second, does not go on) */
		if (pressed && wii_strap_ready(strap, now_us))
		{
			strap->state = WII_STRAP_SWALLOWING;
			strap->gone_us = now_us;
		}
		break;
	case WII_STRAP_SWALLOWING:
		if (!accept || now_us - strap->gone_us >= WII_STRAP_SWALLOW_US)
			strap->state = WII_STRAP_DONE;
		break;
	default:
		break;
	}
	return wii_strap_up(strap);
}

BOOL wii_strap_up(const struct wii_strap *strap)
{
	return strap->state == WII_STRAP_UP;
}

BOOL wii_strap_blocks_input(const struct wii_strap *strap)
{
	return strap->state == WII_STRAP_UP || strap->state == WII_STRAP_SWALLOWING;
}

BOOL wii_strap_ready(const struct wii_strap *strap, uint64_t now_us)
{
	return strap->state == WII_STRAP_UP && now_us - strap->shown_us >= WII_STRAP_IGNORE_US;
}

const char *wii_strap_line(const struct wii_strap *strap, int index)
{
	switch (index)
	{
	case 0:
		return "PLEASE USE THE WRIST STRAP";
	case 1:
		return "FASTEN IT BEFORE YOU PLAY, SO THE REMOTE";
	case 2:
		return "STAYS IN YOUR HAND IF YOU SWING IT.";
	case 3:
		return strap->nunchuk ? "HOLD THE NUNCHUK FIRMLY TOO, AND KEEP ITS CABLE CLEAR." : NULL;
	case 4:
		return "MAKE ROOM AROUND YOU, AND STAND WELL BACK FROM THE SCREEN.";
	case 5:
		return "A: CONTINUE";
	default:
		return NULL;
	}
}
