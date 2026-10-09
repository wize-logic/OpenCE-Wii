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
	/* more constants than GX's four: some read another's value */
	BOOL approximate;
};

/* FALSE when the combiners need more than GX has (the program is then not
usable) */
BOOL gx_psh_translate(const DWORD *render_state, const struct gx_psh_textures *textures,
	struct gx_psh_program *program);
void gx_psh_apply(const struct gx_psh_program *program);
/* the swap tables the stages use, once */
void gx_psh_initialize(void);

#endif
