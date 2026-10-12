/* host stand-in for port/linux/src/platform.h: what xinput_wii_map.c names
(tests/wii_input_check.c). The values are the XDK stand-ins' (port/include/xdk/xdk_xbox.h);
tools/wii_input_check.py compares them. */
#ifndef __HOST_INPUT_PLATFORM_H
#define __HOST_INPUT_PLATFORM_H

#include <stdint.h>

typedef int BOOL;
typedef unsigned short WORD;
#define TRUE 1
#define FALSE 0

#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080

#define XINPUT_GAMEPAD_A 0
#define XINPUT_GAMEPAD_B 1
#define XINPUT_GAMEPAD_X 2
#define XINPUT_GAMEPAD_Y 3
#define XINPUT_GAMEPAD_BLACK 4
#define XINPUT_GAMEPAD_WHITE 5
#define XINPUT_GAMEPAD_LEFT_TRIGGER 6
#define XINPUT_GAMEPAD_RIGHT_TRIGGER 7

typedef struct _XINPUT_GAMEPAD
{
	unsigned short wButtons;
	unsigned char bAnalogButtons[8];
	short sThumbLX;
	short sThumbLY;
	short sThumbRX;
	short sThumbRY;
} XINPUT_GAMEPAD;

#endif
