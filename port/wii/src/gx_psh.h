/*
GX_PSH.H

The Xbox's pixel shaders on GX's texture environment (gx_psh.c): the
register combiners the pixel shader render states hold, as
port/linux/src/nv2a_psh.c translates them for OpenGL, become TEV stages.
*/

#ifndef __GX_PSH_H
#define __GX_PSH_H

#include "platform.h"

#include "wl/platform/tev.h"

enum
{
	GX_PSH_MAXIMUM_STAGES = 16,
	GX_PSH_TEXTURES = 4,
	/* the fog ramp's texture map, as the translation names it: GX's map 7,
	read at texture coordinate 4 (the vertex's fog factor) */
	GX_PSH_FOG_MAP = 8,
	GX_PSH_FOG_TEXMAP = 7,
	GX_PSH_FOG_TEXCOORD = 4,
	/* a bumped lookup's offset (gx_psh_textures.bumped): the bump map read by
	GX's indirect unit as map 5 (stage 0's texels through a TLUT of its
	normals' x and y), at texture coordinate 0, and the two terms of the
	offset, the sphere map's coordinates moved by the normal's x and by its
	y, at texture coordinates 1 and 2 */
	GX_PSH_BUMP_TEXMAP = 5,
	GX_PSH_BUMP_TEXCOORD_S = 1,
	GX_PSH_BUMP_TEXCOORD_T = 2,
	/* D0's alpha from a texture (gx_psh_textures.d0_alpha_map): the map's
	alpha, read at texture coordinate 5; as the translation names it, the
	map plus GX_PSH_D0_ALPHA_MAP */
	GX_PSH_D0_ALPHA_TEXCOORD = 5,
	GX_PSH_D0_ALPHA_MAP = 16,
};

/* a stage's part in a bumped lookup (gx_psh_program.indirect): GX's
environment-mapped bump (GX_SetTevIndBumpST), the coordinate of each lookup
made over the two stages before it */
enum
{
	_gx_psh_indirect_none,
	/* texture coordinate 1 times the bump's x (no texture read) */
	_gx_psh_indirect_s,
	/* plus texture coordinate 2 times the bump's y (no texture read) */
	_gx_psh_indirect_t,
	/* plus the stage's own coordinate: the lookup */
	_gx_psh_indirect_lookup,
	/* the stage before's coordinate again: a lookup right after one */
	_gx_psh_indirect_repeat,
};

/* what the device puts in the vertices for a program (gx_psh_program.vertices) */
enum
{
	/* what the vertex program made */
	_gx_psh_vertices_program,
	/* a bumped reflection (DOT_RFLCT_DIFF and DOT_RFLCT_SPEC): texture
	coordinates 1 and 2 the bump's two terms, 3 the sphere map's coordinates
	of the reflection of the flat normal, and colour 1 that normal, (n + 1) / 2
	(all from texture coordinates 1 to 3: the tangent frame's rows, the eye in
	their w); a flat one's (gx_psh_textures.reflected) the same */
	_gx_psh_vertices_bumped_reflection,
	/* a specular light from a lightmap: colour 0 (1 - e.l) / 2 in red, green
	and blue, of the directions of texture coordinates 2 and 3 (the eye's and
	the light's), normalized */
	_gx_psh_vertices_specular,
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
	(a bumped reflection's lookups, unlit) */
	unsigned unavailable;
	/* the stages whose cube map is sampled as a sphere map, as any other
	texture (a normalization cube map's direction, for a dot product with a
	bump map, among them) */
	unsigned cube;
	/* the stages whose lookup GX's indirect unit makes, offset by stage 0's
	bump map (a bumped reflection's DOT_RFLCT_SPEC: its cube map's sphere map,
	at the flat normal's reflection moved by the bump; read as any texture) */
	unsigned bumped;
	/* the stages read as colour 1, which the device makes what the stage's
	lookup would give of the flat normal (DOT_RFLCT_DIFF's normal) */
	unsigned vertex_color;
	/* the stages whose lookup (a DOT_RFLCT_SPEC whose bump GX cannot
	follow: the water's, of a bump map drawn each frame) is the cube map's
	sphere map at the flat normal's reflection, its coordinates the device's
	(texture coordinate 3, as a bumped reflection's): read as any texture */
	unsigned reflected;
	/* fog (D3DRS_FOGENABLE, its ramp bound) and its colour */
	unsigned char fog;
	DWORD fog_color;
	/* D0's alpha read from this texture map (4 to 7, one the stages leave
	free) rather than the rasterized colour, 0 as it is: the planar fog's,
	which the device makes of the position (gx_lighting.h) */
	unsigned char d0_alpha_map;
};

struct gx_psh_program
{
	/* the stages, konst colours, preloaded registers and swap tables
	(WiiLibs' TEV program, wl/platform/tev.h) */
	WlTevProgram tev;
	/* what the stages read of the vertices: the texture coordinates (bit n:
	GX_TEXCOORDn) and the colour channels (bit 0 COLOR0A0, bit 1 COLOR1A1) */
	unsigned char texcoords, channels;
	/* each stage's part in a bumped lookup (_gx_psh_indirect_*: the device
	sets GX's indirect unit by it), and what the device puts in the vertices
	(_gx_psh_vertices_*) */
	unsigned char indirect[GX_PSH_MAXIMUM_STAGES];
	unsigned char vertices;
	/* why it was not translated */
	const char *reason;
	/* something approximated: more constants than GX holds, a dot product
	or mapping that did not fit */
	BOOL approximate;
	const char *approximation;
};

/* FALSE when the combiners need more than GX has (the program is then not
usable) */
/* whether the specular lights' programs are made by hand (TRUE, the
default); FALSE translates them as any other, their bumps approximated */
extern BOOL gx_psh_specular_recipes;
BOOL gx_psh_translate(const DWORD *render_state, const struct gx_psh_textures *textures,
	struct gx_psh_program *program);
/* the stages to GX (their swap tables, konst colours and preloads too) */
void gx_psh_apply(const struct gx_psh_program *program);

#endif
