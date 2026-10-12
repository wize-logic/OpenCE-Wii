/*
TEST_INPUT.H

The scripted player for automated tests (debug.test_input): controller 1's
state made up from the time (test_input.c), on every port that reads one;
in a script, the other controllers' too.
*/

#ifndef __TEST_INPUT_H
#define __TEST_INPUT_H

#include "platform.h"

/* whether debug.test_input names a scripted player */
BOOL test_input_active(void);
/* whether it plays a script of steps ("script:<file>") */
BOOL test_input_scripted(void);
/* what the game shows, each frame, for the script: the tag names of the
first player's screen and of its focused widget ("" for none), the game's
tick in a game (else -1), the checkpoints saved since the start, and where
the first player's unit is (x, y, z, its facing's x and y, and 1 if it
rides, else 0; NULL for none) (port/linux/game/network_test.c) */
void test_input_observe(const char *screen, const char *focus, long tick, long checkpoints, const float *place);
void test_input_observe_menus_inhibited(int inhibited);
/* the scripted player's buttons and sticks, added to a gamepad's state;
milliseconds is any clock that counts them */
void test_input_gamepad(XINPUT_GAMEPAD *pad, unsigned long long milliseconds);
/* the same for controller port (0 to 3): a script's steps for that player
and its bot ("player", "N:" and "bot N <seed>" lines, test_input.c). The
first controller's read runs the script, and must come first in a frame */
void test_input_gamepad_port(int port, XINPUT_GAMEPAD *pad, unsigned long long milliseconds);
/* whether a script has controller port (1 to 3; the script's 2 to 4) appear
or go ("connect", "disconnect"): 1 there, 0 gone, -1 (and for any other port)
as the machine has it. The port's XGetDeviceChanges and XInputGetState
follow it */
int test_input_controller(int port);
/* the automated tests (port/linux/game/network_test.c): standing still, the
action button held from a second on */
void test_input_hold_action(int hold);

/* The Wii's verbs (home, remote, extension, battery; test_input.c): what a
script says of a Wii Remote replaces what the console reports, in the raw
input before the port reads it (port/wii/src/xinput_wii.c). connected is -1
as reported, 0 gone, 1 there; extension -1 as reported, else 0 none, 1
Nunchuk, 2 Classic Controller (WiiLibs' WlRemoteExtension); battery -1 as
reported, else a percent. The call is TRUE when the script has set any of
them for that port. */
struct test_input_remote
{
	int connected;
	int extension;
	int battery;
};
BOOL test_input_remote_override(int port, struct test_input_remote *remote);
/* the script's HOME press: TRUE for some reads of the controllers after the
step, as a button held for a moment is */
BOOL test_input_home_pressed(void);

#endif
