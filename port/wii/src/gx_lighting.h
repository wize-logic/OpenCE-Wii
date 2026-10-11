/*
GX_LIGHTING.H

The models' lighting on GX's own lights (gx_lighting.c): the colours the
game's lit model programs compute from their normals, made by GX's lighting
channels, and their reflection's sphere map coordinates by GX's texture
coordinate generation from the normal (display.gx_lighting). What GX takes
is computed here, apart from GX, so that the host check
(tests/gx_lighting_check.c) can compare it with the programs.
*/

#ifndef __GX_LIGHTING_H
#define __GX_LIGHTING_H

#include "platform.h"

enum
{
	GX_LIGHTING_LIGHTS = 8,
	/* the view-space sphere map's side, in texels (it is drawn in the EFB's
	rows below the back buffer: d3d8_gx.c) */
	GX_LIGHTING_SPHERE_SIZE = 48,
};

/* what D0's alpha is */
enum
{
	/* 0 (a texture coordinate's z, 0 in the models' vertices) */
	_gx_lighting_alpha_zero,
	/* the planar fog's, of the world position (the processor's, per vertex) */
	_gx_lighting_alpha_planar_fog,
	/* ... times the atmospheric fog's (1 - its distance's), as the programs
	lit in the world's space make it */
	_gx_lighting_alpha_fog_product,
};

struct gx_lighting_recipe;

/* the recipe of a program whose colours GX's lights can make (by its hash,
nv2a_cpu_hash), else NULL */
const struct gx_lighting_recipe *gx_lighting_recipe_find(unsigned long hash);
/* whether the recipe's program makes texture coordinate 3 the reflection
of the eye about the normal (a cube map's direction) */
BOOL gx_lighting_recipe_reflects(const struct gx_lighting_recipe *recipe);
/* whether its skinning blends the node matrices by v5 and v6 (else the
normal's matrix is the position's) */
BOOL gx_lighting_recipe_skinned(const struct gx_lighting_recipe *recipe);

/* The space GX lights in. GX lights the vertices in the space its position
matrices make, but the device's (x w, y w, -w) in pixels (d3d8_gx.c's
position_matrix) is not one where lengths and angles are the world's. It is
V = K E, where E is the world turned and moved (the eye's space, scaled by
sigma) and K is upper triangular with (0, 0, 1) as its last row, which GX's
perspective projection can take. The position matrices then make E, the
projection is multiplied by K, and the depth (Z, the same row) is as it was. */
struct gx_lighting_space
{
	/* K's entries (k01 is 0) */
	float k00, k02, k11, k12;
	/* the world to E (its rows sigma long and at right angles) */
	float world_to_eye[3][4];
	float sigma;
	/* the program's eye (c-92) in E */
	float eye[3];
};

/* the space for the draw's constants (constants[n] is c[n]: the device's
array from its 96th): FALSE where the program's projection has no such
space (a skew, a degenerate matrix) */
BOOL gx_lighting_space_make(const float (*constants)[4], struct gx_lighting_space *space);
/* rows of V (a position matrix) as rows of E: K's inverse times them */
void gx_lighting_space_rows(const struct gx_lighting_space *space, const float in[3][4], float out[3][4]);
/* the projection for E: the device's (x' = 2/W X + Z, y' = -2/H Y - Z,
z' = (1 - a) Z + b, w' = -Z) times K */
void gx_lighting_space_projection(const struct gx_lighting_space *space, float width, float height, float a, float b,
	float out[4][4]);

struct gx_lighting_light
{
	/* in E; the direction as GX_InitLightDir takes it */
	float position[3], direction[3];
	unsigned char color[4];
	/* GX_InitLightAttn's: max(0, a0 + a1 c + a2 c^2) / (k0 + k1 d + k2 d^2),
	c the cosine of the light's direction (GX_AF_SPOT) */
	float a[3], k[3];
};

/* the lights and channels of a draw */
struct gx_lighting
{
	unsigned light_count;
	struct gx_lighting_light lights[GX_LIGHTING_LIGHTS];
	/* each channel's lights (bit n: lights[n]): COLOR0's (D0's colour), and
	COLOR1A1's (D1) */
	unsigned char masks[2];
	/* COLOR1A1's diffuse is signed (GX_DF_SIGNED), COLOR0's clamped */
	unsigned char ambient[2][4];
	/* D0's alpha (_gx_lighting_alpha_...) */
	int alpha;
	/* the fogs' forms of the world position (c-87, c-86, and the
	atmospheric fog's c-88) and their parameters (c-85) */
	float fog_rows[3][4], fog[3];
	/* the reflection's c-92.w (2: a mirror's) */
	float reflection_scale;
	/* whether D1 (the reflection's Fresnel colour) is the program's */
	BOOL fresnel;
	/* the lights some vertex reads: what can be skipped when none is */
	BOOL lit;
};

/* GX's lights for the recipe's program with these constants, in the space;
center is a point of the draw in E (the point lights' fall-off is fitted
about it). FALSE where GX cannot hold them (a colour beyond GX's, a sign
it has no form of). */
BOOL gx_lighting_make(const struct gx_lighting_recipe *recipe, const float (*constants)[4],
	const struct gx_lighting_space *space, const float center[3], struct gx_lighting *lighting);

/* GX's lighting of a vertex at position (in E) with normal (in E, made
unit as GX makes it): D0's colour and D1, 0 to 255 (D0's alpha is not
set) */
void gx_lighting_evaluate(const struct gx_lighting *lighting, const float position[3], const float normal[3],
	unsigned char d0[4], unsigned char d1[4]);

/* D0's alpha of the planar fog, 0 to 1, at a world position */
float gx_lighting_planar_fog(const struct gx_lighting *lighting, const float world[3]);

/* the skinned programs' matrix of a vertex (its node indices v5 and
weights v6): the rows they blend, as the program blends them */
void gx_lighting_skin(const float (*constants)[4], const float nodes[4], const float weights[4], float out[3][4]);

/* The sphere map of a view. The world's sphere map (a mirror ball looking
down the world's z) is looked up at a reflection's direction, which is not
linear in the normal; a view's sphere map is looked up at the normal in a
frame turned to the eye, made unit, (x / 2 + 1/2, y / 2 + 1/2), as GX's
texture coordinates from the normal give it. Its texel is the world's
sphere map at the reflection the programs make of the eye about that
normal, the eye taken to be along the frame's z (for every vertex of the
draws that look it up: the approximation).

The frame of a view (rows: world directions to it) whose z is the direction
toward (in E) to the eye, its angles about E's y and x rounded to step
radians (0: as they are), E's x and y turned with it; turn (if not NULL) is the
turn of E to the frame (rows). */
void gx_lighting_view_frame(const struct gx_lighting_space *space, const float toward[3], float step, float frame[3][3],
	float turn[3][3]);
/* what texel (s, t) of the frame's sphere map holds: the world's sphere
map's coordinates. In two steps, the first the same for every frame: the
unit sphere's point at (s, t), then what the frame's sphere map holds there */
void gx_lighting_sphere_texel(const float frame[3][3], float reflection_scale, float s, float t, float out[2]);
void gx_lighting_sphere_point(float s, float t, float point[3]);
void gx_lighting_sphere_point_texel(const float frame[3][3], float reflection_scale, const float point[3], float out[2]);
/* the world's sphere map's coordinates of a direction (d3d8_gx.c's
sphere_coordinates) */
void gx_lighting_sphere_coordinates(const float direction[3], float out[2]);

#endif
