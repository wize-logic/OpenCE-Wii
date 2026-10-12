/*
WII_SCRIPT_CHECK.C

A host check of the Wii's verbs in port/linux/src/test_input.c (home, remote,
extension, battery, event; tools/wii_input_check.py builds and runs it): a
script is written to a file, played a read of the controller at a time, and
the state the port reads back (test_input_remote_override,
test_input_home_pressed, the simulated reset and power buttons) is compared
with what each step said. And that a verb with a port or value out of range
is a fault, which stops the script loading.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_config.h"
#include "test_input.h"

static int failures;
static int checks;

#define CHECK(condition, ...) \
	do \
	{ \
		checks++; \
		if (!(condition)) \
		{ \
			failures++; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

/* ---------- what test_input.c calls */

static char script_path[256];
static char log_text[8192];
static int simulated_reset, simulated_power;
static int quit_requested;

int config_boolean(const char *name)
{
	(void)name;
	return 0;
}

const char *config_string(const char *name)
{
	static char setting[300];

	if (strcmp(name, "debug.test_input"))
		return "";
	snprintf(setting, sizeof(setting), "script:%s", script_path);
	return setting;
}

const char *platform_data_root(void)
{
	return "/";
}

void platform_log(const char *format, ...)
{
	char line[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	strncat(log_text, line, sizeof(log_text) - strlen(log_text) - 2);
	strcat(log_text, "\n");
}

void platform_request_quit(void)
{
	quit_requested++;
}

/* (port/wii/src/wii_system.c) */
int wii_system_simulate(int power);
int wii_system_simulate(int power)
{
	if (power)
		simulated_power++;
	else
		simulated_reset++;
	return 0;
}

/* ---------- the rig */

static unsigned long long clock_ms;

/* a read of the controller, a tenth of a second on */
static void read_controller(void)
{
	XINPUT_GAMEPAD pad;

	memset(&pad, 0, sizeof(pad));
	clock_ms += 100;
	test_input_gamepad(&pad, clock_ms);
}

static void reads(int count)
{
	while (count-- > 0)
		read_controller();
}

static int overridden(int port, struct test_input_remote *remote)
{
	return test_input_remote_override(port, remote) ? 1 : 0;
}

static void write_script(const char *text)
{
	FILE *file = fopen(script_path, "w");

	fputs(text, file);
	fclose(file);
}

static void check_good(void)
{
	struct test_input_remote remote;
	int pressed = 0;
	int index;

	write_script(
		"# the Wii's verbs\n"
		"wait 0.5\n"
		"home\n"
		"remote 0 off\n"
		"extension 1 classic\n"
		"battery 2 7\n"
		"wait 1\n"
		"remote 0 on\n"
		"extension 1 real\n"
		"battery 2 real\n"
		"remote 3 off\n"
		"extension 3 nunchuk\n"
		"event power\n"
		"event reset\n"
		"wait 0.3\n"
		"remote 3 real\n"
		"extension 3 none\n"
		"battery 1 0\n"
		"battery 1 100\n");
	test_input_observe("", "", -1, 0, NULL);
	CHECK(test_input_scripted(), "the script is played");
	for (index = 0; index < 4; index++)
		CHECK(!overridden(index, &remote), "nothing is overridden before the script says so (port %d)", index);
	CHECK(!test_input_home_pressed(), "and no HOME is pressed");

	/* until the first verbs have run */
	for (index = 0; index < 60 && !overridden(0, &remote); index++)
	{
		read_controller();
		pressed += test_input_home_pressed() ? 1 : 0;
	}
	CHECK(index > 3 && index < 12, "the verbs wait for the 0.5 s before them (read %d)", index);
	CHECK(remote.connected == 0 && remote.extension == -1 && remote.battery == -1, "remote 0 off: gone, the rest as reported");
	CHECK(overridden(1, &remote) && remote.connected == -1 && remote.extension == 2 && remote.battery == -1,
		"extension 1 classic: a Classic Controller");
	CHECK(overridden(2, &remote) && remote.connected == -1 && remote.extension == -1 && remote.battery == 7,
		"battery 2 7: 7 percent");
	CHECK(!overridden(3, &remote), "port 3 untouched");
	CHECK(!overridden(-1, &remote) && !overridden(4, &remote), "and no port outside 0 to 3");

	/* HOME is held for 8 reads, in all */
	for (index = 0; index < 20 && !(overridden(0, &remote) && remote.connected == 1); index++)
	{
		read_controller();
		pressed += test_input_home_pressed() ? 1 : 0;
	}
	CHECK(pressed == 8, "HOME is down for 8 reads of the controllers, not %d", pressed);
	CHECK(index > 7 && index < 16, "the second group waits for the 1 s (read %d)", index);
	CHECK(overridden(0, &remote) && remote.connected == 1, "remote 0 on");
	CHECK(!overridden(1, &remote) && !overridden(2, &remote), "extension 1 real and battery 2 real return them to the console");
	CHECK(overridden(3, &remote) && remote.connected == 0 && remote.extension == 1, "remote 3 off, extension 3 nunchuk");
	CHECK(simulated_power == 1 && simulated_reset == 1, "event power and event reset: one of each, simulated");
	CHECK(!test_input_home_pressed(), "HOME is up again");

	reads(10);
	CHECK(!overridden(3, &remote) || (remote.connected == -1 && remote.extension == 0), "remote 3 real, extension 3 none");
	CHECK(overridden(1, &remote) && remote.battery == 100, "the last battery 1 100 wins");
	CHECK(!strstr(log_text, "not a step"), "no line of the script was refused");
	CHECK(strstr(log_text, "test_input: line") != NULL, "the steps are logged");
	CHECK(quit_requested == 0, "and nothing quits");
}

/* the line is a fault: the script is refused and nothing is set */
static int check_bad(const char *line)
{
	struct test_input_remote remote;
	char text[300];

	snprintf(text, sizeof(text), "wait 0.1\n%s\n", line);
	write_script(text);
	test_input_observe("", "", -1, 0, NULL);
	reads(30);
	CHECK(strstr(log_text, "not a step") != NULL, "'%s' is not a step", line);
	CHECK(!overridden(0, &remote) && !overridden(1, &remote) && !test_input_home_pressed(), "and sets nothing");
	return failures != 0;
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		printf("usage: wii_script_check <script file> good|bad [line]\n");
		return 2;
	}
	snprintf(script_path, sizeof(script_path), "%s", argv[1]);
	if (!strcmp(argv[2], "good"))
		check_good();
	else if (argc == 4)
		check_bad(argv[3]);
	printf("wii_script_check %s: %d checks, %d failures\n", argv[2], checks, failures);
	if (failures)
		printf("%s", log_text);
	return failures ? 1 : 0;
}
