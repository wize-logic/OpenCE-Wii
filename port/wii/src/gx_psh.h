/*
GX_PSH.H

The game's pixel shaders on GX's texture environment (gx_psh.c): WiiLibs'
translation of the register combiners to TEV programs (wl/xbox/psh.h), and
its cache of them (wl/xbox/psh_cache.h), with the programs of the game's
own shaders made by hand (its recipes).
*/

#ifndef __GX_PSH_H
#define __GX_PSH_H

#include "platform.h"

#include "wl/xbox/psh_cache.h"

/* what the device puts in the vertices for a recipe's program
(WlXboxPshProgram.vertices, after WiiLibs' own) */
enum
{
	/* a specular light from a lightmap: colour 0 (1 - e.l) / 2 in red, green
	and blue, of the directions of texture coordinates 2 and 3 (the eye's and
	the light's), normalized */
	_gx_psh_vertices_specular = WL_XBOX_PSH_VERTICES_RECIPE,
};

/* whether the specular lights' programs are made by hand (TRUE, the
default); FALSE translates them as any other, their bumps approximated */
extern BOOL gx_psh_specular_recipes;
/* the program for the pixel shader render states and the textures, the
game's recipes first: FALSE when the combiners need more than GX has (the
program is then not usable) */
BOOL gx_psh_translate(const DWORD *render_state, const WlXboxPshTextures *textures, WlXboxPshProgram *program);
/* the same, kept (WiiLibs' cache, keyed on the texture modes too): *usable
FALSE when it is beyond GX, *translated TRUE when it was translated for this
call. The program is kept until the next call. */
const WlXboxPshProgram *gx_psh_cache_program(const DWORD *render_state, const WlXboxPshTextures *textures,
	BOOL *usable, BOOL *translated);
/* the cache's counts since the start */
void gx_psh_cache_counts(WlXboxPshCacheCounts *counts);

#endif
