/*
NV2A_NATIVE_CHECK.C

A host check of the vertex programs compiled ahead of time
(WiiLibs' tools/xbox_vsh_codegen.c; tools/wii_nv2a_check.py builds and runs it):
each of the game's programs is run as its function and on the interpreter
(WiiLibs' wl/xbox/vsh.c) over random constants and inputs, and the two
must give the same outputs, bit for bit; so must its function for the
outputs a draw reads (wl_xbox_vsh_run_live), in those outputs, for random sets
of them; and for the inputs the analysis says some outputs read
(wl_xbox_vsh_live_inputs: d3d8_gx.c fetches no others), those outputs must be
the same whatever the others hold. A model's vertex on one node (weight 1 on its first index, or 0
on its second) must give every output that the node with weight 1 alone
does, whatever the other index (port/wii/src/d3d8_gx.c keys such vertices
by their node). An output component the analysis finds reads no constant
(wl_xbox_vsh_analyze_constants) must be the same, bit for bit, for other
random constants, on the interpreter and as its function computes the
outputs that hold such components alone (port/wii/src/d3d8_gx.c keeps
those of a static vertex buffer, computed once, for its later draws,
whatever their constants). Then the functions' own arithmetic: wl_xbox_vsh_sqrtf (WiiLibs'
wl_sqrtf_exact, which WiiLibs' own tests cover) with its refinement,
wl_sqrtf_refine, from estimates as coarse as frsqrte's, against sqrtf, and
wl_xbox_vsh_floor_index against floorf.

usage: nv2a_native_check microcode.inc
*/

#include "wl/xbox/vsh_native.h"

/* (the generated programs: nv2a_programs.c) */
extern const WlXboxVshNative xbox_vsh_natives[];
extern const unsigned long xbox_vsh_natives_count;

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* a model's vertex (model_vertex_compressed's declaration): its two node
indices, bytes, and the first node's weight, a normalized short */
#define NODE_INDICES 5
#define NODE_WEIGHT 6

static float random_float(void)
{
	return (float)(rand() % 20001 - 10000) / 1000.0f;
}

/* the outputs in live the same in both */
static int live_same(const WlXboxVshOutput *a, const WlXboxVshOutput *b, unsigned live)
{
	int stage;

	if ((live & WL_XBOX_VSH_LIVE_POSITION) && (memcmp(a->position, b->position, sizeof(a->position)) ||
		memcmp(a->clip, b->clip, sizeof(a->clip)) || a->clip_captured != b->clip_captured))
		return 0;
	if ((live & WL_XBOX_VSH_LIVE_D0) && memcmp(a->d0, b->d0, sizeof(a->d0)))
		return 0;
	if ((live & WL_XBOX_VSH_LIVE_D1) && memcmp(a->d1, b->d1, sizeof(a->d1)))
		return 0;
	if ((live & WL_XBOX_VSH_LIVE_BACK) && (memcmp(a->b0, b->b0, sizeof(a->b0)) || memcmp(a->b1, b->b1, sizeof(a->b1))))
		return 0;
	if ((live & WL_XBOX_VSH_LIVE_POINT_SIZE) && memcmp(&a->point_size, &b->point_size, sizeof(a->point_size)))
		return 0;
	for (stage = 0; stage < 4; stage++)
		if ((live & (WL_XBOX_VSH_LIVE_T0 << stage)) && memcmp(a->t[stage], b->t[stage], sizeof(a->t[stage])))
			return 0;
	if ((live & WL_XBOX_VSH_LIVE_FOG) && memcmp(&a->fog, &b->fog, sizeof(a->fog)))
		return 0;
	return 1;
}

/* a component of an output in live that reads no constant (reads:
wl_xbox_vsh_analyze_constants) the same in both; checked counts them */
static int constant_free_same(const WlXboxVshDependencies *reads, const WlXboxVshOutput *a,
	const WlXboxVshOutput *b, unsigned live, unsigned long *checked)
{
	int stage, component;

	for (component = 0; component < 4; component++)
	{
		if ((live & WL_XBOX_VSH_LIVE_D0) && !(reads->d0[component] & WL_XBOX_VSH_READS_CONSTANT))
		{
			++*checked;
			if (memcmp(&a->d0[component], &b->d0[component], sizeof(float)))
				return 0;
		}
		if ((live & WL_XBOX_VSH_LIVE_D1) && !(reads->d1[component] & WL_XBOX_VSH_READS_CONSTANT))
		{
			++*checked;
			if (memcmp(&a->d1[component], &b->d1[component], sizeof(float)))
				return 0;
		}
		for (stage = 0; stage < 4; stage++)
			if ((live & (WL_XBOX_VSH_LIVE_T0 << stage)) && !(reads->t[stage][component] & WL_XBOX_VSH_READS_CONSTANT))
			{
				++*checked;
				if (memcmp(&a->t[stage][component], &b->t[stage][component], sizeof(float)))
					return 0;
			}
	}
	if ((live & WL_XBOX_VSH_LIVE_FOG) && !(reads->fog & WL_XBOX_VSH_READS_CONSTANT))
	{
		++*checked;
		if (memcmp(&a->fog, &b->fog, sizeof(float)))
			return 0;
	}
	return 1;
}

/* the outputs (WL_XBOX_VSH_LIVE_...) with a component that depends on the
vertex and reads no constant */
static unsigned constant_free_outputs(const WlXboxVshDependencies *reads)
{
	unsigned live = 0;
	int stage, component;

	for (component = 0; component < 4; component++)
	{
		if ((reads->d0[component] & 0xFFFF) && !(reads->d0[component] & WL_XBOX_VSH_READS_CONSTANT))
			live |= WL_XBOX_VSH_LIVE_D0;
		if ((reads->d1[component] & 0xFFFF) && !(reads->d1[component] & WL_XBOX_VSH_READS_CONSTANT))
			live |= WL_XBOX_VSH_LIVE_D1;
		for (stage = 0; stage < 4; stage++)
			if ((reads->t[stage][component] & 0xFFFF) && !(reads->t[stage][component] & WL_XBOX_VSH_READS_CONSTANT))
				live |= WL_XBOX_VSH_LIVE_T0 << stage;
	}
	if ((reads->fog & 0xFFFF) && !(reads->fog & WL_XBOX_VSH_READS_CONSTANT))
		live |= WL_XBOX_VSH_LIVE_FOG;
	return live;
}

/* an estimate of 1 / sqrt(x) to 5 bits (frsqrte's worst), and off by 1/32
either way as which says */
static double coarse_estimate(double x, int which)
{
	double exact = 1.0 / sqrt(x), scale;
	int exponent;

	scale = frexp(exact, &exponent);
	scale = floor(scale * 32.0) / 32.0;
	return ldexp(scale, exponent) * (which == 0 ? 1.0 : which == 1 ? 1.0 + 1.0 / 32.0 : 1.0 - 1.0 / 32.0);
}

/* wl_xbox_vsh_sqrtf's arithmetic for every 199th positive float (each of the
smallest), and wl_xbox_vsh_floor_index around the whole numbers and sixty-fourths */
static unsigned long arithmetic_check(void)
{
	static const float specials[] = { 0.0f, -0.0f, 1.0f / 0.0f, -1.0f, 3.40282347e+38f, 1.0e-45f };
	unsigned long differ = 0, checked = 0;
	uint32_t bits;
	int which;

	for (bits = 1; bits < 0x7F800000u; bits += bits < 0x00100000u ? 1 : 199)
	{
		float x, exact;

		memcpy(&x, &bits, sizeof(x));
		exact = sqrtf(x);
		for (which = 0; which < 3; which++)
		{
			float refined = wl_sqrtf_refine(x, coarse_estimate(x, which));

			checked++;
			if (memcmp(&refined, &exact, sizeof(exact)))
			{
				if (differ < 8)
					printf("wl_xbox_vsh_sqrtf(%a): %a, sqrtf %a\n", x, refined, exact);
				differ++;
			}
		}
	}
	for (which = 0; which < (int)(sizeof(specials) / sizeof(specials[0])); which++)
	{
		float root = wl_xbox_vsh_sqrtf(specials[which]), exact = sqrtf(specials[which]);

		checked++;
		if (memcmp(&root, &exact, sizeof(exact)) && !(root != root && exact != exact))
		{
			printf("wl_xbox_vsh_sqrtf(%a): %a, sqrtf %a\n", specials[which], root, exact);
			differ++;
		}
	}
	for (which = -70000; which <= 70000; which++)
	{
		float whole = (float)which / 64.0f;
		int step;

		for (step = -1; step <= 1; step++)
		{
			float x = step ? nextafterf(whole, step < 0 ? -1.0e30f : 1.0e30f) : whole;

			checked++;
			if (wl_xbox_vsh_floor_index(x) != (int)floorf(x))
			{
				if (differ < 8)
					printf("wl_xbox_vsh_floor_index(%a): %d, floorf %d\n", x, wl_xbox_vsh_floor_index(x), (int)floorf(x));
				differ++;
			}
		}
	}
	printf("%lu square roots and whole parts compared, %lu differ\n", checked, differ);
	return differ;
}

int main(int argc, char **argv)
{
	static uint32_t words[65536];
	static float constants[WL_XBOX_VSH_CONSTANT_COUNT][4], other_constants[WL_XBOX_VSH_CONSTANT_COUNT][4],
		inputs[WL_XBOX_VSH_INPUT_COUNT][4];
	static WlXboxVshProgram program;
	unsigned long count = 0, position, checked = 0, differ = 0, native = 0, one_node_checked = 0, inputs_checked = 0;
	unsigned long constant_free_programs = 0, constant_free_checked = 0, positions_checked = 0;
	char line[512];
	int in_comment = 0;
	FILE *in;

	if (argc != 2 || !(in = fopen(argv[1], "r")))
	{
		fprintf(stderr, "usage: nv2a_native_check microcode.inc\n");
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
			else if (cursor[0] == '0' && cursor[1] == 'x' && count < 65536)
			{
				words[count++] = (uint32_t)strtoul(cursor, &cursor, 16);
			}
			else
			{
				cursor++;
			}
		}
	}
	fclose(in);
	for (position = 0; position < count; )
	{
		unsigned long instruction_count = words[position] >> 16;
		const uint32_t *instructions = words + position + 1;
		WlXboxVshDependencies reads;
		unsigned free_outputs;
		int trial;

		position += 1 + instruction_count * 4;
		if (!instruction_count || position > count)
			continue;
		wl_xbox_vsh_compile(instructions, instruction_count, xbox_vsh_natives, xbox_vsh_natives_count, &program);
		if (!program.native)
		{
			printf("a program of %lu instructions has no function\n", instruction_count);
			differ++;
			continue;
		}
		native++;
		wl_xbox_vsh_analyze_constants(&program, &reads);
		free_outputs = constant_free_outputs(&reads);
		if (free_outputs)
		{
			constant_free_programs++;
			printf("program %08lx: outputs the vertex's alone %03x\n", wl_xbox_vsh_hash(instructions, instruction_count),
				free_outputs);
		}
		for (trial = 0; trial < 64; trial++)
		{
			WlXboxVshOutput compiled, interpreted;
			WlXboxVshLiveProc run = program.native;
			int row, component;

			for (row = 0; row < WL_XBOX_VSH_CONSTANT_COUNT; row++)
				for (component = 0; component < 4; component++)
					constants[row][component] = random_float();
			for (row = 0; row < WL_XBOX_VSH_INPUT_COUNT; row++)
				for (component = 0; component < 4; component++)
					inputs[row][component] = random_float();
			/* (a0's constants: a whole number, as the game's indices are) */
			for (row = 0; row < WL_XBOX_VSH_CONSTANT_COUNT; row += 7)
				constants[row][0] = (float)(rand() % 64);
			wl_xbox_vsh_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &compiled);
			program.native = NULL;
			wl_xbox_vsh_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &interpreted);
			program.native = run;
			checked++;
			if (memcmp(&compiled, &interpreted, sizeof(compiled)))
			{
				printf("program %08lx (%lu instructions): its function and the interpreter differ\n",
					wl_xbox_vsh_hash(instructions, instruction_count), instruction_count);
				differ++;
				break;
			}
			/* its function for some of the outputs: those the same */
			{
				static const unsigned sets[] = { WL_XBOX_VSH_LIVE_ALL, WL_XBOX_VSH_LIVE_POSITION,
					WL_XBOX_VSH_LIVE_POSITION | WL_XBOX_VSH_LIVE_D0, WL_XBOX_VSH_LIVE_POSITION | WL_XBOX_VSH_LIVE_T0,
					WL_XBOX_VSH_LIVE_POSITION | WL_XBOX_VSH_LIVE_D0 | WL_XBOX_VSH_LIVE_T0 | WL_XBOX_VSH_LIVE_FOG };
				WlXboxVshOutput live_output;
				unsigned live = trial < 5 ? sets[trial] : (unsigned)rand() & WL_XBOX_VSH_LIVE_ALL;

				memset(&live_output, 0xA5 + trial, sizeof(live_output));
				wl_xbox_vsh_run_live(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f,
					&live_output, live);
				if (!live_same(&live_output, &interpreted, live))
				{
					printf("program %08lx: its function for the outputs %03x differs\n",
						wl_xbox_vsh_hash(instructions, instruction_count), live);
					differ++;
					break;
				}
			}
			/* its positions function (the skinned programs'): each of four
			points' clip-space and screen-space positions the same, to the bit,
			as a run for the position with v0 that point (d3d8_gx.c's position
			matrices, and their axes: 0 and 1 among the points) */
			if (program.native_positions)
			{
				float points[4][4], clips[4][4], screens[4][4], saved[4];
				int point;

				for (point = 0; point < 4; point++)
					for (component = 0; component < 4; component++)
						points[point][component] = trial & 1 ? random_float() :
							component == 3 ? 1.0f : component + 1 == point ? 1.0f : 0.0f;
				memset(clips, 0x3C, sizeof(clips));
				wl_xbox_vsh_run_positions(&program, (const float (*)[4])constants, (const float (*)[4])inputs,
					(const float (*)[4])points, clips, screens);
				memcpy(saved, inputs[0], sizeof(saved));
				for (point = 0; point < 4; point++)
				{
					WlXboxVshOutput one;

					memcpy(inputs[0], points[point], sizeof(inputs[0]));
					wl_xbox_vsh_run_live(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &one,
						WL_XBOX_VSH_LIVE_POSITION);
					positions_checked++;
					if (memcmp(one.clip, clips[point], sizeof(one.clip)) ||
						memcmp(one.position, screens[point], sizeof(one.position)))
					{
						printf("program %08lx: its positions function differs at point %d\n",
							wl_xbox_vsh_hash(instructions, instruction_count), point);
						differ++;
						break;
					}
				}
				memcpy(inputs[0], saved, sizeof(saved));
				if (point < 4)
					break;
			}
			/* the components that read no constant: the same for other
			constants, on the interpreter and as its function computes the
			outputs that hold them alone (as d3d8_gx.c keeps them) */
			{
				WlXboxVshOutput other, alone;

				for (row = 0; row < WL_XBOX_VSH_CONSTANT_COUNT; row++)
					for (component = 0; component < 4; component++)
						other_constants[row][component] = random_float();
				for (row = 0; row < WL_XBOX_VSH_CONSTANT_COUNT; row += 5)
					other_constants[row][0] = (float)(rand() % 64);
				program.native = NULL;
				wl_xbox_vsh_run(&program, (const float (*)[4])other_constants, (const float (*)[4])inputs, 1.0f, &other);
				program.native = run;
				memset(&alone, 0x5A + trial, sizeof(alone));
				wl_xbox_vsh_run_live(&program, (const float (*)[4])other_constants, (const float (*)[4])inputs, 1.0f,
					&alone, free_outputs);
				if (!constant_free_same(&reads, &interpreted, &other, WL_XBOX_VSH_LIVE_ALL, &constant_free_checked) ||
					!constant_free_same(&reads, &interpreted, &alone, free_outputs, &constant_free_checked))
				{
					printf("program %08lx: an output that reads no constant differs with other constants\n",
						wl_xbox_vsh_hash(instructions, instruction_count));
					differ++;
					break;
				}
			}
			/* the inputs the analysis says those outputs read: the outputs the
			same whatever the others hold (d3d8_gx.c fetches no more) */
			{
				static float others[WL_XBOX_VSH_INPUT_COUNT][4];
				static WlXboxVshDependencies dependencies;
				WlXboxVshOutput first, second;
				unsigned live = (unsigned)rand() & WL_XBOX_VSH_LIVE_ALL;
				unsigned long reads;

				if (!trial)
					wl_xbox_vsh_analyze(&program, &dependencies);
				reads = wl_xbox_vsh_live_inputs(&dependencies, live);
				for (row = 0; row < WL_XBOX_VSH_INPUT_COUNT; row++)
					for (component = 0; component < 4; component++)
						others[row][component] = (reads & (1UL << row)) ? inputs[row][component] : random_float();
				wl_xbox_vsh_run_live(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &first, live);
				wl_xbox_vsh_run_live(&program, (const float (*)[4])constants, (const float (*)[4])others, 1.0f, &second, live);
				inputs_checked++;
				if (!live_same(&first, &second, live))
				{
					printf("program %08lx: the outputs %03x read more than inputs %04lx\n",
						wl_xbox_vsh_hash(instructions, instruction_count), live, reads);
					differ++;
					break;
				}
				reads = wl_xbox_vsh_live_inputs(&dependencies, WL_XBOX_VSH_LIVE_D0 | WL_XBOX_VSH_LIVE_D1 | WL_XBOX_VSH_LIVE_FOG);
				for (row = 0; row < WL_XBOX_VSH_INPUT_COUNT; row++)
					for (component = 0; component < 4; component++)
						others[row][component] = (reads & (1UL << row)) ? inputs[row][component] : random_float();
				memset(&first, 0, sizeof(first));
				memset(&second, 0, sizeof(second));
				wl_xbox_vsh_run_colors(&program, (const float (*)[4])constants, (const float (*)[4])inputs, &first);
				wl_xbox_vsh_run_colors(&program, (const float (*)[4])constants, (const float (*)[4])others, &second);
				if (!live_same(&first, &second, WL_XBOX_VSH_LIVE_D0 | WL_XBOX_VSH_LIVE_D1 | WL_XBOX_VSH_LIVE_FOG))
				{
					printf("program %08lx: its colours read more than inputs %04lx\n",
						wl_xbox_vsh_hash(instructions, instruction_count), reads);
					differ++;
					break;
				}
			}
			/* its colour-only function: the same colours and fog */
			{
				WlXboxVshOutput colors;

				memset(&colors, 0, sizeof(colors));
				wl_xbox_vsh_run_colors(&program, (const float (*)[4])constants, (const float (*)[4])inputs, &colors);
				if (memcmp(colors.d0, compiled.d0, sizeof(colors.d0)) || memcmp(colors.d1, compiled.d1, sizeof(colors.d1)) ||
					memcmp(&colors.fog, &compiled.fog, sizeof(colors.fog)))
				{
					printf("program %08lx: its colour-only function's colours differ\n",
						wl_xbox_vsh_hash(instructions, instruction_count));
					differ++;
					break;
				}
			}
			/* a model's vertex on one node (d3d8_gx.c's node_keys):
			node indices (a, b) with a weight of 1, or (b, a) with 0, give
			every output that (a, a) with 1 does */
			{
				WlXboxVshOutput one_node, other;
				int a = rand() % 256, b = rand() % 256, order;

				inputs[NODE_INDICES][0] = a / 255.0f;
				inputs[NODE_INDICES][1] = a / 255.0f;
				inputs[NODE_INDICES][2] = 0.0f;
				inputs[NODE_INDICES][3] = 1.0f;
				inputs[NODE_WEIGHT][0] = 1.0f;
				inputs[NODE_WEIGHT][1] = inputs[NODE_WEIGHT][2] = 0.0f;
				inputs[NODE_WEIGHT][3] = 1.0f;
				wl_xbox_vsh_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &one_node);
				for (order = 0; order < 2; order++)
				{
					inputs[NODE_INDICES][order] = b / 255.0f;
					inputs[NODE_WEIGHT][0] = order ? 1.0f : 0.0f;
					wl_xbox_vsh_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &other);
					inputs[NODE_INDICES][order] = a / 255.0f;
					one_node_checked++;
					if (memcmp(&one_node, &other, sizeof(other)))
					{
						printf("program %08lx: a vertex on node %d (indices %d and %d, weight %d) differs\n",
							wl_xbox_vsh_hash(instructions, instruction_count), a, order ? a : b, order ? b : a, order);
						differ++;
						break;
					}
				}
				if (order < 2)
					break;
			}
		}
	}
	printf("%lu programs with functions, %lu runs compared, %lu vertices on one node, %lu runs of the inputs read alone, "
		"%lu differ\n", native, checked, one_node_checked, inputs_checked, differ);
	printf("%lu positions of four at once compared with runs\n", positions_checked);
	printf("%lu programs with outputs the vertex's alone; %lu components that read no constant compared with other "
		"constants\n", constant_free_programs, constant_free_checked);
	differ += arithmetic_check();
	return differ != 0;
}
