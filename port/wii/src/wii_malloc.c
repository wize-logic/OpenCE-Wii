/*
WII_MALLOC.C

The game's heap on the Wii. newlib's malloc grows from MEM1's arena, which
the game's code, static data and game state fill (port/wii/README.md,
"Memory"): about 4 MB are left there, where the Xbox had all of its 64 MB to
share. The link wraps malloc, calloc, realloc and free (tools/wii_build.py),
so that what MEM1 cannot hold comes from a heap of WiiLibs' in MEM2 instead
(WII_MEMORY_TAG_GAME, src/wii_boot.c), and goes back to the heap it came
from. A block from MEM2 keeps its size in front of it, for realloc.
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/mem.h"
#include "wl/platform/platform.h"

#include "wii_memory.h"

#define OVERFLOW_HEADER 32u

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
size_t malloc_usable_size(void *pointer);

void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *pointer, size_t size);
void __wrap_free(void *pointer);

static uintptr_t overflow_base, overflow_end;

static int from_overflow(const void *pointer)
{
	if (!overflow_end)
	{
		void *base;
		size_t size;

		if (wl_platform_mem_region(WL_MEM2, &base, &size) != WL_OK)
			return 0;
		overflow_base = (uintptr_t)base;
		overflow_end = overflow_base + size;
	}
	return (uintptr_t)pointer >= overflow_base && (uintptr_t)pointer < overflow_end;
}

static void *overflow_alloc(size_t size)
{
	static int reported;
	unsigned char *block;

	if (!wl_mem_ready())
		return NULL;
	block = wl_mem_alloc_at(WII_MEMORY_TAG_GAME, size + OVERFLOW_HEADER, 32, __FILE__, __LINE__);
	if (!block)
		return NULL;
	if (!reported)
	{
		reported = 1;
		WL_LOG_W("wii", "MEM1's heap is full: allocations go to MEM2 now");
	}
	*(size_t *)block = size;
	return block + OVERFLOW_HEADER;
}

void *__wrap_malloc(size_t size)
{
	void *pointer = __real_malloc(size);

	if (!pointer && size)
	{
		pointer = overflow_alloc(size);
		if (!pointer)
		{
			/* (who asked: the return addresses up the frame chain, for
			addr2line on build/wii/halo.elf) */
			unsigned long *frame = __builtin_frame_address(0);
			unsigned long callers[6] = { 0 };
			int depth;

			for (depth = 0; depth < 6 && frame && (unsigned long)frame >= 0x80000000UL &&
				(unsigned long)frame < 0x94000000UL; depth++)
			{
				frame = (unsigned long *)frame[0];
				if (!frame || (unsigned long)frame < 0x80000000UL || (unsigned long)frame >= 0x94000000UL)
					break;
				callers[depth] = frame[1];
			}
			WL_LOG_E("wii", "malloc(%u) failed, from %08lx %08lx %08lx %08lx %08lx %08lx", (unsigned)size,
				callers[0], callers[1], callers[2], callers[3], callers[4], callers[5]);
		}
	}
	return pointer;
}

void *__wrap_calloc(size_t count, size_t size)
{
	void *pointer = __real_calloc(count, size);

	if (!pointer && count && size && size <= SIZE_MAX / count)
	{
		pointer = overflow_alloc(count * size);
		if (pointer)
			memset(pointer, 0, count * size);
	}
	return pointer;
}

void __wrap_free(void *pointer)
{
	if (!pointer)
		return;
	if (from_overflow(pointer))
		wl_mem_free((unsigned char *)pointer - OVERFLOW_HEADER);
	else
		__real_free(pointer);
}

void *__wrap_realloc(void *pointer, size_t size)
{
	size_t old_size;
	void *moved;

	if (!pointer)
		return __wrap_malloc(size);
	if (!size)
	{
		__wrap_free(pointer);
		return NULL;
	}
	if (!from_overflow(pointer))
	{
		moved = __real_realloc(pointer, size);
		if (moved)
			return moved;
		old_size = malloc_usable_size(pointer);
	}
	else
	{
		old_size = *(size_t *)((unsigned char *)pointer - OVERFLOW_HEADER);
	}
	moved = __wrap_malloc(size);
	if (!moved)
		return NULL;
	memcpy(moved, pointer, old_size < size ? old_size : size);
	__wrap_free(pointer);
	return moved;
}
