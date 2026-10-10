/*
WII_MEMORY.H

The Wii port's memory, between its own units (port/wii/src): the WiiLibs
heap tag the game's overflow heap is (src/wii_boot.c, src/wii_malloc.c).
*/

#ifndef __WII_MEMORY_H
#define __WII_MEMORY_H

#include "wl/platform/mem.h"

enum
{
	/* the game's malloc, once MEM1's heap is full (src/wii_malloc.c) */
	WII_MEMORY_TAG_GAME = WL_TAG_USER,
	WII_MEMORY_TAG_COUNT
};

/* the contiguous window's pages in use and free (the log's memory report) */
void wii_memory_window_usage(unsigned long *used_bytes, unsigned long *free_bytes);

#endif /* __WII_MEMORY_H */
