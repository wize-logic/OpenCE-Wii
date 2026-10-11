/*
TEST_INPUT.H

The scripted player for automated tests (debug.test_input): controller 1's
state made up from the time (test_input.c), on every port that reads one.
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
/* the scripted player's buttons and sticks, added to a gamepad's state;
milliseconds is any clock that counts them */
void test_input_gamepad(XINPUT_GAMEPAD *pad, unsigned long long milliseconds);
/* the automated tests (port/linux/game/network_test.c): standing still, the
action button held from a second on */
void test_input_hold_action(int hold);

#endif
