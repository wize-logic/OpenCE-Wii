/*
XINPUT_WII.C

The Xbox controllers (XInput) on the Wii's, in place of
port/linux/src/xinput_sdl.c, through WiiLibs' raw input
(wl_platform_input_poll). Port N is the Nth Wii Remote, with its Classic
Controller or Nunchuk, or the Nth GameCube controller when no Remote is
connected there. The buttons are mapped in xinput_wii_map.c (and listed in
port/wii/README.md, "The controllers"); this file is the XAPI around them,
the pointer's aim and the desktop's controls a Wii has none of.

The rumble is a service of its own (wii_rumble_tick, run by WiiLibs' system
watcher thread, port/wii/src/wii_system.c): the game says how strongly each
motor is to run (XInputSetState), and the service switches the controller's
one motor on and off by a duty cycle (xinput_wii_map.c), keeps the console's
rumble setting, and stops it when the game stops asking, a menu is up, the
controller goes, or the game quits.

The HOME button (WiiLibs' home_shell.h, joined to the game by
wii_home_game.c) is looked for where the controllers are read: while its menu
is up the game is given no controller and no rumble. A script's changes to the Remotes (test_input.c: remote, extension,
battery, home) are made to the raw input there, as the console's would be.

The Wii Remote's pointer aims (halo_linux_mouse_look): the view turns while
it points outside a box at the screen's centre, faster nearer the edge, at the
rates of the player's look sensitivity. It keeps turning for a moment when the
sensor bar is lost. The pointer is the mean of the Remote's reports of the
frame (WiiLibs reads them all), and it holds still within a small radius of
where it is held (input.pointer_steadiness), which takes the hand's tremor out.
*/

#include "platform.h"

#include <string.h>

#include "wl/platform/input_gesture.h"
#include "wl/platform/input_shape.h"
#include "wl/platform/platform.h"

#include "port_config.h"
#include "test_input.h"
#include "wii_home_config.h"
#include "wii_home_game.h"
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

/* a controller's motors: what the game asked for, and when, and the service's
own state (the watcher thread's) */
struct motor
{
	volatile unsigned strength;
	volatile unsigned long asked_ms;
	struct wii_rumble duty;
	BOOL sent;
};

/* a motor not asked for within this time is stopped: the game stops asking
while it loads (and halts), with the last strength still set */
#define RUMBLE_STALE_MS 250

static struct controller controllers[PORT_COUNT];
static DWORD reported_gamepads;
static WlRawInput input;
static uint64_t input_polled_us;
static uint64_t look_us[PORT_COUNT];
static WlPointer pointers[PORT_COUNT];
static struct motor motors[PORT_COUNT];
/* the Nunchuk's thrust as melee (input.nunchuk_melee): the detector for each
port while its Remote has a Nunchuk and the setting is on, the press it found,
and how many times the controllers have been polled (a press is shown by poll) */
static WlThrust thrusts[PORT_COUNT];
static BOOL thrust_live[PORT_COUNT];
static struct wii_melee_press melee_presses[PORT_COUNT];
static unsigned long poll_serial;
/* which controllers the last poll found (a bit a port), for the rumble
service, which must not read the poll's state while it is being filled */
static volatile unsigned remotes_connected;
static volatile unsigned pads_connected;
/* the scripted controller's navigation of the HOME menu, as of its last read */
static unsigned script_navigation;
static WlPlatformMutex motor_lock;
static BOOL motor_lock_made;
static BOOL motors_stopped;

/* a script's say over a Remote (test_input.c) in place of what the console
reported: gone, or there with nothing pressed, its extension, its battery */
static void script_remotes(WlRawInput *raw)
{
	int port;

	for (port = 0; port < PORT_COUNT; port++)
	{
		struct test_input_remote script;
		WlRawRemote *remote = &raw->remotes[port];

		if (!test_input_remote_override(port, &script))
			continue;
		if (script.connected == 0)
			memset(remote, 0, sizeof(*remote));
		else if (script.connected == 1 && !remote->connected)
			remote->connected = TRUE;
		if (!remote->connected)
			continue;
		if (script.extension >= 0)
			remote->extension = (unsigned char)script.extension;
		if (script.battery >= 0)
			remote->battery = (unsigned char)script.battery;
	}
}

/* input.pointer_steadiness, read again when the settings change */
static float pointer_steadiness(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static float radius = WII_POINTER_STEADINESS_DEFAULT;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		radius = wii_pointer_steadiness(config_real("input.pointer_steadiness"));
	}
	return radius;
}

/* input.nunchuk_melee, read again when the settings change */
static BOOL nunchuk_melee(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static BOOL on;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		on = config_boolean("input.nunchuk_melee") ? TRUE : FALSE;
	}
	return on;
}

/* the frame's readings of a Nunchuk to its detector: a thrust is a press of
melee for the game's next read (nothing is done, and nothing kept, for a port
without a Nunchuk or with the setting off) */
static void thrust_step(int port, const WlRawRemote *remote, uint64_t now_us)
{
	if (!nunchuk_melee() || !remote->connected || remote->extension != WL_EXT_NUNCHUK)
	{
		if (thrust_live[port])
		{
			thrust_live[port] = FALSE;
			melee_presses[port].state = 0;
		}
		return;
	}
	if (!thrust_live[port])
	{
		thrust_live[port] = TRUE;
		wl_thrust_init(&thrusts[port]);
	}
	if (remote->nunchuk_samples &&
		wl_thrust_feed(&thrusts[port], remote->nunchuk_accel, remote->nunchuk_samples, remote->nunchuk_reports, now_us))
		wii_melee_press_flag(&melee_presses[port], now_us);
}

static BOOL input_logging(void);

/* with debug.input_log, how the GameCube controllers' reads at every retrace
are doing (WiiLibs counts them): about every ten seconds */
static void pad_stats_log(void)
{
	WlPadStats stats;

	wl_platform_input_pad_stats(&stats);
	platform_log("input: pads read at the retrace: %u samples, %u with fresh data; %u polls, %u that read the pads themselves",
		stats.retrace_samples, stats.retrace_fresh, stats.polls, stats.polls_unsampled);
}

/* the controllers once a frame at most (every port reads them each tick) */
static const WlRawInput *input_read(void)
{
	uint64_t now = wl_platform_time_us();

	if (!input_polled_us || now - input_polled_us > 4000)
	{
		int port;
		unsigned remote_bits = 0;
		unsigned pad_bits = 0;

		wl_platform_input_poll(&input);
		input_polled_us = now;
		poll_serial++;
		script_remotes(&input);
		/* (the HOME menu: its look at the controllers, and what the script presses) */
		wii_home_game_poll(&input, now, script_navigation, test_input_home_pressed());
		for (port = 0; port < PORT_COUNT; port++)
		{
			if (input.remotes[port].connected)
				remote_bits |= 1u << port;
			if (input.pads[port].connected)
				pad_bits |= 1u << port;
			thrust_step(port, &input.remotes[port], now);
			/* (the Classic Controller has the sticks to look with, not the pointer) */
			if (input.remotes[port].extension == WL_EXT_CLASSIC)
				memset(&pointers[port], 0, sizeof(pointers[port]));
			else
				wl_pointer_update_steady(&pointers[port], &input.remotes[port], now, WII_POINTER_HOLD_US,
					pointer_steadiness());
		}
		remotes_connected = remote_bits;
		pads_connected = pad_bits;
		if (pad_bits && poll_serial % 300 == 0 && input_logging())
			pad_stats_log();
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

/* (the scripted player of the automated tests is controller 1, and a
script's connect and disconnect add and take away the others:
port/linux/src/test_input.c) */
static BOOL scripted_port(const WlRawInput *raw, int port)
{
	struct test_input_remote script;

	/* (a script that has taken Remote 0 away leaves the scripted controller
	with nothing, as it would be with no Remote and no pad) */
	if (port == 0 && test_input_remote_override(0, &script) && script.connected == 0)
		return FALSE;
	(void)raw;
	if (port > 0)
		return test_input_controller(port) == 1;
	return port == 0 && test_input_active();
}

/* a script's disconnect: the controller is gone, whatever the console has there */
static BOOL script_removed(int port)
{
	return port > 0 && test_input_controller(port) == 0;
}

static BOOL port_connected(const WlRawInput *raw, int port)
{
	if (script_removed(port))
		return FALSE;
	return raw->remotes[port].connected || raw->pads[port].connected || scripted_port(raw, port);
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
	{
		controller->open = FALSE;
		motors[controller - controllers].strength = 0;
	}
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
	if (script_removed(port))
	{
		motors[port].strength = 0;
		return ERROR_DEVICE_NOT_CONNECTED;
	}
	if (raw->remotes[port].connected)
	{
		if (raw->remotes[port].extension == WL_EXT_CLASSIC)
			wii_map_classic(&raw->remotes[port], &state->Gamepad, ui_widgets_active());
		else
		{
			wii_map_remote(&raw->remotes[port], &state->Gamepad, ui_widgets_active());
			if (raw->remotes[port].extension == WL_EXT_NUNCHUK &&
				wii_melee_press_read(&melee_presses[port], poll_serial, wl_platform_time_us(), ui_widgets_active()))
				wii_map_melee(&state->Gamepad);
		}
	}
	else if (raw->pads[port].connected)
	{
		wii_map_gamecube(&raw->pads[port], &state->Gamepad, ui_widgets_active());
	}
	else if (!scripted_port(raw, port))
	{
		/* (a controller that has gone does not keep its motor running) */
		motors[port].strength = 0;
		return ERROR_DEVICE_NOT_CONNECTED;
	}
	test_input_gamepad_port(port, &state->Gamepad, wl_platform_time_ms());
	if (port == 0)
	{
		/* (the scripted controller steers the HOME menu, if it is up) */
		script_navigation = test_input_active() ? wii_home_pad_navigation(&state->Gamepad) : 0;
	}
	/* the HOME menu is up, or a button that closed it is still down: the game
	sees a controller with nothing pressed */
	if (wii_home_game_blocks_input())
		memset(&state->Gamepad, 0, sizeof(state->Gamepad));
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

	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	feedback->Header.dwStatus = ERROR_SUCCESS;
	if (port < 0)
		return ERROR_DEVICE_NOT_CONNECTED;
	/* (the service switches the motor: the strength and when it was asked, and
	none while a menu is up, as when the game is paused) */
	motors[port].strength = ui_widgets_active() || wii_home_game_open() ? 0 :
		wii_rumble_strength(feedback->Rumble.wLeftMotorSpeed, feedback->Rumble.wRightMotorSpeed);
	motors[port].asked_ms = (unsigned long)wl_platform_time_ms();
	return ERROR_SUCCESS;
}

/* ---------- the rumble service (port/wii/src/wii_system.c runs it) */

void wii_rumble_start(void)
{
	if (!motor_lock_made && wl_platform_mutex_create(&motor_lock) == WL_OK)
		motor_lock_made = TRUE;
}

/* every motor to its duty cycle's state, on the watcher thread's schedule */
void wii_rumble_tick(void *unused)
{
	uint64_t now = wl_platform_time_us();
	unsigned long now_ms = (unsigned long)(now / 1000);
	int port;

	(void)unused;
	if (!motor_lock_made)
		return;
	wl_platform_mutex_lock(motor_lock);
	if (!motors_stopped)
	{
		for (port = 0; port < PORT_COUNT; port++)
		{
			struct motor *motor = &motors[port];
			BOOL remote = (remotes_connected >> port) & 1;
			BOOL pad = !remote && ((pads_connected >> port) & 1);
			unsigned strength = motor->strength;
			BOOL on;

			if (now_ms - motor->asked_ms > RUMBLE_STALE_MS || wii_home_game_open())
				strength = 0;
			if (!remote && !pad)
			{
				/* (nothing to run: the next one that connects starts still) */
				strength = 0;
				motor->sent = FALSE;
			}
			on = wii_rumble_duty(&motor->duty, strength, now);
			if (on != motor->sent && (remote || pad))
			{
				motor->sent = on;
				if (remote)
					wl_platform_rumble((uint8_t)port, on);
				else
					wl_platform_pad_rumble((uint8_t)port, on);
			}
		}
	}
	wl_platform_mutex_unlock(motor_lock);
}

/* every motor off, and the service off: before the game quits (WiiLibs' exit
does it too, but a tick must not turn one on again after) */
void wii_rumble_stop(void)
{
	int port;

	if (motor_lock_made)
		wl_platform_mutex_lock(motor_lock);
	motors_stopped = TRUE;
	for (port = 0; port < PORT_COUNT; port++)
	{
		motors[port].strength = 0;
		motors[port].sent = FALSE;
	}
	wl_platform_input_stop_rumble();
	if (motor_lock_made)
		wl_platform_mutex_unlock(motor_lock);
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
	/* (the HOME menu is up: the view does not turn) */
	if (!pointers[gamepad_index].tracking || wii_home_game_blocks_input())
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
