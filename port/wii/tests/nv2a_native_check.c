/*
NV2A_NATIVE_CHECK.C

A host check of the vertex programs compiled ahead of time
(port/wii/tools/nv2a_codegen.c; tools/wii_nv2a_check.py builds and runs it):
each of the game's programs is run as its function and on the interpreter
(port/wii/src/nv2a_cpu.c) over random constants and inputs, and the two
must give the same outputs, bit for bit; so must its function for the
outputs a draw reads (nv2a_cpu_run_live), in those outputs, for random sets
of them. A model's vertex on one node (weight 1 on its first index, or 0
on its second) must give every output that the node with weight 1 alone
does, whatever the other index (port/wii/src/d3d8_gx.c keys such vertices
by their node). An output component the analysis finds reads no constant
(nv2a_cpu_analyze_constants) must be the same, bit for bit, for other
random constants, on the interpreter and as its function computes the
outputs that hold such components alone (port/wii/src/d3d8_gx.c keeps
those of a static vertex buffer, computed once, for its later draws,
whatever their constants). Then the functions' own arithmetic: nv2a_sqrtf (WiiLibs'
wl_sqrtf_exact, which WiiLibs' own tests cover) with its refinement,
wl_sqrtf_refine, from estimates as coarse as frsqrte's, against sqrtf, and
nv2a_floor_index against floorf.

usage: nv2a_native_check microcode.inc
*/

#include "nv2a_cpu_native.h"

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
static int live_same(const struct nv2a_cpu_output *a, const struct nv2a_cpu_output *b, unsigned live)
{
	int stage;

	if ((live & NV2A_CPU_LIVE_POSITION) && (memcmp(a->position, b->position, sizeof(a->position)) ||
		memcmp(a->clip, b->clip, sizeof(a->clip)) || a->clip_captured != b->clip_captured))
		return 0;
	if ((live & NV2A_CPU_LIVE_D0) && memcmp(a->d0, b->d0, sizeof(a->d0)))
		return 0;
	if ((live & NV2A_CPU_LIVE_D1) && memcmp(a->d1, b->d1, sizeof(a->d1)))
		return 0;
	if ((live & NV2A_CPU_LIVE_BACK) && (memcmp(a->b0, b->b0, sizeof(a->b0)) || memcmp(a->b1, b->b1, sizeof(a->b1))))
		return 0;
	if ((live & NV2A_CPU_LIVE_POINT_SIZE) && memcmp(&a->point_size, &b->point_size, sizeof(a->point_size)))
		return 0;
	for (stage = 0; stage < 4; stage++)
		if ((live & (NV2A_CPU_LIVE_T0 << stage)) && memcmp(a->t[stage], b->t[stage], sizeof(a->t[stage])))
			return 0;
	if ((live & NV2A_CPU_LIVE_FOG) && memcmp(&a->fog, &b->fog, sizeof(a->fog)))
		return 0;
	return 1;
}

/* a component of an output in live that reads no constant (reads:
nv2a_cpu_analyze_constants) the same in both; checked counts them */
static int constant_free_same(const struct nv2a_cpu_dependencies *reads, const struct nv2a_cpu_output *a,
	const struct nv2a_cpu_output *b, unsigned live, unsigned long *checked)
{
	int stage, component;

	for (component = 0; component < 4; component++)
	{
		if ((live & NV2A_CPU_LIVE_D0) && !(reads->d0[component] & NV2A_CPU_READS_CONSTANT))
		{
			++*checked;
			if (memcmp(&a->d0[component], &b->d0[component], sizeof(float)))
				return 0;
		}
		if ((live & NV2A_CPU_LIVE_D1) && !(reads->d1[component] & NV2A_CPU_READS_CONSTANT))
		{
			++*checked;
			if (memcmp(&a->d1[component], &b->d1[component], sizeof(float)))
				return 0;
		}
		for (stage = 0; stage < 4; stage++)
			if ((live & (NV2A_CPU_LIVE_T0 << stage)) && !(reads->t[stage][component] & NV2A_CPU_READS_CONSTANT))
			{
				++*checked;
				if (memcmp(&a->t[stage][component], &b->t[stage][component], sizeof(float)))
					return 0;
			}
	}
	if ((live & NV2A_CPU_LIVE_FOG) && !(reads->fog & NV2A_CPU_READS_CONSTANT))
	{
		++*checked;
		if (memcmp(&a->fog, &b->fog, sizeof(float)))
			return 0;
	}
	return 1;
}

/* the outputs (NV2A_CPU_LIVE_...) with a component that depends on the
vertex and reads no constant */
static unsigned constant_free_outputs(const struct nv2a_cpu_dependencies *reads)
{
	unsigned live = 0;
	int stage, component;

	for (component = 0; component < 4; component++)
	{
		if ((reads->d0[component] & 0xFFFF) && !(reads->d0[component] & NV2A_CPU_READS_CONSTANT))
			live |= NV2A_CPU_LIVE_D0;
		if ((reads->d1[component] & 0xFFFF) && !(reads->d1[component] & NV2A_CPU_READS_CONSTANT))
			live |= NV2A_CPU_LIVE_D1;
		for (stage = 0; stage < 4; stage++)
			if ((reads->t[stage][component] & 0xFFFF) && !(reads->t[stage][component] & NV2A_CPU_READS_CONSTANT))
				live |= NV2A_CPU_LIVE_T0 << stage;
	}
	if ((reads->fog & 0xFFFF) && !(reads->fog & NV2A_CPU_READS_CONSTANT))
		live |= NV2A_CPU_LIVE_FOG;
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

/* nv2a_sqrtf's arithmetic for every 199th positive float (each of the
smallest), and nv2a_floor_index around the whole numbers and sixty-fourths */
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
					printf("nv2a_sqrtf(%a): %a, sqrtf %a\n", x, refined, exact);
				differ++;
			}
		}
	}
	for (which = 0; which < (int)(sizeof(specials) / sizeof(specials[0])); which++)
	{
		float root = nv2a_sqrtf(specials[which]), exact = sqrtf(specials[which]);

		checked++;
		if (memcmp(&root, &exact, sizeof(exact)) && !(root != root && exact != exact))
		{
			printf("nv2a_sqrtf(%a): %a, sqrtf %a\n", specials[which], root, exact);
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
			if (nv2a_floor_index(x) != (int)floorf(x))
			{
				if (differ < 8)
					printf("nv2a_floor_index(%a): %d, floorf %d\n", x, nv2a_floor_index(x), (int)floorf(x));
				differ++;
			}
		}
	}
	printf("%lu square roots and whole parts compared, %lu differ\n", checked, differ);
	return differ;
}

int main(int argc, char **argv)
{
	static DWORD words[65536];
	static float constants[NV2A_CPU_CONSTANT_COUNT][4], other_constants[NV2A_CPU_CONSTANT_COUNT][4],
		inputs[NV2A_CPU_INPUT_COUNT][4];
	static struct nv2a_cpu_program program;
	unsigned long count = 0, position, checked = 0, differ = 0, native = 0, one_node_checked = 0;
	unsigned long constant_free_programs = 0, constant_free_checked = 0;
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
				words[count++] = (DWORD)strtoul(cursor, &cursor, 16);
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
		const DWORD *instructions = words + position + 1;
		struct nv2a_cpu_dependencies reads;
		unsigned free_outputs;
		int trial;

		position += 1 + instruction_count * 4;
		if (!instruction_count || position > count)
			continue;
		nv2a_cpu_compile(instructions, instruction_count, &program);
		if (!program.native)
		{
			printf("a program of %lu instructions has no function\n", instruction_count);
			differ++;
			continue;
		}
		native++;
		nv2a_cpu_analyze_constants(&program, &reads);
		free_outputs = constant_free_outputs(&reads);
		if (free_outputs)
		{
			constant_free_programs++;
			printf("program %08lx: outputs the vertex's alone %03x\n", nv2a_cpu_hash(instructions, instruction_count),
				free_outputs);
		}
		for (trial = 0; trial < 64; trial++)
		{
			struct nv2a_cpu_output compiled, interpreted;
			nv2a_cpu_live_proc run = program.native;
			int row, component;

			for (row = 0; row < NV2A_CPU_CONSTANT_COUNT; row++)
				for (component = 0; component < 4; component++)
					constants[row][component] = random_float();
			for (row = 0; row < NV2A_CPU_INPUT_COUNT; row++)
				for (component = 0; component < 4; component++)
					inputs[row][component] = random_float();
			/* (a0's constants: a whole number, as the game's indices are) */
			for (row = 0; row < NV2A_CPU_CONSTANT_COUNT; row += 7)
				constants[row][0] = (float)(rand() % 64);
			nv2a_cpu_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &compiled);
			program.native = NULL;
			nv2a_cpu_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &interpreted);
			program.native = run;
			checked++;
			if (memcmp(&compiled, &interpreted, sizeof(compiled)))
			{
				printf("program %08lx (%lu instructions): its function and the interpreter differ\n",
					nv2a_cpu_hash(instructions, instruction_count), instruction_count);
				differ++;
				break;
			}
			/* its function for some of the outputs: those the same */
			{
				static const unsigned sets[] = { NV2A_CPU_LIVE_ALL, NV2A_CPU_LIVE_POSITION,
					NV2A_CPU_LIVE_POSITION | NV2A_CPU_LIVE_D0, NV2A_CPU_LIVE_POSITION | NV2A_CPU_LIVE_T0,
					NV2A_CPU_LIVE_POSITION | NV2A_CPU_LIVE_D0 | NV2A_CPU_LIVE_T0 | NV2A_CPU_LIVE_FOG };
				struct nv2a_cpu_output live_output;
				unsigned live = trial < 5 ? sets[trial] : (unsigned)rand() & NV2A_CPU_LIVE_ALL;

				memset(&live_output, 0xA5 + trial, sizeof(live_output));
				nv2a_cpu_run_live(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f,
					&live_output, live);
				if (!live_same(&live_output, &interpreted, live))
				{
					printf("program %08lx: its function for the outputs %03x differs\n",
						nv2a_cpu_hash(instructions, instruction_count), live);
					differ++;
					break;
				}
			}
			/* the components that read no constant: the same for other
			constants, on the interpreter and as its function computes the
			outputs that hold them alone (as d3d8_gx.c keeps them) */
			{
				struct nv2a_cpu_output other, alone;

				for (row = 0; row < NV2A_CPU_CONSTANT_COUNT; row++)
					for (component = 0; component < 4; component++)
						other_constants[row][component] = random_float();
				for (row = 0; row < NV2A_CPU_CONSTANT_COUNT; row += 5)
					other_constants[row][0] = (float)(rand() % 64);
				program.native = NULL;
				nv2a_cpu_run(&program, (const float (*)[4])other_constants, (const float (*)[4])inputs, 1.0f, &other);
				program.native = run;
				memset(&alone, 0x5A + trial, sizeof(alone));
				nv2a_cpu_run_live(&program, (const float (*)[4])other_constants, (const float (*)[4])inputs, 1.0f,
					&alone, free_outputs);
				if (!constant_free_same(&reads, &interpreted, &other, NV2A_CPU_LIVE_ALL, &constant_free_checked) ||
					!constant_free_same(&reads, &interpreted, &alone, free_outputs, &constant_free_checked))
				{
					printf("program %08lx: an output that reads no constant differs with other constants\n",
						nv2a_cpu_hash(instructions, instruction_count));
					differ++;
					break;
				}
			}
			/* its colour-only function: the same colours and fog */
			{
				struct nv2a_cpu_output colors;

				memset(&colors, 0, sizeof(colors));
				nv2a_cpu_run_colors(&program, (const float (*)[4])constants, (const float (*)[4])inputs, &colors);
				if (memcmp(colors.d0, compiled.d0, sizeof(colors.d0)) || memcmp(colors.d1, compiled.d1, sizeof(colors.d1)) ||
					memcmp(&colors.fog, &compiled.fog, sizeof(colors.fog)))
				{
					printf("program %08lx: its colour-only function's colours differ\n",
						nv2a_cpu_hash(instructions, instruction_count));
					differ++;
					break;
				}
			}
			/* a model's vertex on one node (d3d8_gx.c's node_keys):
			node indices (a, b) with a weight of 1, or (b, a) with 0, give
			every output that (a, a) with 1 does */
			{
				struct nv2a_cpu_output one_node, other;
				int a = rand() % 256, b = rand() % 256, order;

				inputs[NODE_INDICES][0] = a / 255.0f;
				inputs[NODE_INDICES][1] = a / 255.0f;
				inputs[NODE_INDICES][2] = 0.0f;
				inputs[NODE_INDICES][3] = 1.0f;
				inputs[NODE_WEIGHT][0] = 1.0f;
				inputs[NODE_WEIGHT][1] = inputs[NODE_WEIGHT][2] = 0.0f;
				inputs[NODE_WEIGHT][3] = 1.0f;
				nv2a_cpu_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &one_node);
				for (order = 0; order < 2; order++)
				{
					inputs[NODE_INDICES][order] = b / 255.0f;
					inputs[NODE_WEIGHT][0] = order ? 1.0f : 0.0f;
					nv2a_cpu_run(&program, (const float (*)[4])constants, (const float (*)[4])inputs, 1.0f, &other);
					inputs[NODE_INDICES][order] = a / 255.0f;
					one_node_checked++;
					if (memcmp(&one_node, &other, sizeof(other)))
					{
						printf("program %08lx: a vertex on node %d (indices %d and %d, weight %d) differs\n",
							nv2a_cpu_hash(instructions, instruction_count), a, order ? a : b, order ? b : a, order);
						differ++;
						break;
					}
				}
				if (order < 2)
					break;
			}
		}
	}
	printf("%lu programs with functions, %lu runs compared, %lu vertices on one node, %lu differ\n", native, checked,
		one_node_checked, differ);
	printf("%lu programs with outputs the vertex's alone; %lu components that read no constant compared with other "
		"constants\n", constant_free_programs, constant_free_checked);
	differ += arithmetic_check();
	return differ != 0;
}
