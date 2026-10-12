/*
XINPUT_WII.C

The Xbox controllers (XInput) on the Wii's, in place of
port/linux/src/xinput_sdl.c, through WiiLibs' raw input
(wl_platform_input_poll). Port N is the Nth Wii Remote, with its Classic
Controller or Nunchuk, or the Nth GameCube controller when no Remote is
connected there. The buttons are mapped in xinput_wii_map.c (and listed in
port/wii/README.md, "The controllers"); this file is the XAPI around them,
the pointer's aim and the desktop's controls a Wii has none of.

The Wii Remote's pointer aims (halo_linux_mouse_look): the view turns while
it points outside a box at the screen's centre, faster nearer the edge, at the
rates of the player's look sensitivity. It keeps turning for a moment when the
sensor bar is lost.
*/

#include "platform.h"

#include <string.h>

#include "wl/platform/input_shape.h"
#include "wl/platform/platform.h"

#include "port_config.h"
#include "test_input.h"
#include "xinput_wii_map.h"

#define PORT_COUNT 4

/* (player_control.c: degrees per second, the profile's look sensitivity) */
extern float player_look_yaw_rate[];
extern float player_look_pitch_rate[];
/* (ui_widget.c) whether any menu is up */
extern unsigned char ui_widgets_active(void);

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
static WlPointer pointers[PORT_COUNT];

/* the controllers once a frame at most (every port reads them each tick) */
static const WlRawInput *input_read(void)
{
	uint64_t now = wl_platform_time_us();

	if (!input_polled_us || now - input_polled_us > 4000)
	{
		int port;

		wl_platform_input_poll(&input);
		input_polled_us = now;
		for (port = 0; port < PORT_COUNT; port++)
		{
			/* (the Classic Controller has the sticks to look with, not the pointer) */
			if (input.remotes[port].extension == WL_EXT_CLASSIC)
				memset(&pointers[port], 0, sizeof(pointers[port]));
			else
				wl_pointer_update(&pointers[port], &input.remotes[port], now, WII_POINTER_HOLD_US);
		}
	}
	return &input;
}

/* debug.input_log: the gamepad state written to the log when it changes */
static BOOL input_logging(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static BOOL on;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		on = config_boolean("debug.input_log") ? TRUE : FALSE;
	}
	return on;
}

/* (the scripted player of the automated tests is controller 1:
port/linux/src/test_input.c) */
static BOOL port_connected(const WlRawInput *raw, int port)
{
	return raw->remotes[port].connected || raw->pads[port].connected || (port == 0 && test_input_active());
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
			wii_map_classic(&raw->remotes[port], &state->Gamepad, ui_widgets_active());
		else
			wii_map_remote(&raw->remotes[port], &state->Gamepad, ui_widgets_active());
	}
	else if (raw->pads[port].connected)
	{
		wii_map_gamecube(&raw->pads[port], &state->Gamepad, ui_widgets_active());
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
		if (input_logging())
		{
			const XINPUT_GAMEPAD *pad = &state->Gamepad;

			platform_log("input %d: buttons %04x analog %02x %02x %02x %02x %02x %02x %02x %02x left %d %d right %d %d%s",
				port, pad->wButtons, pad->bAnalogButtons[0], pad->bAnalogButtons[1], pad->bAnalogButtons[2],
				pad->bAnalogButtons[3], pad->bAnalogButtons[4], pad->bAnalogButtons[5], pad->bAnalogButtons[6],
				pad->bAnalogButtons[7], pad->sThumbLX, pad->sThumbLY, pad->sThumbRX, pad->sThumbRY,
				ui_widgets_active() ? " (menu)" : "");
		}
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
	if (gamepad_index < 0 || gamepad_index >= PORT_COUNT)
		return FALSE;
	input_read();
	if (!pointers[gamepad_index].tracking)
		return FALSE;
	*x = pointers[gamepad_index].x;
	*y = pointers[gamepad_index].y;
	return TRUE;
}

/* no aim magnetism while the pointer turns the view, as for the mouse; it
holds still in the box, where the magnetism helps to stay on a target */
int halo_linux_mouse_aiming(short gamepad_index)
{
	float x, y;

	return pointer_aiming(gamepad_index, &x, &y) &&
		(wl_pointer_turn(x, WII_POINTER_DEAD_ZONE) != 0.0f || wl_pointer_turn(y, WII_POINTER_DEAD_ZONE) != 0.0f);
}

/* the fastest turn, at the edge: the player's look rate (degrees per second
in player_control.c), until the game has set it */
static float look_rate(const float *rates, short gamepad_index, float fallback)
{
	return rates[gamepad_index] > 0.0f ? rates[gamepad_index] * (3.14159265f / 180.0f) : fallback;
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
	wii_map_pointer_look(x, y, seconds,
		look_rate(player_look_yaw_rate, gamepad_index, WII_POINTER_DEFAULT_YAW_RADIANS_PER_SECOND),
		look_rate(player_look_pitch_rate, gamepad_index, WII_POINTER_DEFAULT_PITCH_RADIANS_PER_SECOND),
		yaw, pitch);
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

/* ---------- the touch controls (port/linux/src/xinput_sdl.c's): a Wii has none */

int halo_linux_touch_aiming(short gamepad_index)
{
	(void)gamepad_index;
	return FALSE;
}

int halo_linux_touch_move(short controller_index, float *forward, float *strafe)
{
	(void)controller_index;
	*forward = 0.0f;
	*strafe = 0.0f;
	return FALSE;
}

int halo_linux_touch_look(short gamepad_index, float *yaw, float *pitch, float *gyro_yaw, float *gyro_pitch)
{
	(void)gamepad_index;
	*yaw = 0.0f;
	*pitch = 0.0f;
	*gyro_yaw = 0.0f;
	*gyro_pitch = 0.0f;
	return FALSE;
}
