/*
WII_VIDEO.C

The Wii's video mode and frame rate. WiiLibs (wl/platform/video_mode.h)
chooses the mode from the console's settings: 480p with the progressive
setting and a component cable, EuRGB60 on a PAL console with that setting
(else 576i at 50 Hz), a 16:9 TV's line of 704 pixels and the console's
display position. The game draws a 640x480 picture for a 4:3 screen; on a
16:9 setting WiiLibs narrows it into the middle three quarters of the
framebuffer with black at the sides (display.wii_widescreen = "pillarbox",
the default), or lets the TV stretch it ("stretch").

The game's frame rate is 30 frames a second (presentation interval two on
the Xbox's 60 Hz). A 50 Hz display cannot show that evenly: the display
queue holds each frame for the ratio of retraces that comes to 30 a second
(alternately two retraces and one, 30.0 frames a second with 3:2 pulldown's
unevenness) instead of two retraces each, which would be 25 a second. The
game's clock keeps its speed either way.
*/

#include "platform.h"

#include <string.h>

#include "port_config.h"
#include "wii_video.h"

#include "wl/platform/platform.h"

WlResult wii_video_init(void)
{
	const char *setting = config_string("display.wii_widescreen");
	WlWidescreen mode = WL_WIDESCREEN_PILLARBOX;
	WlResult result;
	char description[200];

	if (setting && !strcmp(setting, "stretch"))
		mode = WL_WIDESCREEN_STRETCH;
	else if (setting && strcmp(setting, "pillarbox"))
		platform_log("video: display.wii_widescreen \"%s\" is not \"pillarbox\" or \"stretch\": pillarbox", setting);
	result = wl_platform_video_set_widescreen(mode);
	if (result == WL_OK)
		result = wl_platform_video_init();
	if (result != WL_OK)
	{
		platform_log("video: could not start (WiiLibs result %d)", (int)result);
		return result;
	}
	wl_platform_video_describe(description, sizeof(description));
	platform_log("video: %s (display.wii_widescreen = \"%s\")", description,
		mode == WL_WIDESCREEN_STRETCH ? "stretch" : "pillarbox");
	return WL_OK;
}

void wii_video_present_interval(unsigned interval)
{
	/* (the Xbox's interval two is its 60 Hz display's 30 frames a second) */
	wl_platform_video_set_rate(interval >= 2 ? 60u / interval : 0u);
}
