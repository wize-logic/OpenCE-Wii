/*
WII_HOME_GAME.H

WiiLibs' HOME menu and wrist strap reminder (wl/platform/home_shell.h) joined
to the game and the console: the game's pause and Reset to title, WiiLibs'
sound fade and exit, and the overlay WiiLibs draws over the finished frame
(wl_gx_set_overlay) while a menu, notice or reminder is up. Called from
xinput_wii.c, which reads the controllers.
*/

#ifndef __WII_HOME_GAME_H
#define __WII_HOME_GAME_H

#include "platform.h"

#include "wl/platform/platform.h"

/* each time the controllers are read: raw as WiiLibs gave them (and the
script's changes to them), the navigation of the scripted controller, a
HOME press that is not on a controller */
void wii_home_game_poll(const WlRawInput *raw, uint64_t now_us, unsigned extra_nav, BOOL home_extra);

/* the menu or the reminder is up (any thread): no rumble */
BOOL wii_home_game_open(void);
/* the game sees no controller: the menu or the reminder is up, or a button that closed it is still down */
BOOL wii_home_game_blocks_input(void);

#endif
