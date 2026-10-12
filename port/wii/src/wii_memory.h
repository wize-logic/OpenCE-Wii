/*
WII_MEMORY.H

The Wii port's memory, between its own units (port/wii/src): the WiiLibs
heap tag the game's overflow heap is (src/wii_boot.c, WiiLibs' heap overflow).
*/

#ifndef __WII_MEMORY_H
#define __WII_MEMORY_H

#include "wl/platform/mem.h"

enum
{
	/* the game's malloc, once MEM1's heap is full (wl_heap_overflow_enable) */
	WII_MEMORY_TAG_GAME = WL_TAG_USER,
	WII_MEMORY_TAG_COUNT
};

/* the contiguous window's pages in use and free, and its largest free run
(NULL: not wanted), for the log's memory report */
void wii_memory_window_usage(unsigned long *used_bytes, unsigned long *free_bytes, unsigned long *largest_free);
/* the window's start and size, as claimed */
void wii_memory_window_range(unsigned long *base, unsigned long *size);

/* The Direct3D device's kept buffers (src/d3d8_gx.c, "the kept buffers'
memory"): pages from the window's bottom, where the game's top-down blocks
reach last, in the smallest free run they fit; NULL where they would leave
the window's largest free run below keep_largest bytes (unless it is not
that run they take). Freed with platform_contiguous_free. */
void *wii_memory_window_alloc_low(unsigned long size, unsigned long keep_largest);
/* how many times the window's pages were freed: room may be had again */
unsigned long wii_memory_window_frees(void);
/* called when a top-down block finds no room, on the thread asking for it:
it frees what it can (the device's kept buffers) and says whether it freed
anything, and the block is looked for again */
void wii_memory_window_set_reclaim(int (*reclaim)(void));

#endif /* __WII_MEMORY_H */
