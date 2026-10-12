/*
WII_VIDEO.H

The Wii's video mode and frame rate, between wii_video.c and the Direct3D
device (d3d8_gx.c): the device starts the video through wii_video_init and
sets each frame's stay on the screen through wii_video_present_interval.
port/wii/README.md ("The picture on each TV") says what the person sees.
*/

#ifndef __WII_VIDEO_H
#define __WII_VIDEO_H

#include "wl/platform/platform.h"

/* applies display.wii_widescreen, starts WiiLibs' video (the mode the
console's settings ask for: 480p, EuRGB60 or PAL 50, a 16:9 TV's line) and
logs the mode once ("video: ..."). WL_OK when it is up */
WlResult wii_video_init(void);

/* the game's presentation interval (1: every retrace the game's frame rate
throttle allows, 2: 30 frames a second) as the display queue's rate: on a
50 Hz display 30 frames a second are alternately two retraces and one, not
the 25 of an interval of two */
void wii_video_present_interval(unsigned interval);

#endif /* __WII_VIDEO_H */
