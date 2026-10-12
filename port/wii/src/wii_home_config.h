/*
WII_HOME_CONFIG.H

What Halo asks of WiiLibs' HOME menu and wrist strap reminder
(wl/platform/home_menu.h, strap_screen.h, home_shell.h): the menu with a
"Reset to title" item, the Classic Controller's buttons as the game's menus
have them (its lower button, b, accepts and the right one, a, goes back:
xinput_wii_map.c), the words of the reminder. Apart from the game so that
tools/wii_input_check.py checks them on the host. wii_home_game.c joins the
menu to the game.
*/

#ifndef __WII_HOME_CONFIG_H
#define __WII_HOME_CONFIG_H

#include "platform.h"

#include "wl/platform/home_menu.h"
#include "wl/platform/strap_screen.h"

WlHomeConfig wii_home_config(void);
WlStrapConfig wii_strap_config(void);

/* what the scripted controller's gamepad gives the menu (test_input.c), as
WL_HOME_NAV_ bits */
unsigned wii_home_pad_navigation(const XINPUT_GAMEPAD *pad);

#endif
