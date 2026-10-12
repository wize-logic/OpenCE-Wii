/*
GX_STAND_IN.H

libogc's GX for the device on the host (port/wii/tests/draw_bench.c, run
under qemu-ppc by tools/wii_draw_bench.py): gx_stand_in.c has the functions
the device calls. Built with GX_STAND_IN_RECORD (and the copy of <ogc/gx.h>
whose vertex writes call it), it decodes each primitive's vertices as GX
reads them - indices through the arrays, each vertex's position and texture
matrices from the matrix memory - and digests what GX would draw: two builds
of the device that send the same draws the same way or another (a vertex
direct or by index, another matrix slot, a display list) give the same
digests. Without it, the functions do nothing (instruction counts).
*/

#ifndef __GX_STAND_IN_H
#define __GX_STAND_IN_H

#include <stdint.h>

/* each primitive drawn since the last call: its digest, its vertex count */
struct gx_stand_in_draw
{
	uint64_t digest;
	uint32_t vertices;
};

/* the draws recorded since the last take (at most capacity; the count
returned may be larger) */
unsigned long gx_stand_in_take(struct gx_stand_in_draw *draws, unsigned long capacity);
/* vertex writes outside a primitive, and primitives left short (recording) */
unsigned long gx_stand_in_errors(void);
/* each state call's last arguments (recording): its name, selector and
their hash */
void gx_stand_in_states(void (*each)(const char *name, uint32_t selector, uint64_t hash));
/* (recording) each vertex's texture coordinate gen's s as GX makes it,
through its matrix and post-transform matrix, passed to each; NULL: none */
void gx_stand_in_trace(unsigned gen, void (*each)(float s));
/* calls to GX's functions (not the vertex writes) since the last take */
unsigned long gx_stand_in_calls(void);

#endif
