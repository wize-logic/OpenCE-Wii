/*
XINPUT_WII.C

The Xbox controllers (XInput) on the Wii's, in place of
port/linux/src/xinput_sdl.c, through WiiLibs' raw input
(wl_platform_input_poll). Port N is the Nth Wii Remote, with its Classic
Controller or Nunchuk, or the Nth GameCube controller when no Remote is
connected there. The Xbox's controls are Halo's defaults: A jump, B melee, X
action and reload, Y switch weapons, the left trigger a grenade, the right
fire, Black switch grenades, White the flashlight, a click of the left stick
crouch and of the right zoom.

- Classic Controller: its buttons where the Xbox controller's are (b is A, a
  B, y X, x Y), its triggers and sticks the same; ZL Black, ZR White; the
  D-pad, and down and up are also the stick clicks; - Back, + Start.
- GameCube controller: A, B, X, Y, the triggers and sticks the same; Z
  Black, Start; the D-pad, down and up also the stick clicks, left Back and
  right White.
- Wii Remote and Nunchuk: the Nunchuk's stick moves; B fires, Z throws a
  grenade, A jumps, C crouches, 1 melees, 2 zooms; the D-pad up switches
  weapons, down is the action and reload, left Black, right White; - Back,
  + Start. The pointer aims (halo_linux_mouse_look): the view turns while it
  points outside a box at the screen's centre, faster nearer the edge.
*/

#include "platform.h"

#include <math.h>
#include <string.h>

#include "wl/platform/platform.h"

#include "test_input.h"

#define PORT_COUNT 4
/* the pointer's box in which the view does not turn (a fraction of the
screen's half width and height), and the fastest turn, at the edge */
#define POINTER_DEAD_ZONE 0.35f
#define POINTER_TURN_RADIANS_PER_SECOND 3.0f
#define TRIGGER_THRESHOLD 0x30

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

struct controller
{
	BOOL open;
	DWORD packet_number;
	XINPUT_GAMEPAD previous;
};

static struct controller controllers[PORT_COUNT];
static DWORD reported_gamepads;
static WlRawInput input;
static uint64_t input_polled_us;
static uint64_t look_us[PORT_COUNT];

/* the controllers once a frame at most (every port reads them each tick) */
static const WlRawInput *input_read(void)
{
	uint64_t now = wl_platform_time_us();

	if (!input_polled_us || now - input_polled_us > 4000)
	{
		wl_platform_input_poll(&input);
		input_polled_us = now;
	}
	return &input;
}

/* (the scripted player of the automated tests is controller 1:
port/linux/src/test_input.c) */
static BOOL port_connected(const WlRawInput *raw, int port)
{
	return raw->remotes[port].connected || raw->pads[port].connected || (port == 0 && test_input_active());
}

static short stick(int8_t value)
{
	long scaled = (long)value * 32767 / 127;

	return (short)(scaled < -32768 ? -32768 : scaled > 32767 ? 32767 : scaled);
}

static void button(XINPUT_GAMEPAD *pad, BOOL held, int analog)
{
	if (held)
		pad->bAnalogButtons[analog] = 0xFF;
}

static void digital(XINPUT_GAMEPAD *pad, BOOL held, WORD flag)
{
	if (held)
		pad->wButtons |= flag;
}

static void classic_state(const WlRawRemote *remote, XINPUT_GAMEPAD *pad)
{
	uint32_t held = remote->buttons;

	button(pad, held & WL_CC_B, XINPUT_GAMEPAD_A);
	button(pad, held & WL_CC_A, XINPUT_GAMEPAD_B);
	button(pad, held & WL_CC_Y, XINPUT_GAMEPAD_X);
	button(pad, held & WL_CC_X, XINPUT_GAMEPAD_Y);
	button(pad, held & WL_CC_ZL, XINPUT_GAMEPAD_BLACK);
	button(pad, held & WL_CC_ZR, XINPUT_GAMEPAD_WHITE);
	pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = (held & WL_CC_L) ? 0xFF : remote->trigger[0];
	pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = (held & WL_CC_R) ? 0xFF : remote->trigger[1];
	digital(pad, held & WL_CC_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_RIGHT_THUMB);
	digital(pad, held & WL_CC_DOWN, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_LEFT_THUMB);
	digital(pad, held & WL_CC_LEFT, XINPUT_GAMEPAD_DPAD_LEFT);
	digital(pad, held & WL_CC_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT);
	digital(pad, held & WL_CC_PLUS, XINPUT_GAMEPAD_START);
	digital(pad, held & WL_CC_MINUS, XINPUT_GAMEPAD_BACK);
	pad->sThumbLX = stick(remote->stick[0][0]);
	pad->sThumbLY = stick(remote->stick[0][1]);
	pad->sThumbRX = stick(remote->stick[1][0]);
	pad->sThumbRY = stick(remote->stick[1][1]);
}

static void nunchuk_state(const WlRawRemote *remote, XINPUT_GAMEPAD *pad)
{
	uint32_t held = remote->buttons;

	button(pad, held & WL_WM_A, XINPUT_GAMEPAD_A);
	button(pad, held & WL_WM_1, XINPUT_GAMEPAD_B);
	button(pad, held & WL_WM_DOWN, XINPUT_GAMEPAD_X);
	button(pad, held & WL_WM_UP, XINPUT_GAMEPAD_Y);
	button(pad, held & WL_WM_LEFT, XINPUT_GAMEPAD_BLACK);
	button(pad, held & WL_WM_RIGHT, XINPUT_GAMEPAD_WHITE);
	button(pad, held & WL_NC_Z, XINPUT_GAMEPAD_LEFT_TRIGGER);
	button(pad, held & WL_WM_B, XINPUT_GAMEPAD_RIGHT_TRIGGER);
	digital(pad, held & WL_NC_C, XINPUT_GAMEPAD_LEFT_THUMB);
	digital(pad, held & WL_WM_2, XINPUT_GAMEPAD_RIGHT_THUMB);
	digital(pad, held & WL_WM_PLUS, XINPUT_GAMEPAD_START);
	digital(pad, held & WL_WM_MINUS, XINPUT_GAMEPAD_BACK);
	/* (the menus are driven with the D-pad as well as the stick) */
	digital(pad, held & WL_WM_UP, XINPUT_GAMEPAD_DPAD_UP);
	digital(pad, held & WL_WM_DOWN, XINPUT_GAMEPAD_DPAD_DOWN);
	digital(pad, held & WL_WM_LEFT, XINPUT_GAMEPAD_DPAD_LEFT);
	digital(pad, held & WL_WM_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT);
	if (remote->extension == WL_EXT_NUNCHUK)
	{
		pad->sThumbLX = stick(remote->stick[0][0]);
		pad->sThumbLY = stick(remote->stick[0][1]);
	}
}

static void gamecube_state(const WlRawPad *gamecube, XINPUT_GAMEPAD *pad)
{
	uint16_t held = gamecube->buttons;

	button(pad, held & WL_PAD_A, XINPUT_GAMEPAD_A);
	button(pad, held & WL_PAD_B, XINPUT_GAMEPAD_B);
	button(pad, held & WL_PAD_X, XINPUT_GAMEPAD_X);
	button(pad, held & WL_PAD_Y, XINPUT_GAMEPAD_Y);
	button(pad, held & WL_PAD_Z, XINPUT_GAMEPAD_BLACK);
	button(pad, held & WL_PAD_RIGHT, XINPUT_GAMEPAD_WHITE);
	pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = (held & WL_PAD_L) ? 0xFF : gamecube->trigger[0];
	pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = (held & WL_PAD_R) ? 0xFF : gamecube->trigger[1];
	digital(pad, held & WL_PAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_RIGHT_THUMB);
	digital(pad, held & WL_PAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_LEFT_THUMB);
	digital(pad, held & WL_PAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_BACK);
	digital(pad, held & WL_PAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT);
	digital(pad, held & WL_PAD_START, XINPUT_GAMEPAD_START);
	pad->sThumbLX = stick(gamecube->stick[0][0]);
	pad->sThumbLY = stick(gamecube->stick[0][1]);
	pad->sThumbRX = stick(gamecube->stick[1][0]);
	pad->sThumbRY = stick(gamecube->stick[1][1]);
}

/* ---------- XAPI */

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
	wl_platform_input_init();
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	*insertions = 0;
	*removals = 0;
	if (device_type == XDEVICE_TYPE_GAMEPAD)
	{
		const WlRawInput *raw = input_read();
		DWORD connected = 0;
		int port;

		for (port = 0; port < PORT_COUNT; port++)
		{
			if (port_connected(raw, port))
				connected |= 1UL << port;
		}
		*insertions = connected & ~reported_gamepads;
		*removals = reported_gamepads & ~connected;
		reported_gamepads = connected;
	}
	return *insertions || *removals;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)slot;
	(void)polling_parameters;
	if (device_type == XDEVICE_TYPE_GAMEPAD && port < PORT_COUNT)
	{
		memset(&controllers[port], 0, sizeof(controllers[port]));
		controllers[port].open = TRUE;
		return (HANDLE)&controllers[port];
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	struct controller *controller = (struct controller *)device;

	if (controller)
		controller->open = FALSE;
}

static int controller_port(HANDLE device)
{
	int port;

	for (port = 0; port < PORT_COUNT; port++)
	{
		if (device == (HANDLE)&controllers[port] && controllers[port].open)
			return port;
	}
	return -1;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	int port = controller_port(device);
	const WlRawInput *raw;

	memset(state, 0, sizeof(*state));
	if (port < 0)
		return ERROR_DEVICE_NOT_CONNECTED;
	raw = input_read();
	if (raw->remotes[port].connected)
	{
		if (raw->remotes[port].extension == WL_EXT_CLASSIC)
			classic_state(&raw->remotes[port], &state->Gamepad);
		else
			nunchuk_state(&raw->remotes[port], &state->Gamepad);
	}
	else if (raw->pads[port].connected)
	{
		gamecube_state(&raw->pads[port], &state->Gamepad);
	}
	else if (!(port == 0 && test_input_active()))
	{
		return ERROR_DEVICE_NOT_CONNECTED;
	}
	if (port == 0)
		test_input_gamepad(&state->Gamepad, wl_platform_time_us() / 1000);
	if (memcmp(&state->Gamepad, &controllers[port].previous, sizeof(state->Gamepad)))
	{
		controllers[port].packet_number++;
		controllers[port].previous = state->Gamepad;
	}
	state->dwPacketNumber = controllers[port].packet_number;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	int port = controller_port(device);
	BOOL on;

	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	feedback->Header.dwStatus = ERROR_SUCCESS;
	if (port < 0)
		return ERROR_DEVICE_NOT_CONNECTED;
	/* (the Wii's motors are on or off) */
	on = feedback->Rumble.wLeftMotorSpeed > 0x4000 || feedback->Rumble.wRightMotorSpeed > 0x4000;
	if (input.remotes[port].connected)
		wl_platform_rumble((uint8_t)port, on);
	else if (input.pads[port].connected)
		wl_platform_pad_rumble((uint8_t)port, on);
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	(void)parameters;
	return ERROR_SUCCESS;
}

/* (no debug keyboard: the console and typed text come from the controller's
virtual keyboard) */
DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	memset(keystroke, 0, sizeof(*keystroke));
	return ERROR_HANDLE_EOF;
}

/* ---------- the pointer's aim, through the mouse's hooks (source/game/player_control.c) */

static BOOL pointer_aiming(short gamepad_index, float *x, float *y)
{
	const WlRawRemote *remote;

	if (gamepad_index < 0 || gamepad_index >= PORT_COUNT)
		return FALSE;
	remote = &input_read()->remotes[gamepad_index];
	if (!remote->connected || remote->extension == WL_EXT_CLASSIC || !remote->ir_valid)
		return FALSE;
	/* -1 to 1 across the 640 by 480 screen */
	*x = (remote->ir_x - 320) / 320.0f;
	*y = (remote->ir_y - 240) / 240.0f;
	return TRUE;
}

/* no aim magnetism while the pointer aims, as for the mouse */
int halo_linux_mouse_aiming(short gamepad_index)
{
	float x, y;

	return pointer_aiming(gamepad_index, &x, &y);
}

static float turn(float offset)
{
	float magnitude = fabsf(offset);

	if (magnitude <= POINTER_DEAD_ZONE)
		return 0.0f;
	if (magnitude > 1.0f)
		magnitude = 1.0f;
	magnitude = (magnitude - POINTER_DEAD_ZONE) / (1.0f - POINTER_DEAD_ZONE);
	return (offset < 0.0f ? -1.0f : 1.0f) * magnitude * magnitude * POINTER_TURN_RADIANS_PER_SECOND;
}

int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	uint64_t now = wl_platform_time_us();
	float x, y, seconds;

	*yaw = 0.0f;
	*pitch = 0.0f;
	if (!pointer_aiming(gamepad_index, &x, &y))
	{
		if (gamepad_index >= 0 && gamepad_index < PORT_COUNT)
			look_us[gamepad_index] = 0;
		return FALSE;
	}
	seconds = look_us[gamepad_index] ? (now - look_us[gamepad_index]) / 1000000.0f : 0.0f;
	if (seconds > 0.1f)
		seconds = 0.1f;
	look_us[gamepad_index] = now;
	/* right turns the view right (yaw down), up looks up */
	*yaw = -turn(x) * seconds;
	*pitch = -turn(y) * seconds;
	return TRUE;
}

/* ---------- the desktop's keyboard and mouse controls, which the Wii has none of */

unsigned long halo_keyboard_actions(short controller_index)
{
	(void)controller_index;
	return 0;
}

int halo_push_to_talk_held(void)
{
	return 0;
}

void halo_input_name(int input_code, char *name, unsigned int size)
{
	(void)input_code;
	if (size)
		name[0] = '\0';
}

void platform_text_typing(int typing)
{
	(void)typing;
}

void platform_text_field(int typing, int password)
{
	(void)typing;
	(void)password;
}
