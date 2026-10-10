/*
GX_PSH.H

The Xbox's pixel shaders on GX's texture environment (gx_psh.c): the
register combiners the pixel shader render states hold, as
port/linux/src/nv2a_psh.c translates them for OpenGL, become TEV stages.
*/

#ifndef __GX_PSH_H
#define __GX_PSH_H

#include "platform.h"

enum
{
	GX_PSH_MAXIMUM_STAGES = 16,
	GX_PSH_TEXTURES = 4,
	/* the fog ramp's texture map, as the translation names it: GX's map 7,
	read at texture coordinate 4 (the vertex's fog factor) */
	GX_PSH_FOG_MAP = 8,
	GX_PSH_FOG_TEXMAP = 7,
	GX_PSH_FOG_TEXCOORD = 4,
};

/* what a texture stage's texture holds */
enum
{
	/* all four channels */
	_gx_psh_texture_all,
	/* colour only: its alpha reads as 1 (luminance) */
	_gx_psh_texture_color,
	/* alpha only: its colour reads as 1 */
	_gx_psh_texture_alpha,
};

struct gx_psh_textures
{
	/* the stages that sample a texture (bound, in a 2D mode) */
	unsigned present;
	/* the stages whose alpha is in the texture map four after (DXT3, DXT5) */
	unsigned separate_alpha;
	unsigned char contents[GX_PSH_TEXTURES];
	/* the stages whose texture GX cannot sample (a cube map, a dependent or
	dot product mode): read as 0, but a dot product with one is taken as 1
	(a bump map against a normalization cube map, unlit) */
	unsigned unavailable;
	/* the stages whose cube map is sampled as a sphere map: a lookup reads
	it, but a dot product with it (a bump map against a normalization cube
	map, whose signed values TEV clamps) is still taken as 1 */
	unsigned cube;
	/* fog (D3DRS_FOGENABLE, its ramp bound) and its colour */
	unsigned char fog;
	DWORD fog_color;
};

struct gx_psh_stage
{
	unsigned char texture_map, ras_channel;
	unsigned char texture_swap, ras_swap;
	unsigned char konst_color, konst_alpha;
	unsigned char color_in[4], color_op, color_bias, color_scale, color_dest;
	unsigned char alpha_in[4], alpha_op, alpha_bias, alpha_scale, alpha_dest;
};

struct gx_psh_program
{
	unsigned stage_count;
	struct gx_psh_stage stages[GX_PSH_MAXIMUM_STAGES];
	unsigned konst_count;
	DWORD konst[4];
	/* why it was not translated */
	const char *reason;
	/* constants beyond the four konst registers: TEV registers given their
	values before the stages run (GX_SetTevColor) */
	unsigned preload_count;
	unsigned char preload_register[3];
	DWORD preload_color[3];
	/* something approximated: more constants than GX holds, a dot product
	or mapping that did not fit */
	BOOL approximate;
	const char *approximation;
};

/* FALSE when the combiners need more than GX has (the program is then not
usable) */
BOOL gx_psh_translate(const DWORD *render_state, const struct gx_psh_textures *textures,
	struct gx_psh_program *program);
void gx_psh_apply(const struct gx_psh_program *program);
/* the swap tables the stages use, once */
void gx_psh_initialize(void);

#endif
