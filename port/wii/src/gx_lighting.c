/*
GX_LIGHTING.C

The models' lighting on GX's own lights (gx_lighting.h).

The game's lit model programs (rasterizer_xbox_models.c chooses them, and
rasterizer_xbox.c's rasterizer_set_model_lighting sets their constants)
light each vertex by its normal, skinned by the same nodes as its position:
- D0's colour: the ambient colour (c-69), two distant lights (direction
  c-73 and c-71, colour c-72 and c-70), the first with a back side for
  translucent shaders (its n.l less than 0 times c-82.z), and in the "lights"
  and planar fog permutations two point lights (position c-79 and c-76 with
  their radius's inverse square in w, spot direction c-78 and c-75 with its
  scale in w, colour c-77 and c-74 with the spot's offset in w), each
  max(1 - d^2/r^2, 0) times n.l times the spot clamped to [0, 1];
- D1: the reflection's Fresnel colour, c-80 + c-81 (n.e), e the unit
  vector to the eye (c-92);
- D0's alpha: 0, or the planar fog's (a function of the world position);
- texture coordinate 3: the eye's reflection about the normal,
  c-92.w (n.e) n - e, a cube map's direction (its sphere map's coordinates).

GX lights the colours: COLOR0 by a light for each distant light (far away,
along its direction), one more for a translucent back side, and one for
each point light; COLOR1A1 by two lights at the eye, signed (GX_DF_SIGNED),
one adding where c-81 is positive and one, with a negative attenuation,
where it is negative. The distant lights and the Fresnel colour are what
the programs compute but for GX's 8-bit colours and its unit normals (the
programs use the skinned normal as it comes, which is unit but for a
node's scale and a blend's shortening). A point light's fall-off is not a
form GX has (a quotient of polynomials in the distance): it is fitted about
the draw (gx_lighting_make), and the spot's clamped line becomes a
parabola through the same cut-off, 1 where the axis is.

The reflection's sphere map is the world's (tools/wii_gx_textures.py makes
it of the cube map: a mirror ball looking down the world's z), at the
reflection's direction, which is not linear in the normal. A sphere map of
the view (gx_lighting_sphere_texel) is: drawn each frame from the world's,
it is looked up by GX's texture coordinates from the normal, as the eye
would see a mirror ball straight ahead. The approximation is the eye's
direction: straight ahead for every vertex, where the programs take each
vertex's own.

tests/gx_lighting_check.c compares all of it with the programs on the
interpreter.
*/

#include "gx_lighting.h"
#include "nv2a_cpu_native.h"

#include <math.h>
#include <string.h>

/* ---------- recipes */

struct gx_lighting_recipe
{
	unsigned long hash;
	/* D0's first distant light has a translucent back side (c-82.z) */
	unsigned char translucent;
	/* its point lights (0 or 2): each one's spot direction constant, and
	whether the program takes the cosine with it negated */
	unsigned char point_lights;
	signed char spot_constant[2];
	unsigned char spot_negated[2];
	/* D1 the Fresnel colour */
	unsigned char fresnel;
	/* D0's alpha (_gx_lighting_alpha_...) */
	unsigned char alpha;
	/* texture coordinate 3 the reflection */
	unsigned char reflects;
	/* skinned by v5 and v6 (else its normal's matrix is its position's) */
	unsigned char skinned;
};

static const struct gx_lighting_recipe recipes[] =
{
	/* the models' reflection permutation, skinned */
	{ 0xcfc6f3a4UL, 1, 0, { 0, 0 }, { 0, 0 }, 1, _gx_lighting_alpha_zero, 1, 1 },
	/* the lights permutation */
	{ 0x9d2757d2UL, 1, 2, { -78, -75 }, { 1, 1 }, 1, _gx_lighting_alpha_zero, 1, 1 },
	/* the planar fog permutation */
	{ 0x8f831b43UL, 1, 2, { -78, -75 }, { 1, 1 }, 1, _gx_lighting_alpha_planar_fog, 1, 1 },
	/* the reflection permutation of one node (its matrix c-36) */
	{ 0x71f9f272UL, 1, 0, { 0, 0 }, { 0, 0 }, 0, _gx_lighting_alpha_zero, 0, 0 },
	/* skinned, with lights and the fog's product (its second spot reads the
	first's direction) */
	{ 0x1d3f5b13UL, 0, 2, { -78, -78 }, { 0, 0 }, 0, _gx_lighting_alpha_fog_product, 0, 1 },
	/* in the world's space, with lights and the fog's product */
	{ 0xe6e006c7UL, 0, 2, { -78, -78 }, { 0, 0 }, 0, _gx_lighting_alpha_fog_product, 0, 0 },
};

const struct gx_lighting_recipe *gx_lighting_recipe_find(unsigned long hash)
{
	unsigned index;

	for (index = 0; index < sizeof(recipes) / sizeof(recipes[0]); index++)
		if (recipes[index].hash == hash)
			return &recipes[index];
	return NULL;
}

BOOL gx_lighting_recipe_reflects(const struct gx_lighting_recipe *recipe)
{
	return recipe->reflects;
}

BOOL gx_lighting_recipe_skinned(const struct gx_lighting_recipe *recipe)
{
	return recipe->skinned;
}

/* ---------- vectors */

static float dot3(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float length3(const float *a)
{
	return nv2a_sqrtf(dot3(a, a));
}

/* E's rows applied to a world direction (no translation) */
static void eye_direction(const struct gx_lighting_space *space, const float world[3], float out[3])
{
	int row;

	for (row = 0; row < 3; row++)
		out[row] = dot3(space->world_to_eye[row], world);
}

static void eye_point(const struct gx_lighting_space *space, const float world[3], float out[3])
{
	int row;

	for (row = 0; row < 3; row++)
		out[row] = dot3(space->world_to_eye[row], world) + space->world_to_eye[row][3];
}

/* ---------- the space */

BOOL gx_lighting_space_make(const float (*c)[4], struct gx_lighting_space *space)
{
	const float *scale = c[-38], *offset = c[-37];
	float v[3][4], u0[3], u1[3], e1[3], sigma2, k01;
	int column;

	/* the world's (x w, y w, -w) in pixels, as d3d8_gx.c's position matrix
	makes it of the program's clip-space position (c-96, c-95, c-93) */
	for (column = 0; column < 4; column++)
	{
		v[0][column] = c[-96][column] * scale[0] + (offset[0] + 0.5f) * c[-93][column];
		v[1][column] = c[-95][column] * scale[1] + (offset[1] + 0.5f) * c[-93][column];
		v[2][column] = -c[-93][column];
	}
	sigma2 = dot3(v[2], v[2]);
	if (!(sigma2 > 1.0e-20f))
		return FALSE;
	space->sigma = nv2a_sqrtf(sigma2);
	/* (Gram-Schmidt from the last row up: K's columns) */
	space->k12 = dot3(v[1], v[2]) / sigma2;
	for (column = 0; column < 3; column++)
		u1[column] = v[1][column] - space->k12 * v[2][column];
	space->k11 = length3(u1) / space->sigma;
	if (!(space->k11 > 1.0e-12f))
		return FALSE;
	for (column = 0; column < 3; column++)
		e1[column] = u1[column] / space->k11;
	space->k02 = dot3(v[0], v[2]) / sigma2;
	k01 = dot3(v[0], e1) / sigma2;
	for (column = 0; column < 3; column++)
		u0[column] = v[0][column] - space->k02 * v[2][column] - k01 * e1[column];
	space->k00 = length3(u0) / space->sigma;
	if (!(space->k00 > 1.0e-12f) || fabsf(k01) > 1.0e-3f * space->k00)
		return FALSE;
	gx_lighting_space_rows(space, (const float (*)[4])v, space->world_to_eye);
	eye_point(space, c[-92], space->eye);
	return TRUE;
}

void gx_lighting_space_rows(const struct gx_lighting_space *space, const float in[3][4], float out[3][4])
{
	float rows[3][4];
	int column;

	for (column = 0; column < 4; column++)
	{
		rows[0][column] = (in[0][column] - space->k02 * in[2][column]) / space->k00;
		rows[1][column] = (in[1][column] - space->k12 * in[2][column]) / space->k11;
		rows[2][column] = in[2][column];
	}
	memcpy(out, rows, sizeof(rows));
}

void gx_lighting_space_projection(const struct gx_lighting_space *space, float width, float height, float a, float b,
	float out[4][4])
{
	memset(out, 0, 16 * sizeof(float));
	out[0][0] = 2.0f / width * space->k00;
	out[0][2] = 2.0f / width * space->k02 + 1.0f;
	out[1][1] = -2.0f / height * space->k11;
	out[1][2] = -2.0f / height * space->k12 - 1.0f;
	out[2][2] = 1.0f - a;
	out[2][3] = b;
	out[3][2] = -1.0f;
}

/* ---------- lights */

/* the point lights' fall-off, max(1 - d^2/r^2, 0), as GX's 1 / (k0 + k1 d +
k2 d^2): its reciprocal's second-order expansion about the draw's distance
(exact there, and close about it), the distance taken no further than this
much of the radius, where the fall-off's reciprocal grows too fast */
#define FALL_OFF_FARTHEST 0.85f
/* (and where the draw is beyond the radius: this fit of the whole, for
x = d / r: 0.86 / (1 - 1.75 x + 4.6 x^2)) */
static const float fall_off_whole[3] = { 0.86f, -1.75f, 4.6f };

static void fall_off_fit(float radius, float distance, float *scale, float k[3])
{
	float x = distance / radius, x0, u, q0, q1, q2, r2 = radius * radius;

	if (x > 1.0f)
	{
		*scale = fall_off_whole[0];
		k[0] = 1.0f;
		k[1] = fall_off_whole[1] / radius;
		k[2] = fall_off_whole[2] / r2;
		return;
	}
	x0 = x < FALL_OFF_FARTHEST ? x : FALL_OFF_FARTHEST;
	distance = x0 * radius;
	u = x0 * x0;
	/* q = 1 / (1 - d^2/r^2), and its first two derivatives in d */
	q0 = 1.0f / (1.0f - u);
	q1 = 2.0f * distance / r2 * q0 * q0;
	q2 = 2.0f / r2 * q0 * q0 + 8.0f * distance * distance / (r2 * r2) * q0 * q0 * q0;
	*scale = 1.0f;
	k[0] = q0 - q1 * distance + 0.5f * q2 * distance * distance;
	k[1] = q1 - q2 * distance;
	k[2] = 0.5f * q2;
	/* (a parabola that dips below 1 nearer the light, more than the light
	itself: the whole's fit) */
	if (k[1] < 0.0f && k[0] - k[1] * k[1] / (4.0f * k[2]) < 1.0f)
	{
		*scale = fall_off_whole[0];
		k[0] = 1.0f;
		k[1] = fall_off_whole[1] / radius;
		k[2] = fall_off_whole[2] / r2;
	}
}

/* a light: its colour (any size, any sign) made GX's 8-bit colour and a
scale of its attenuation, a negative one by a negative k */
static BOOL light_add(struct gx_lighting *lighting, int channel, const float position[3], const float direction[3],
	const float color[4], float scale, const float a[3], const float k[3])
{
	struct gx_lighting_light *light;
	float most = 0.0f;
	int component;

	for (component = 0; component < 4; component++)
		if (fabsf(color[component]) > most)
			most = fabsf(color[component]);
	if (!(most > 1.0e-6f) || scale == 0.0f)
		return TRUE;
	if (lighting->light_count >= GX_LIGHTING_LIGHTS)
		return FALSE;
	light = &lighting->lights[lighting->light_count];
	memcpy(light->position, position, sizeof(light->position));
	memcpy(light->direction, direction, sizeof(light->direction));
	for (component = 0; component < 4; component++)
	{
		float value = color[component] / most * 255.0f + 0.5f;

		/* (the caller's colours are of one sign) */
		light->color[component] = (unsigned char)(value < 0.0f ? 0 : value > 255.0f ? 255 : (int)value);
	}
	for (component = 0; component < 3; component++)
	{
		light->a[component] = a[component] * most * fabsf(scale);
		light->k[component] = scale < 0.0f ? -k[component] : k[component];
	}
	lighting->masks[channel] |= (unsigned char)(1u << lighting->light_count);
	lighting->light_count++;
	return TRUE;
}

/* a distant light, toward -c[direction], of colour c[color] (rgb): far along
its direction from center, scaled by the direction's length (the programs'
n.l is with the direction as it is) */
static BOOL distant_light(struct gx_lighting *lighting, const float (*c)[4], const struct gx_lighting_space *space,
	const float center[3], int direction_constant, int color_constant, float back)
{
	static const float none[3] = { 0.0f, 0.0f, 1.0f }, k[3] = { 1.0f, 0.0f, 0.0f };
	float toward[3], eye[3], position[3], length, eye_length, a[3] = { 0.0f, 0.0f, 0.0f }, color[4];
	int component;

	for (component = 0; component < 3; component++)
		toward[component] = -c[direction_constant][component];
	length = length3(toward);
	if (!(length > 1.0e-12f))
		return TRUE;
	eye_direction(space, toward, eye);
	eye_length = length3(eye);
	for (component = 0; component < 3; component++)
	{
		color[component] = c[color_constant][component];
		/* (a far point: GX normalizes its direction to each vertex) */
		position[component] = center[component] + eye[component] / eye_length * 1.0e6f * (back != 0.0f ? -1.0f : 1.0f);
	}
	color[3] = 0.0f;
	a[0] = length;
	return light_add(lighting, 0, position, none, color, back != 0.0f ? back : 1.0f, a, k);
}

static BOOL point_light(struct gx_lighting *lighting, const float (*c)[4], const struct gx_lighting_space *space,
	const float center[3], const struct gx_lighting_recipe *recipe, int which)
{
	int position_constant = which ? -76 : -79, color_constant = which ? -74 : -77;
	float scale = c[which ? -75 : -78][3], offset = c[color_constant][3];
	float position[3], forward[3], direction[3], color[4], a[3], k[3], radius, distance, fit_scale, length, offset_e[3];
	int component;

	for (component = 0; component < 3; component++)
		color[component] = c[color_constant][component];
	color[3] = 0.0f;
	if (!(c[position_constant][3] > 0.0f) || (color[0] == 0.0f && color[1] == 0.0f && color[2] == 0.0f))
		return TRUE;
	eye_point(space, c[position_constant], position);
	/* (GX_InitLightDir's direction is negated: GX's cosine is the vertex's
	direction to the light with it) */
	for (component = 0; component < 3; component++)
		forward[component] = recipe->spot_negated[which] ? c[recipe->spot_constant[which]][component] :
			-c[recipe->spot_constant[which]][component];
	eye_direction(space, forward, direction);
	length = length3(direction);
	for (component = 0; component < 3; component++)
		direction[component] = length > 1.0e-12f ? direction[component] / length * length3(forward) : 0.0f;
	/* the spot: clamp(cos scale + offset, 0, 1) as a parabola 0 at the
	cut-off, 1 with no slope on the axis */
	if (scale == 0.0f)
	{
		a[0] = offset < 0.0f ? 0.0f : offset > 1.0f ? 1.0f : offset;
		a[1] = a[2] = 0.0f;
	}
	else
	{
		float cut_off = -offset / scale, d = 1.0f - cut_off;

		if (scale < 0.0f)
			return FALSE;
		if (!(d > 1.0e-4f))
			return TRUE;
		a[0] = 1.0f - 1.0f / (d * d);
		a[1] = 2.0f / (d * d);
		a[2] = -1.0f / (d * d);
	}
	/* the fall-off, in E (whose lengths are sigma times the world's) */
	radius = space->sigma / nv2a_sqrtf(c[position_constant][3]);
	for (component = 0; component < 3; component++)
		offset_e[component] = center[component] - position[component];
	distance = length3(offset_e);
	fall_off_fit(radius, distance, &fit_scale, k);
	for (component = 0; component < 3; component++)
		a[component] *= fit_scale;
	return light_add(lighting, 0, position, direction, color, 1.0f, a, k);
}

BOOL gx_lighting_make(const struct gx_lighting_recipe *recipe, const float (*c)[4],
	const struct gx_lighting_space *space, const float center[3], struct gx_lighting *lighting)
{
	int component, which;

	memset(lighting, 0, sizeof(*lighting));
	for (component = 0; component < 3; component++)
	{
		float value = c[-69][component] * 255.0f + 0.5f;

		lighting->ambient[0][component] = (unsigned char)(value < 0.0f ? 0 : value > 255.0f ? 255 : (int)value);
	}
	lighting->ambient[0][3] = 255;
	/* D0: the distant lights (the first's translucent back side: max(n.l,
	-n.l t) is max(n.l, 0) + t max(-n.l, 0) for t of 0 to 1) */
	if (!distant_light(lighting, c, space, center, -73, -72, 0.0f))
		return FALSE;
	if (recipe->translucent)
	{
		float back = c[-82][2];

		if (back < 0.0f || back > 1.0f)
			return FALSE;
		if (back > 0.0f && !distant_light(lighting, c, space, center, -73, -72, back))
			return FALSE;
	}
	if (!distant_light(lighting, c, space, center, -71, -70, 0.0f))
		return FALSE;
	for (which = 0; which < recipe->point_lights; which++)
		if (!point_light(lighting, c, space, center, recipe, which))
			return FALSE;
	/* D1: c-80 + c-81 (n.e) */
	lighting->fresnel = recipe->fresnel;
	if (recipe->fresnel)
	{
		static const float none[3] = { 0.0f, 0.0f, 1.0f }, a[3] = { 1.0f, 0.0f, 0.0f }, k[3] = { 1.0f, 0.0f, 0.0f };
		float up[4], down[4];

		for (component = 0; component < 4; component++)
		{
			float base = c[-80][component] * 255.0f + 0.5f;

			if (c[-80][component] < 0.0f || c[-80][component] > 1.0f)
				return FALSE;
			lighting->ambient[1][component] = (unsigned char)(int)base;
			up[component] = c[-81][component] > 0.0f ? c[-81][component] : 0.0f;
			down[component] = c[-81][component] < 0.0f ? -c[-81][component] : 0.0f;
		}
		if (!light_add(lighting, 1, space->eye, none, up, 1.0f, a, k) ||
			!light_add(lighting, 1, space->eye, none, down, -1.0f, a, k))
			return FALSE;
	}
	lighting->alpha = recipe->alpha;
	memcpy(lighting->fog_rows[0], c[-87], sizeof(lighting->fog_rows[0]));
	memcpy(lighting->fog_rows[1], c[-86], sizeof(lighting->fog_rows[1]));
	memcpy(lighting->fog_rows[2], c[-88], sizeof(lighting->fog_rows[2]));
	memcpy(lighting->fog, c[-85], sizeof(lighting->fog));
	lighting->reflection_scale = c[-92][3];
	lighting->lit = TRUE;
	return TRUE;
}

/* ---------- GX's lighting, as Dolphin computes it (for the host check) */

void gx_lighting_evaluate(const struct gx_lighting *lighting, const float position[3], const float normal[3],
	unsigned char d0[4], unsigned char d1[4])
{
	float n[3], length = length3(normal);
	int channel, index, component;

	for (component = 0; component < 3; component++)
		n[component] = length > 0.0f ? normal[component] / length : 0.0f;
	for (channel = 0; channel < 2; channel++)
	{
		int total[4];
		unsigned char *out = channel ? d1 : d0;

		for (component = 0; component < 4; component++)
			total[component] = lighting->ambient[channel][component];
		for (index = 0; index < (int)lighting->light_count; index++)
		{
			const struct gx_lighting_light *light = &lighting->lights[index];
			float to[3], distance2, distance, cosine, attenuation, diffuse;

			if (!(lighting->masks[channel] & (1u << index)))
				continue;
			for (component = 0; component < 3; component++)
				to[component] = light->position[component] - position[component];
			distance2 = dot3(to, to);
			distance = nv2a_sqrtf(distance2);
			for (component = 0; component < 3; component++)
				to[component] /= distance;
			/* (the hardware's direction is GX_InitLightDir's negated) */
			cosine = -dot3(to, light->direction);
			cosine = cosine > 0.0f ? cosine : 0.0f;
			attenuation = light->a[0] + light->a[1] * cosine + light->a[2] * cosine * cosine;
			attenuation = (attenuation > 0.0f ? attenuation : 0.0f) /
				(light->k[0] + light->k[1] * distance + light->k[2] * distance2);
			diffuse = dot3(to, n);
			if (!channel && diffuse < 0.0f)
				diffuse = 0.0f;
			for (component = 0; component < 4; component++)
				total[component] += (int)lroundf(attenuation * diffuse * (float)light->color[component]);
		}
		for (component = 0; component < 4; component++)
		{
			int value = total[component] < 0 ? 0 : total[component] > 255 ? 255 : total[component];

			/* (the material colour, white, times it) */
			out[component] = (unsigned char)((255 * (value + (value >> 7))) >> 8);
		}
	}
}

/* ---------- the planar fog */

static float clamp01(float value)
{
	return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

float gx_lighting_planar_fog(const struct gx_lighting *lighting, const float world[3])
{
	float u = 1.0f - (dot3(lighting->fog_rows[0], world) + lighting->fog_rows[0][3]);
	float v = 1.0f - (dot3(lighting->fog_rows[1], world) + lighting->fog_rows[1][3]);
	float x, y, a, b;

	u = u > 0.0f ? u : 0.0f;
	v = v > 0.0f ? v : 0.0f;
	u = u * u;
	v = v * v;
	u = u < 1.0f ? u : 1.0f;
	v = v < 1.0f ? v : 1.0f;
	x = u + v;
	x = 1.0f - (x < 1.0f ? x : 1.0f);
	y = 1.0f - v;
	a = x * x;
	b = y * y;
	a = lighting->fog[1] * (b - a) + a;
	a *= lighting->fog[2];
	if (lighting->alpha == _gx_lighting_alpha_fog_product)
	{
		float distance = clamp01(dot3(lighting->fog_rows[2], world) + lighting->fog_rows[2][3]) * lighting->fog[0];

		return clamp01((1.0f - distance) * (1.0f - a));
	}
	return clamp01(a);
}

/* ---------- skinning */

static const float *constant_at(const float (*c)[4], int index)
{
	/* (as NV2A's a0-relative reads, kept in the 192) */
	index += 96;
	index = index < 0 ? 0 : index > 191 ? 191 : index;
	return c[index - 96];
}

static int arl_index(float x)
{
	return (int)floorf(x + 0.001f);
}

void gx_lighting_skin(const float (*c)[4], const float nodes[4], const float weights[4], float out[3][4])
{
	int first = arl_index(nodes[0] * c[-89][3]), second = arl_index(nodes[1] * c[-89][3]);
	float rest = weights[3] - weights[0];
	int row, column;

	for (row = 0; row < 3; row++)
	{
		const float *a = constant_at(c, first - 36 + row), *b = constant_at(c, second - 36 + row);

		for (column = 0; column < 4; column++)
			out[row][column] = rest * b[column] + weights[0] * a[column];
	}
}

/* ---------- the sphere maps */

/* 1 / sqrt(x) for x > 0, to about a millionth: frsqrte's estimate and two
of Newton's steps (the sphere maps' grids, a few hundred points each: their
library square roots were most of a grid's time) */
static float reciprocal_root(float x)
{
	double y = nv2a_reciprocal_root_estimate(x);

	y = y * (1.5 - 0.5 * x * y * y);
	y = y * (1.5 - 0.5 * x * y * y);
	return (float)y;
}

void gx_lighting_sphere_coordinates(const float direction[3], float out[2])
{
	float x = direction[0], y = direction[1], z = direction[2], length2 = x * x + y * y + z * z, scale, m2;

	if (!(length2 > 1.0e-24f))
	{
		out[0] = out[1] = 0.5f;
		return;
	}
	scale = reciprocal_root(length2);
	x *= scale;
	y *= scale;
	z *= scale;
	m2 = x * x + y * y + (z + 1.0f) * (z + 1.0f);
	if (!(m2 > 0.25e-12f))
	{
		out[0] = 0.5f;
		out[1] = 1.0f;
		return;
	}
	/* (x / m + 1/2, m twice the root) */
	scale = 0.5f * reciprocal_root(m2);
	out[0] = x * scale + 0.5f;
	out[1] = y * scale + 0.5f;
}

void gx_lighting_view_frame(const struct gx_lighting_space *space, const float toward[3], float step, float frame[3][3],
	float turn[3][3])
{
	float z[3], x[3], y[3], length, k;
	int row, column;

	length = nv2a_sqrtf(dot3(toward, toward));
	if (!(length > 1.0e-12f) || !(toward[2] > 0.0f))
	{
		z[0] = z[1] = 0.0f;
		z[2] = 1.0f;
	}
	else if (step > 0.0f)
	{
		/* (its angles about E's y and x, to the nearest step) */
		z[0] = tanf(step * floorf(atan2f(toward[0], toward[2]) / step + 0.5f));
		z[1] = tanf(step * floorf(atan2f(toward[1], toward[2]) / step + 0.5f));
		z[2] = 1.0f;
		length = nv2a_sqrtf(dot3(z, z));
		for (row = 0; row < 3; row++)
			z[row] /= length;
	}
	else
		for (row = 0; row < 3; row++)
			z[row] = toward[row] / length;
	/* (the turn of E's z to it about their common perpendicular: E's x and y
	turned with it) */
	k = 1.0f / (1.0f + z[2]);
	x[0] = 1.0f - z[0] * z[0] * k;
	x[1] = -z[0] * z[1] * k;
	x[2] = -z[0];
	y[0] = -z[0] * z[1] * k;
	y[1] = 1.0f - z[1] * z[1] * k;
	y[2] = -z[1];
	if (turn)
	{
		memcpy(turn[0], x, sizeof(x));
		memcpy(turn[1], y, sizeof(y));
		memcpy(turn[2], z, sizeof(z));
	}
	for (column = 0; column < 3; column++)
	{
		float e0 = space->world_to_eye[0][column] / space->sigma, e1 = space->world_to_eye[1][column] / space->sigma,
			e2 = space->world_to_eye[2][column] / space->sigma;

		frame[0][column] = x[0] * e0 + x[1] * e1 + x[2] * e2;
		frame[1][column] = y[0] * e0 + y[1] * e1 + y[2] * e2;
		frame[2][column] = z[0] * e0 + z[1] * e1 + z[2] * e2;
	}
}

void gx_lighting_sphere_point(float s, float t, float point[3])
{
	float x = 2.0f * s - 1.0f, y = 2.0f * t - 1.0f, z, r2 = x * x + y * y;

	if (r2 >= 1.0f)
	{
		float scale = reciprocal_root(r2);

		x *= scale;
		y *= scale;
		z = 0.0f;
	}
	else
		z = (1.0f - r2) * reciprocal_root(1.0f - r2);
	point[0] = x;
	point[1] = y;
	point[2] = z;
}

void gx_lighting_sphere_texel(const float frame[3][3], float reflection_scale, float s, float t, float out[2])
{
	float point[3];

	gx_lighting_sphere_point(s, t, point);
	gx_lighting_sphere_point_texel(frame, reflection_scale, point, out);
}

void gx_lighting_sphere_point_texel(const float frame[3][3], float reflection_scale, const float point[3], float out[2])
{
	float x = point[0], y = point[1], z = point[2], reflection[3], world[3];
	int row, column;

	/* (the eye along the frame's z) */
	reflection[0] = reflection_scale * z * x;
	reflection[1] = reflection_scale * z * y;
	reflection[2] = reflection_scale * z * z - 1.0f;
	/* (the frame's rows are a rotation's: its transpose turns it back) */
	for (column = 0; column < 3; column++)
	{
		world[column] = 0.0f;
		for (row = 0; row < 3; row++)
			world[column] += reflection[row] * frame[row][column];
	}
	gx_lighting_sphere_coordinates(world, out);
}
