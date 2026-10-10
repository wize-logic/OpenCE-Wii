/*
GX_PSH_CACHE.H

The translated pixel shaders, kept (gx_psh_cache.c): a program is translated
(gx_psh_translate) once for each set of combiners and textures, and once
again only when its constants change in a way the translation could see.
*/

#ifndef __GX_PSH_CACHE_H
#define __GX_PSH_CACHE_H

#include "gx_psh.h"

struct gx_psh_cache_counts
{
	/* programs translated (gx_psh_translate called for a draw), and of
	them those beyond GX */
	unsigned long translated, unusable;
	/* draws whose program was kept as it was, and those whose program was
	kept with its constants changed */
	unsigned long found, patched;
	/* the translations made to tell whether a program's constants can be
	changed in place (two a set of combiners), those that came out otherwise,
	and the sets found to be so */
	unsigned long tests, tests_differ, generic;
	/* changed programs translated again to check them, and those that came
	out otherwise (then translated for each set of constants) */
	unsigned long audited, audits_differ;
	/* structures pushed out of the cache by others, and structures
	translated that had been (by their hash, of the last 256 pushed out) */
	unsigned long evicted, again;
};

/* the program for the pixel shader render states and the textures (as
gx_psh_translate takes them); *usable FALSE when it is beyond GX, *translated
TRUE when it was translated for this call. The program is kept until the
next call. */
const struct gx_psh_program *gx_psh_cache_program(const DWORD *render_state,
	const struct gx_psh_textures *textures, BOOL *usable, BOOL *translated);
/* the counts since the start */
void gx_psh_cache_counts(struct gx_psh_cache_counts *counts);

#endif
