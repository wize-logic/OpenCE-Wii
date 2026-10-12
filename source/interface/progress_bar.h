/*
PROGRESS_BAR.H

header included in hcex build.
*/

#ifndef __PROGRESS_BAR_H
#define __PROGRESS_BAR_H
#pragma once

/* ---------- headers */

#include "cseries/cseries.h"
#include "real_math.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes/PROGRESS_BAR.C */

void progress_bar_initialize(
	void);
void progress_bar_dispose(
	void);
void progress_bar_begin(
	boolean skip_frame_capture);
void progress_bar_end(
	void);
boolean progress_bar_is_active(
	void);
void progress_bar_enable(
	boolean enabled);
void progress_bar_eachframe(
	void);
void progress_bar_display(
	real progress);
boolean progress_bar_is_stuff_ready(
	void);

#ifdef HALO_WII
/* port: the Wii's loading screen kept moving through a level's load: begun
when the precache is done (game_precache_new_map), drawn at most every 33
ms from where the load waits or between its steps (progress_bar_port_pump),
ended when main_new_map has made the game */
void progress_bar_port_load_begin(
	void);
void progress_bar_port_load_end(
	void);
void progress_bar_port_pump(
	void);
#endif

/* ---------- globals */

/* ---------- public code */

#endif // __PROGRESS_BAR_H
