/*
WII_HOME_CONFIG.C

Halo's configuration of WiiLibs' HOME menu and strap reminder: see
wii_home_config.h. The words are WiiLibs' own defaults (the HOME menu's, the
notices' and the reminder's); the game adds the reset item and swaps the
Classic Controller's buttons.
*/

#include "wii_home_config.h"

WlHomeConfig wii_home_config(void)
{
	WlHomeConfig config = wl_home_config_default();

	/* (Reset to title is the game's own way to its main menu, main_goto_main_menu) */
	config.reset_label = "Reset to title";
	config.classic_swap = TRUE;
	return config;
}

WlStrapConfig wii_strap_config(void)
{
	WlStrapConfig config = wl_strap_config_default();

	config.classic_swap = TRUE;
	return config;
}

unsigned wii_home_pad_navigation(const XINPUT_GAMEPAD *pad)
{
	unsigned nav = 0;

	if (pad->wButtons & XINPUT_GAMEPAD_DPAD_UP)
		nav |= WL_HOME_NAV_UP;
	if (pad->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)
		nav |= WL_HOME_NAV_DOWN;
	if (pad->bAnalogButtons[XINPUT_GAMEPAD_A])
		nav |= WL_HOME_NAV_ACCEPT;
	if (pad->bAnalogButtons[XINPUT_GAMEPAD_B])
		nav |= WL_HOME_NAV_BACK;
	return nav;
}
