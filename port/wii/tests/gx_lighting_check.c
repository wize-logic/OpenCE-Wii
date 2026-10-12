/*
GX_LIGHTING_CHECK.C

A host check of the models' lighting on GX's lights (port/wii/src/gx_lighting.c;
tools/wii_nv2a_check.py builds and runs it): each program gx_lighting.c has a
recipe for is run on the interpreter (port/wii/src/nv2a_cpu.c) for random
scenes (a camera, a model of skinned nodes in front of it, lights about it,
the reflection's and fogs' constants) and vertices, and what GX would make of
the same vertex (the lights gx_lighting_make sets, lit as Dolphin lights them:
gx_lighting_evaluate) is compared with the program's outputs:
- D0's colour, apart for scenes with and without point lights (the distant
  lights and ambient are the programs' but for GX's unit normal and 8-bit
  colours; a point light's fall-off and spot are fitted);
- D1, the reflection's Fresnel colour;
- D0's alpha, the planar fog's or its product with the atmospheric fog's
  (the processor's, per vertex), and the same from GX's ramp
  (gx_lighting_fog_ramp, read at the coordinates the post-transform matrix
  makes of the position in E, filtered as GX filters it), at the vertices
  and halfway between two (where the programs' is the mean of the two, GX's
  the fog's there); the product's atmospheric distance (c-88) is of the
  planar fog's distance's direction (c-86), as the game makes it;
- the reflection's direction: the program's, against the one the view's
  sphere map holds at the texture coordinates GX makes of the normal (the
  eye taken straight ahead), in degrees;
- the position through E and the projection times K, against the program's
  screen position, in pixels.
It prints each one's mean and largest error, 0 to 255 for the colours, and
fails where the colours that should be close are not.

usage: gx_lighting_check microcode.inc
*/

#include "nv2a_cpu_native.h"
#include "gx_lighting.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENES 400
#define VERTICES 200
#define NODES 4
#define PI 3.14159265f
/* the view frames' rounding (d3d8_gx.c's) */
/* the view's sphere map's grid (d3d8_gx.c's sphere_bake) */
#define GRID 24
#define REFLECTION_STEP (15.0f * PI / 180.0f)

static float uniform(float low, float high)
{
	return low + (high - low) * (float)rand() / (float)RAND_MAX;
}

static void random_unit(float out[3])
{
	float length;

	do
	{
		out[0] = uniform(-1.0f, 1.0f);
		out[1] = uniform(-1.0f, 1.0f);
		out[2] = uniform(-1.0f, 1.0f);
		length = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	} while (length < 0.1f || length > 1.0f);
	out[0] /= length;
	out[1] /= length;
	out[2] /= length;
}

static float dot3(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float *a, const float *b, float *out)
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static void normalize3(float *a)
{
	float length = sqrtf(dot3(a, a));

	a[0] /= length;
	a[1] /= length;
	a[2] /= length;
}

/* a random rotation's rows */
static void random_rotation(float rows[3][3])
{
	float helper[3];

	random_unit(rows[0]);
	random_unit(helper);
	cross3(rows[0], helper, rows[1]);
	normalize3(rows[1]);
	cross3(rows[0], rows[1], rows[2]);
}

/* rows turned about a random axis by up to most radians */
static void turned(const float rows[3][3], float most, float out[3][3])
{
	float axis[3], angle = uniform(0.0f, most), s = sinf(angle), c = cosf(angle), turn[3][3];
	int row, column, k;

	random_unit(axis);
	for (row = 0; row < 3; row++)
		for (column = 0; column < 3; column++)
			turn[row][column] = (row == column ? c : 0.0f) + (1.0f - c) * axis[row] * axis[column];
	turn[0][1] -= s * axis[2];
	turn[0][2] += s * axis[1];
	turn[1][0] += s * axis[2];
	turn[1][2] -= s * axis[0];
	turn[2][0] -= s * axis[1];
	turn[2][1] += s * axis[0];
	for (row = 0; row < 3; row++)
		for (column = 0; column < 3; column++)
		{
			out[row][column] = 0.0f;
			for (k = 0; k < 3; k++)
				out[row][column] += turn[row][k] * rows[k][column];
		}
}

static float clamp_unit(float value)
{
	return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

static unsigned char color_byte(float value)
{
	return (unsigned char)(value <= 0.0f ? 0 : value >= 1.0f ? 255 : (int)(value * 255.0f + 0.5f));
}

/* a normal as the maps' 11:11:10 words hold it, and as the device reads them */
static float packed(float value, float most)
{
	long whole = lroundf(value * most);
	float result = (float)whole * (1.0f / most);

	return result < -1.0f ? -1.0f : result;
}

struct statistic
{
	double sum, largest;
	unsigned long count;
};

static void add(struct statistic *statistic, double error)
{
	error = fabs(error);
	statistic->sum += error;
	statistic->count++;
	if (error > statistic->largest)
		statistic->largest = error;
}

static double mean(const struct statistic *statistic)
{
	return statistic->count ? statistic->sum / statistic->count : 0.0;
}

static void report(const char *name, const struct statistic *statistic, const char *unit)
{
	printf("  %-46s mean %8.3f, largest %8.3f %s (%lu)\n", name, mean(statistic), statistic->largest, unit,
		statistic->count);
}

/* the program words of the hash, from the microcode */
static int program_find(const DWORD *words, unsigned long word_count, unsigned long hash, struct nv2a_cpu_program *program)
{
	unsigned long position;

	for (position = 0; position < word_count; )
	{
		unsigned long count = words[position] >> 16;
		const DWORD *instructions = words + position + 1;

		position += 1 + count * 4;
		if (!count || position > word_count)
			continue;
		if (nv2a_cpu_hash(instructions, count) == hash)
		{
			nv2a_cpu_compile(instructions, count, program);
			return 1;
		}
	}
	return 0;
}

static const unsigned long programs[] =
{
	0xcfc6f3a4UL, 0x9d2757d2UL, 0x8f831b43UL, 0x71f9f272UL, 0x1d3f5b13UL, 0xe6e006c7UL,
};

struct results
{
	/* (the reflection with the eye straight ahead, along the draw's
	direction rounded to REFLECTION_STEP, and along the draw's direction) */
	struct statistic d0, d0_points, d1, alpha, reflection[3], texel, grid, position;
	/* the ramp's alpha at the vertices, halfway between two, and its error
	against the fog's own there */
	struct statistic ramp, ramp_between, ramp_exact;
	unsigned long scenes, refused, ramp_refused, ramp_rows;
};

static void check_program(const struct nv2a_cpu_program *program, struct results *results)
{
	const struct gx_lighting_recipe *recipe = gx_lighting_recipe_find(program->hash);
	int skinned = gx_lighting_recipe_skinned(recipe);
	int scene;

	for (scene = 0; scene < SCENES; scene++)
	{
		static float constants[NV2A_CPU_CONSTANT_COUNT][4];
		float (*c)[4] = constants + NV2A_CPU_CONSTANT_BIAS;
		float view[3][3], eye[3], center[3], model[3], fovy = uniform(0.9f, 1.3f), aspect = 4.0f / 3.0f;
		float nodes[NODES][3][4], w2v[3][4], center_e[3], base[3][3], frames[3][3][3];
		float near_plane = 0.06f, far_plane = 1000.0f;
		int points = 0, node, row, column, vertex, light;
		struct gx_lighting_space space;
		struct gx_lighting lighting;
		float projection[4][4];
		int has_points;
		static unsigned char ramp[GX_LIGHTING_FOG_ROWS * GX_LIGHTING_FOG_SIZE];
		struct gx_lighting_fog_form form;
		float ramp_matrix[3][4], last_coordinates[2] = { 0.0f, 0.0f }, last_world[3] = { 0.0f, 0.0f, 0.0f };
		float last_alpha = 0.0f;
		int ramped = 0;

		memset(constants, 0, sizeof(constants));
		/* the camera: looking along -view[2] from the eye */
		random_rotation(view);
		for (row = 0; row < 3; row++)
			eye[row] = uniform(-50.0f, 50.0f);
		/* the model: somewhere in view, 1.5 to 25 units away */
		{
			float distance = uniform(1.5f, 25.0f), ahead[3], x = uniform(-0.8f, 0.8f), y = uniform(-0.8f, 0.8f);

			for (row = 0; row < 3; row++)
			{
				ahead[row] = -view[2][row] + x * tanf(fovy / 2) * aspect * view[0][row] + y * tanf(fovy / 2) * view[1][row];
			}
			normalize3(ahead);
			for (row = 0; row < 3; row++)
				model[row] = eye[row] + ahead[row] * distance;
		}
		/* world to clip (c-96 to c-93): D3D's perspective of the view */
		{
			float cot = 1.0f / tanf(fovy / 2), translation[3];

			for (row = 0; row < 3; row++)
				translation[row] = -dot3(view[row], eye);
			for (column = 0; column < 3; column++)
			{
				c[-96][column] = cot / aspect * view[0][column];
				c[-95][column] = cot * view[1][column];
				c[-94][column] = -far_plane / (far_plane - near_plane) * view[2][column];
				c[-93][column] = -view[2][column];
			}
			c[-96][3] = cot / aspect * translation[0];
			c[-95][3] = cot * translation[1];
			c[-94][3] = -far_plane / (far_plane - near_plane) * translation[2] - far_plane * near_plane / (far_plane - near_plane);
			c[-93][3] = -translation[2];
		}
		/* the viewport (c-38 scale, c-37 offset) */
		c[-38][0] = 320.0f;
		c[-38][1] = -240.0f;
		c[-38][2] = 16777215.0f;
		c[-37][0] = 320.0f;
		c[-37][1] = 240.0f;
		memcpy(c[-92], eye, sizeof(eye));
		c[-92][3] = 2.0f;
		c[-89][3] = 765.0f;
		c[-84][0] = c[-84][1] = uniform(1.0f, 8.0f);
		c[-84][2] = c[-84][3] = 1.0f;
		c[-83][0] = 1.0f;
		c[-82][1] = 1.0f;
		c[-82][2] = rand() % 3 ? 0.0f : uniform(0.0f, 0.6f);
		/* the Fresnel colours: perpendicular less parallel, and parallel */
		for (row = 0; row < 4; row++)
		{
			float parallel = uniform(0.0f, 1.0f), perpendicular = uniform(0.0f, 1.0f);

			c[-81][row] = perpendicular - parallel;
			c[-80][row] = parallel;
		}
		/* the nodes: turned, about the model, each within 40 degrees of the
		model's own turn (a blend's nodes are a skeleton's neighbours) */
		random_rotation(base);
		for (node = 0; node < NODES; node++)
		{
			float rotation[3][3];

			turned(base, 0.7f, rotation);
			for (row = 0; row < 3; row++)
			{
				for (column = 0; column < 3; column++)
					nodes[node][row][column] = rotation[column][row];
				nodes[node][row][3] = model[row] + uniform(-0.4f, 0.4f);
				memcpy(c[-36 + 3 * node + row], nodes[node][row], sizeof(nodes[node][row]));
			}
		}
		/* the lights: two distant, ambient, and (half of the scenes) two point
		lights about the model */
		for (light = 0; light < 2; light++)
		{
			float direction[3];

			random_unit(direction);
			memcpy(c[light ? -71 : -73], direction, sizeof(direction));
			for (row = 0; row < 3; row++)
				c[light ? -70 : -72][row] = uniform(0.0f, light ? 0.6f : 1.2f);
		}
		for (row = 0; row < 3; row++)
			c[-69][row] = uniform(0.0f, 0.35f);
		has_points = recipe && scene % 2 == 1;
		for (light = 0; light < 2 && has_points; light++)
		{
			float offset[3], forward[3], radius = uniform(0.8f, 6.0f);
			int base = light ? -76 : -79;

			random_unit(offset);
			random_unit(forward);
			for (row = 0; row < 3; row++)
				c[base][row] = model[row] + offset[row] * uniform(0.3f, 1.1f) * radius;
			c[base][3] = 1.0f / (radius * radius);
			memcpy(c[base + 1], forward, sizeof(forward));
			for (row = 0; row < 3; row++)
				c[base + 2][row] = uniform(0.0f, 1.0f);
			if (rand() % 2)
			{
				/* a spot: its cut-off and fall-off cosines */
				float cut_off = uniform(0.3f, 0.9f), fall_off = cut_off + uniform(0.03f, 1.0f - cut_off);

				c[base + 1][3] = 1.0f / (fall_off - cut_off);
				c[base + 2][3] = -c[base + 1][3] * cut_off;
			}
			else
			{
				c[base + 1][3] = 0.0f;
				c[base + 2][3] = 1.0f;
			}
			points++;
		}
		if (!has_points)
		{
			c[-78][3] = c[-75][3] = 0.0f;
			c[-77][3] = c[-74][3] = 1.0f;
		}
		/* the fogs: planes in the world, the atmospheric distance */
		for (row = 0; row < 3; row++)
		{
			float plane[3];

			random_unit(plane);
			for (column = 0; column < 3; column++)
				c[-88 + row][column] = plane[column] * uniform(0.0f, 0.3f);
			c[-88 + row][3] = -dot3(c[-88 + row], model) + uniform(-0.5f, 1.5f);
		}
		c[-85][0] = uniform(0.0f, 1.0f);
		c[-85][1] = uniform(0.0f, 1.0f);
		c[-85][2] = uniform(0.0f, 1.0f);
		if (program->hash == 0x1d3f5b13UL || program->hash == 0xe6e006c7UL)
		{
			/* (the atmospheric fog's distance along the planar fog's: the
			camera's forward over each one's range, r = alpha t + beta, beta
			the atmospheric fog's least distance over its range, negated) */
			float alpha = uniform(0.2f, 4.0f), beta = uniform(-3.0f, 0.5f);

			for (column = 0; column < 3; column++)
				c[-88][column] = alpha * c[-86][column];
			c[-88][3] = beta + alpha * c[-86][3];
		}

		results->scenes++;
		if (!gx_lighting_space_make((const float (*)[4])c, &space))
		{
			results->refused++;
			continue;
		}
		/* (what the device's position matrices hold: the world's (x w, y w,
		-w) of a node's matrix, as gx_lighting_space_make makes the world's) */
		for (column = 0; column < 4; column++)
		{
			w2v[0][column] = c[-96][column] * c[-38][0] + (c[-37][0] + 0.5f) * c[-93][column];
			w2v[1][column] = c[-95][column] * c[-38][1] + (c[-37][1] + 0.5f) * c[-93][column];
			w2v[2][column] = -c[-93][column];
		}
		for (row = 0; row < 3; row++)
			center_e[row] = dot3(space.world_to_eye[row], model) + space.world_to_eye[row][3];
		if (!gx_lighting_make(recipe, (const float (*)[4])c, &space, center_e, &lighting))
		{
			results->refused++;
			continue;
		}
		if (lighting.alpha != _gx_lighting_alpha_zero)
		{
			ramped = gx_lighting_fog_form(&lighting, &form) &&
				gx_lighting_fog_matrix(&lighting, &form, &space, center_e, ramp_matrix);
			results->ramp_refused += !ramped;
			if (ramped)
			{
				gx_lighting_fog_ramp(&form, 0, form.rows, ramp);
				results->ramp_rows += form.rows;
			}
		}
		gx_lighting_space_projection(&space, 640.0f, 480.0f, 0.0f, 1.0f, projection);
		{
			float toward[3], ahead[3] = { 0.0f, 0.0f, 1.0f };

			for (row = 0; row < 3; row++)
				toward[row] = space.eye[row] - center_e[row];
			gx_lighting_view_frame(&space, ahead, 0.0f, frames[0], NULL);
			gx_lighting_view_frame(&space, toward, REFLECTION_STEP, frames[1], NULL);
			gx_lighting_view_frame(&space, toward, 0.0f, frames[2], NULL);
		}
		(void)center;

		for (vertex = 0; vertex < VERTICES; vertex++)
		{
			float inputs[NV2A_CPU_INPUT_COUNT][4], matrix[3][4], v_rows[3][4], e_rows[3][4], world[3], normal[3];
			float position_e[3], normal_e[3];
			struct nv2a_cpu_output out;
			unsigned char d0[4], d1[4];
			int first = rand() % NODES, second = rand() % NODES, component;
			float weight = rand() % 3 ? 1.0f : uniform(0.0f, 1.0f);

			memset(inputs, 0, sizeof(inputs));
			for (row = 0; row < NV2A_CPU_INPUT_COUNT; row++)
				inputs[row][3] = 1.0f;
			for (row = 0; row < 3; row++)
				inputs[0][row] = skinned || program->hash == 0x71f9f272UL ? uniform(-0.5f, 0.5f) : model[row] + uniform(-0.5f, 0.5f);
			random_unit(normal);
			inputs[1][0] = packed(normal[0], 1023.0f);
			inputs[1][1] = packed(normal[1], 1023.0f);
			inputs[1][2] = packed(normal[2], 511.0f);
			inputs[4][0] = uniform(0.0f, 1.0f);
			inputs[4][1] = uniform(0.0f, 1.0f);
			inputs[5][0] = (float)first / 255.0f;
			inputs[5][1] = (float)second / 255.0f;
			inputs[6][0] = (float)lroundf(weight * 32767.0f) / 32767.0f;
			nv2a_cpu_run(program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &out);

			/* GX: the vertex's matrix (its node, or its blend), through E */
			if (skinned)
				gx_lighting_skin((const float (*)[4])c, inputs[5], inputs[6], matrix);
			else if (program->hash == 0x71f9f272UL)
				memcpy(matrix, c[-36], sizeof(matrix));
			else
			{
				memset(matrix, 0, sizeof(matrix));
				matrix[0][0] = matrix[1][1] = matrix[2][2] = 1.0f;
			}
			for (row = 0; row < 3; row++)
				for (column = 0; column < 4; column++)
					v_rows[row][column] = w2v[row][0] * matrix[0][column] + w2v[row][1] * matrix[1][column] +
						w2v[row][2] * matrix[2][column] + (column == 3 ? w2v[row][3] : 0.0f);
			gx_lighting_space_rows(&space, (const float (*)[4])v_rows, e_rows);
			for (row = 0; row < 3; row++)
			{
				position_e[row] = dot3(e_rows[row], inputs[0]) + e_rows[row][3];
				normal_e[row] = dot3(e_rows[row], inputs[1]);
				world[row] = dot3(matrix[row], inputs[0]) + matrix[row][3];
			}
			/* (the position: GX's projection of E against the program's) */
			{
				float x = projection[0][0] * position_e[0] + projection[0][2] * position_e[2];
				float y = projection[1][1] * position_e[1] + projection[1][2] * position_e[2];
				float w = -position_e[2];

				if (out.clip[3] > 0.5f)
				{
					add(&results->position, (x / w + 1.0f) * 320.0f - (out.clip[0] * c[-38][0] / out.clip[3] + c[-37][0] + 0.5f));
					add(&results->position, (1.0f - y / w) * 240.0f - (out.clip[1] * c[-38][1] / out.clip[3] + c[-37][1] + 0.5f));
				}
			}
			gx_lighting_evaluate(&lighting, position_e, normal_e, d0, d1);
			for (component = 0; component < 3; component++)
				add(has_points ? &results->d0_points : &results->d0, (double)d0[component] - color_byte(out.d0[component]));
			if (lighting.fresnel)
				for (component = 0; component < 4; component++)
					add(&results->d1, (double)d1[component] - color_byte(out.d1[component]));
			if (lighting.alpha != _gx_lighting_alpha_zero)
				add(&results->alpha, (double)color_byte(gx_lighting_planar_fog(&lighting, world)) - color_byte(out.d0[3]));
			if (ramped)
			{
				/* (GX's: the position in E through the post-transform matrix, the
				ramp filtered there) */
				float coordinates[2];

				for (row = 0; row < 2; row++)
					coordinates[row] = dot3(ramp_matrix[row], position_e) + ramp_matrix[row][3];
				add(&results->ramp, (double)gx_lighting_fog_sample(ramp, form.rows, coordinates[0], coordinates[1]) -
					color_byte(out.d0[3]));
				if (vertex)
				{
					/* (halfway to the vertex before: the programs' interpolated, GX's
					the ramp's at the coordinates there, which are affine; and the
					fog's own there, the world position halfway) */
					float half[2], world_half[3];
					unsigned char sampled;
					int k;

					for (k = 0; k < 2; k++)
						half[k] = 0.5f * (coordinates[k] + last_coordinates[k]);
					for (k = 0; k < 3; k++)
						world_half[k] = 0.5f * (world[k] + last_world[k]);
					sampled = gx_lighting_fog_sample(ramp, form.rows, half[0], half[1]);
					add(&results->ramp_between, (double)sampled - 255.0 * 0.5 * (last_alpha + clamp_unit(out.d0[3])));
					add(&results->ramp_exact, (double)sampled - 255.0 * gx_lighting_planar_fog(&lighting, world_half));
				}
				last_coordinates[0] = coordinates[0];
				last_coordinates[1] = coordinates[1];
				memcpy(last_world, world, sizeof(last_world));
				last_alpha = clamp_unit(out.d0[3]);
			}
			if (gx_lighting_recipe_reflects(recipe))
			{
				/* the reflection's direction: the program's, and what each frame's
				sphere map holds at the texture coordinates GX makes of the normal
				(where the normal faces the eye: the others are culled) */
				float exact[3], to_eye[3], normal_world[3];
				int frame_index;

				for (row = 0; row < 3; row++)
				{
					exact[row] = out.t[3][row];
					normal_world[row] = dot3(matrix[row], inputs[1]);
					to_eye[row] = c[-92][row] - world[row];
				}
				normalize3(exact);
				if (dot3(normal_world, to_eye) > 0.0f)
					for (frame_index = 0; frame_index < 3; frame_index++)
					{
						float n[3], reflection[3], approximate[3], texel[2], held[2], z, cosine;

						for (row = 0; row < 3; row++)
							n[row] = dot3(frames[frame_index][row], normal_world);
						normalize3(n);
						gx_lighting_sphere_texel((const float (*)[3])frames[frame_index], lighting.reflection_scale,
							0.5f * n[0] + 0.5f, 0.5f * n[1] + 0.5f, texel);
						z = sqrtf(fmaxf(0.0f, 1.0f - n[0] * n[0] - n[1] * n[1]));
						reflection[0] = 2.0f * z * n[0];
						reflection[1] = 2.0f * z * n[1];
						reflection[2] = 2.0f * z * z - 1.0f;
						for (column = 0; column < 3; column++)
							approximate[column] = reflection[0] * frames[frame_index][0][column] +
								reflection[1] * frames[frame_index][1][column] + reflection[2] * frames[frame_index][2][column];
						normalize3(approximate);
						/* (the texel's coordinates are those of the direction it holds) */
						gx_lighting_sphere_coordinates(approximate, held);
						add(&results->texel, (held[0] - texel[0]) * 128.0f);
						add(&results->texel, (held[1] - texel[1]) * 128.0f);
						cosine = dot3(exact, approximate);
						cosine = cosine > 1.0f ? 1.0f : cosine < -1.0f ? -1.0f : cosine;
						add(&results->reflection[frame_index], acosf(cosine) * 180.0f / PI);
						/* (the device's sphere map: a grid of GRID squares drawn over its
						texels, the world's coordinates between its points interpolated) */
						if (frame_index == 1)
						{
							float u = (0.5f * n[0] + 0.5f) * GRID, v = (0.5f * n[1] + 0.5f) * GRID, fu, fv, corners[4][2];
							int iu = (int)u < GRID ? (int)u : GRID - 1, iv = (int)v < GRID ? (int)v : GRID - 1, corner;

							fu = u - iu;
							fv = v - iv;
							for (corner = 0; corner < 4; corner++)
								gx_lighting_sphere_texel((const float (*)[3])frames[1], lighting.reflection_scale,
									(float)(iu + (corner & 1)) / GRID, (float)(iv + (corner >> 1)) / GRID, corners[corner]);
							for (column = 0; column < 2; column++)
							{
								float drawn = (1 - fv) * ((1 - fu) * corners[0][column] + fu * corners[1][column]) +
									fv * ((1 - fu) * corners[2][column] + fu * corners[3][column]);

								add(&results->grid, (drawn - texel[column]) * 128.0f);
							}
						}
					}
			}
		}
		(void)points;
	}
}

int main(int argc, char **argv)
{
	static DWORD words[65536];
	unsigned long word_count = 0, index;
	char line[512];
	FILE *in;
	int in_comment = 0, failed = 0;

	if (argc != 2)
	{
		fprintf(stderr, "usage: gx_lighting_check microcode.inc\n");
		return 2;
	}
	in = fopen(argv[1], "r");
	if (!in)
	{
		perror(argv[1]);
		return 2;
	}
	while (fgets(line, sizeof(line), in))
	{
		char *cursor = line;

		while (*cursor)
		{
			if (in_comment)
			{
				char *end = strstr(cursor, "*/");

				if (!end)
					break;
				in_comment = 0;
				cursor = end + 2;
			}
			else if (cursor[0] == '/' && cursor[1] == '*')
			{
				in_comment = 1;
				cursor += 2;
			}
			else if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X') && word_count < 65536)
				words[word_count++] = (DWORD)strtoul(cursor, &cursor, 16);
			else
				cursor++;
		}
	}
	fclose(in);
	srand(12345);
	printf("gx_lighting_check: GX's lights against the programs, %d scenes of %d vertices each\n", SCENES, VERTICES);
	for (index = 0; index < sizeof(programs) / sizeof(programs[0]); index++)
	{
		static struct nv2a_cpu_program program;
		struct results results;

		memset(&results, 0, sizeof(results));
		if (!program_find(words, word_count, programs[index], &program) || !gx_lighting_recipe_find(programs[index]))
		{
			printf("program %08lx: not found\n", programs[index]);
			failed = 1;
			continue;
		}
		check_program(&program, &results);
		printf("program %08lx (%lu scenes, %lu refused):\n", programs[index], results.scenes, results.refused);
		report("D0 colour, distant lights", &results.d0, "/255");
		report("D0 colour, with point lights (fitted)", &results.d0_points, "/255");
		if (results.d1.count)
			report("D1, the Fresnel colour", &results.d1, "/255");
		if (results.alpha.count)
			report("D0 alpha, the fog's (processor)", &results.alpha, "/255");
		if (results.ramp.count)
		{
			report("D0 alpha, GX's ramp, at the vertices", &results.ramp, "/255");
			report("D0 alpha, GX's ramp, halfway (programs: mean)", &results.ramp_between, "/255");
			report("D0 alpha, GX's ramp, halfway (the fog's there)", &results.ramp_exact, "/255");
			printf("  %-46s %lu of %lu scenes\n", "the ramp's range refused", results.ramp_refused, results.scenes);
			printf("  %-46s %lu\n", "the ramp's rows (mean)", results.ramp_rows / (results.scenes - results.ramp_refused));
		}
		if (results.reflection[0].count)
		{
			report("reflection, the eye straight ahead", &results.reflection[0], "degrees");
			report("reflection, the eye along the draw (rounded)", &results.reflection[1], "degrees");
			report("reflection, the eye along the draw", &results.reflection[2], "degrees");
			report("reflection, a texel's place (of 128)", &results.texel, "texels");
			report("reflection, the grid's place (of 128)", &results.grid, "texels");
		}
		report("position", &results.position, "pixels");
		/* the colours GX makes as the programs do: within a level or two on
		average (unit normals, 8-bit light colours) */
		if (mean(&results.d0) > 2.0 || mean(&results.d1) > 2.0 || results.alpha.largest > 1.0 ||
			mean(&results.ramp) > 1.0 || results.ramp.largest > 4.0 || results.ramp_refused > results.scenes / 20 ||
			results.position.largest > 0.01 || results.refused > results.scenes / 20)
		{
			printf("  ... beyond what it should be\n");
			failed = 1;
		}
	}
	printf("gx_lighting_check: %s\n", failed ? "FAILED" : "passed");
	return failed;
}
