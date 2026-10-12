/*
WII_MEMORY.C

Xbox contiguous memory (XPhysicalAlloc) for the Wii, in place of
port/linux/src/xbox_memory.c.

The Wii, as the Xbox, sees physical memory at virtual 0x80000000 + P (the
cached mapping), so the game's and Direct3D's physical/virtual arithmetic
works on the real addresses, and GX takes the physical ones. There is no
memory to reserve and no MMU to protect pages with: the layer claims fixed
ranges of the console's two regions from WiiLibs before anything else runs
(wii_memory_claim, from port/wii/src/wii_boot.c) and hands out page-granular
blocks from them, as the Linux layer does from its window:

- the window (PLATFORM_CONTIGUOUS_WINDOW, in MEM2): the tag cache placed at
  its bottom, which converted maps are linked to; the texture and sound
  caches and Direct3D's resources top-down. It reaches down from there to
  the start of what libogc leaves of MEM2 (about 1 MB more), which the
  top-down blocks reach last. The Direct3D device's kept buffers come from
  the bottom up (wii_memory_window_alloc_low), and give their pages back to
  a top-down block that finds no room (wii_memory_window_set_reclaim);
- the game state's, at the top of MEM1 (HALO_PORT_GAME_STATE_BASE_ADDRESS).

Page protection is only recorded (XQueryMemoryProtect reports it).
*/

#include "platform.h"
#include "halo_port_capacity.h"

#include <string.h>

#include "wl/platform/platform.h"

#include "wii_memory.h"

/* libogc's (ogc/system.h, whose BOOL is not the port's): where what it
leaves of MEM2 begins */
void *SYS_GetArena2Lo(void);

#define PAGE_SIZE_BYTES 0x1000UL

struct contiguous_window
{
	unsigned long base;
	unsigned long page_count;
	WlMemRegion region;
	/* per page: 0 free, otherwise the protection of the block (PAGE_*, all
	below 0x400 once the caching flags are taken off); the first page of a
	block also records the block length */
	unsigned short *page_protection;
	unsigned short *block_page_count;
	BOOL claimed;
};

/* the window's top, and the lowest it may reach down to (MEM2's start) */
#define WINDOW_TOP (PLATFORM_CONTIGUOUS_WINDOW + PLATFORM_CONTIGUOUS_SIZE)
#define WINDOW_LOWEST 0x90000000UL
#define WINDOW_PAGES ((WINDOW_TOP - WINDOW_LOWEST) / PAGE_SIZE_BYTES)
#define GAME_STATE_PAGES ((HALO_PORT_GAME_STATE_SIZE + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES)

static unsigned short window_protection[WINDOW_PAGES];
static unsigned short window_blocks[WINDOW_PAGES];
static unsigned short game_state_protection[GAME_STATE_PAGES];
static unsigned short game_state_blocks[GAME_STATE_PAGES];

static struct contiguous_window windows[] =
{
	{ PLATFORM_CONTIGUOUS_WINDOW, PLATFORM_CONTIGUOUS_SIZE / PAGE_SIZE_BYTES, WL_MEM2, window_protection, window_blocks, FALSE },
	{ HALO_PORT_GAME_STATE_BASE_ADDRESS, GAME_STATE_PAGES, WL_MEM1, game_state_protection, game_state_blocks, FALSE },
};
#define WINDOW_COUNT (sizeof(windows) / sizeof(windows[0]))

static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
/* (a change of the windows' pages, taken or freed: wii_memory_window_usage
counts them again only after one) */
static unsigned long window_changes = 1;
/* (the window's pages freed: wii_memory_window_frees) */
static unsigned long window_frees;
/* (what frees the device's kept buffers when a block finds no room, and
whether it is running) */
static int (*window_reclaim)(void);
static int window_reclaiming;

BOOL wii_memory_claim(void)
{
	unsigned long index;
	BOOL claimed = TRUE;
	/* (the window from where libogc's MEM2 begins, if that is below the
	tag cache: the pages between are otherwise no one's) */
	unsigned long lowest = ((unsigned long)SYS_GetArena2Lo() + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1);

	if (lowest >= WINDOW_LOWEST && lowest < windows[0].base)
	{
		windows[0].base = lowest;
		windows[0].page_count = (WINDOW_TOP - lowest) / PAGE_SIZE_BYTES;
	}
	for (index = 0; index < WINDOW_COUNT; index++)
	{
		struct contiguous_window *window = &windows[index];
		WlResult result = wl_platform_mem_claim_fixed(window->region, (void *)window->base,
			window->page_count * PAGE_SIZE_BYTES);

		window->claimed = result == WL_OK;
		if (!window->claimed)
		{
			platform_log("cannot claim the contiguous memory at 0x%08lx (%lu bytes, MEM%d): %d",
				window->base, window->page_count * PAGE_SIZE_BYTES, window->region + 1, (int)result);
			claimed = FALSE;
		}
	}
	return claimed;
}

static struct contiguous_window *window_of(unsigned long address)
{
	unsigned long index;

	for (index = 0; index < WINDOW_COUNT; index++)
	{
		if (address >= windows[index].base &&
			address - windows[index].base < windows[index].page_count * PAGE_SIZE_BYTES)
			return &windows[index];
	}
	return NULL;
}

BOOL platform_is_contiguous(const void *address)
{
	return window_of((unsigned long)address) != NULL;
}

static BOOL pages_free(const struct contiguous_window *window, unsigned long first, unsigned long count)
{
	unsigned long page;

	if (first + count > window->page_count)
		return FALSE;
	for (page = first; page < first + count; page++)
	{
		if (window->page_protection[page])
			return FALSE;
	}
	return TRUE;
}

static void *contiguous_alloc_once(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect)
{
	unsigned long count = (size + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
	unsigned long alignment_pages = alignment > PAGE_SIZE_BYTES ? alignment / PAGE_SIZE_BYTES : 1;
	struct contiguous_window *window = &windows[0];
	unsigned long first;
	unsigned long page;
	void *address;

	if (!count)
		count = 1;
	protect &= ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
	if (!protect)
		protect = PAGE_READWRITE;

	pthread_mutex_lock(&window_lock);
	if (physical_address != PLATFORM_ANY_PHYSICAL_ADDRESS)
	{
		/* exactly there, in whichever window holds it */
		unsigned long wanted = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(physical_address);

		window = window_of(wanted);
		first = window && window->claimed && (wanted - window->base) % PAGE_SIZE_BYTES == 0 &&
			pages_free(window, (wanted - window->base) / PAGE_SIZE_BYTES, count) ?
			(wanted - window->base) / PAGE_SIZE_BYTES : (unsigned long)-1;
	}
	else
	{
		/* top-down first fit in the main window, like the Xbox contiguous
		allocator */
		first = (unsigned long)-1;
		if (window->claimed && count <= window->page_count)
		{
			unsigned long candidate = window->page_count - count;

			for (;;)
			{
				candidate -= candidate % alignment_pages;
				if (pages_free(window, candidate, count))
				{
					first = candidate;
					break;
				}
				if (candidate == 0)
					break;
				candidate--;
			}
		}
	}
	if (first == (unsigned long)-1)
	{
		pthread_mutex_unlock(&window_lock);
		return NULL;
	}

	address = (void *)(window->base + first * PAGE_SIZE_BYTES);
	for (page = first; page < first + count; page++)
		window->page_protection[page] = (unsigned short)protect;
	window->block_page_count[first] = (unsigned short)count;
	window_changes++;
	pthread_mutex_unlock(&window_lock);
	/* fresh pages are zeroed, as the Xbox kernel's are */
	memset(address, 0, count * PAGE_SIZE_BYTES);
	return address;
}

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect)
{
	void *address = contiguous_alloc_once(size, alignment, physical_address, protect);

	/* (no room: the device's kept buffers give theirs back, and it is looked
	for again; the game's and the device's own blocks come before them) */
	if (!address && physical_address == PLATFORM_ANY_PHYSICAL_ADDRESS && window_reclaim && !window_reclaiming)
	{
		int freed;

		window_reclaiming = 1;
		freed = window_reclaim();
		window_reclaiming = 0;
		if (freed)
		{
			address = contiguous_alloc_once(size, alignment, physical_address, protect);
			platform_log("contiguous memory: the Direct3D device's kept buffers given back for a block of %lu bytes%s",
				size, address ? "" : ", and still no room");
		}
	}
	return address;
}

/* the device's kept buffers, from the window's bottom: the smallest free run
they fit, at its bottom (best fit: the runs the game's blocks left between
them first), unless it is the window's largest and would be left below
keep_largest */
void *wii_memory_window_alloc_low(unsigned long size, unsigned long keep_largest)
{
	struct contiguous_window *window = &windows[0];
	unsigned long count = (size + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
	unsigned long page, run = 0, best = (unsigned long)-1, best_run = 0, largest = 0, second = 0, largest_first = 0;
	unsigned long after;
	void *address;

	if (!count)
		count = 1;
	pthread_mutex_lock(&window_lock);
	if (!window->claimed)
	{
		pthread_mutex_unlock(&window_lock);
		return NULL;
	}
	for (page = 0; page <= window->page_count; page++)
	{
		if (page < window->page_count && !window->page_protection[page])
		{
			run++;
			continue;
		}
		if (run)
		{
			unsigned long first = page - run;

			if (run >= count && (best == (unsigned long)-1 || run < best_run))
			{
				best = first;
				best_run = run;
			}
			if (run > largest)
			{
				second = largest;
				largest = run;
				largest_first = first;
			}
			else if (run > second)
			{
				second = run;
			}
			run = 0;
		}
	}
	/* (the largest run the window keeps once the block is taken) */
	after = best != largest_first ? largest : best_run - count > second ? best_run - count : second;
	if (best == (unsigned long)-1 || (after != largest && after * PAGE_SIZE_BYTES < keep_largest))
	{
		pthread_mutex_unlock(&window_lock);
		return NULL;
	}
	address = (void *)(window->base + best * PAGE_SIZE_BYTES);
	for (page = best; page < best + count; page++)
		window->page_protection[page] = PAGE_READWRITE;
	window->block_page_count[best] = (unsigned short)count;
	window_changes++;
	pthread_mutex_unlock(&window_lock);
	return address;
}

unsigned long wii_memory_window_frees(void)
{
	return window_frees;
}

void wii_memory_window_set_reclaim(int (*reclaim)(void))
{
	window_reclaim = reclaim;
}

/* the main window's pages in use and free, and the largest free run, for
the log */
void wii_memory_window_usage(unsigned long *used_bytes, unsigned long *free_bytes, unsigned long *largest_free)
{
	const struct contiguous_window *window = &windows[0];
	static unsigned long counted_changes, used, largest;
	unsigned long page, run = 0;

	pthread_mutex_lock(&window_lock);
	if (counted_changes != window_changes)
	{
		counted_changes = window_changes;
		used = 0;
		largest = 0;
		for (page = 0; page < window->page_count; page++)
		{
			if (window->page_protection[page])
			{
				used++;
				run = 0;
			}
			else if (++run > largest)
			{
				largest = run;
			}
		}
	}
	if (used_bytes)
		*used_bytes = used * PAGE_SIZE_BYTES;
	if (free_bytes)
		*free_bytes = (window->page_count - used) * PAGE_SIZE_BYTES;
	if (largest_free)
		*largest_free = largest * PAGE_SIZE_BYTES;
	pthread_mutex_unlock(&window_lock);
}

/* the window's start and size (it reaches below PLATFORM_CONTIGUOUS_WINDOW
when MEM2 is free there) */
void wii_memory_window_range(unsigned long *base, unsigned long *size)
{
	*base = windows[0].base;
	*size = windows[0].page_count * PAGE_SIZE_BYTES;
}

void platform_contiguous_free(void *address)
{
	struct contiguous_window *window = window_of((unsigned long)address);
	unsigned long first, count, page;

	if (!window)
		return;
	first = ((unsigned long)address - window->base) / PAGE_SIZE_BYTES;
	pthread_mutex_lock(&window_lock);
	count = window->block_page_count[first];
	for (page = first; page < first + count; page++)
		window->page_protection[page] = 0;
	window->block_page_count[first] = 0;
	window_changes++;
	window_frees++;
	pthread_mutex_unlock(&window_lock);
}

/* Halo Custom Edition maps are linked to 0x40440000, which the Wii does not
have: they are not run (yet). */
void *halo_custom_edition_tag_cache(void)
{
	return NULL;
}

/* ---------- XAPI */

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR physical_address, ULONG_PTR alignment, DWORD protect)
{
	/* An address inside a window places the block there, which is how the
	game gets its fixed game state and tag cache; anything else (the Xbox's
	"highest acceptable address") may go anywhere. */
	unsigned long wanted = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(physical_address);
	void *result = platform_contiguous_alloc(size, alignment,
		physical_address != PLATFORM_ANY_PHYSICAL_ADDRESS && window_of(wanted) ? physical_address :
		PLATFORM_ANY_PHYSICAL_ADDRESS, protect);

	if (!result)
	{
		platform_log("XPhysicalAlloc: cannot allocate %lu bytes (physical address 0x%08lx)",
			(unsigned long)size, (unsigned long)physical_address);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	}
	return result;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	platform_contiguous_free(address);
}

static void protection_set(unsigned long start, unsigned long end, DWORD protect)
{
	unsigned long address;

	pthread_mutex_lock(&window_lock);
	for (address = start; address < end; address += PAGE_SIZE_BYTES)
	{
		struct contiguous_window *window = window_of(address);
		unsigned long page;

		if (!window)
			continue;
		page = (address - window->base) / PAGE_SIZE_BYTES;
		if (window->page_protection[page])
			window->page_protection[page] = (unsigned short)(protect & ~(PAGE_WRITECOMBINE | PAGE_NOCACHE));
	}
	pthread_mutex_unlock(&window_lock);
}

BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	unsigned long start = (unsigned long)address & ~(PAGE_SIZE_BYTES - 1);
	unsigned long end = ((unsigned long)address + size + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1);

	if (old_protect)
		*old_protect = XQueryMemoryProtect(address);
	protection_set(start, end, new_protect);
	return TRUE;
}

VOID WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD new_protect)
{
	VirtualProtect(address, size, new_protect, NULL);
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	struct contiguous_window *window = window_of((unsigned long)address);
	DWORD protect;

	if (!window)
		return PAGE_READWRITE;
	pthread_mutex_lock(&window_lock);
	protect = window->page_protection[((unsigned long)address - window->base) / PAGE_SIZE_BYTES];
	pthread_mutex_unlock(&window_lock);
	return protect ? protect : PAGE_NOACCESS;
}
