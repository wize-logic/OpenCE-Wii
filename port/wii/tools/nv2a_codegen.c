/*
NV2A_CODEGEN.C

The game's vertex programs as C (a host tool; tools/wii_build.py runs it):
each program of the game's vertex shader microcode
(source/rasterizer/xbox/rasterizer_xbox_vertex_shaders_data.inc) is decoded
by port/wii/src/nv2a_cpu.c's own decoder and written out as a function that
does what nv2a_cpu_run does for it, its operands, swizzles and registers
fixed, so the compiler keeps them in registers. nv2a_cpu_compile finds a
program's function by a hash of its words (nv2a_cpu_hash); a program not
among them runs on the interpreter.

usage: nv2a_codegen microcode.inc output.c
*/

#include "../src/nv2a_cpu.c"

#include <stdio.h>
#include <stdlib.h>

#define MAXIMUM_WORDS 65536
#define MAXIMUM_PROGRAMS 512

static const char components[] = "xyzw";

/* an operand's source, as read_operand chooses it */
static void source(char *text, size_t size, const struct nv2a_cpu_instruction *instruction,
	const struct nv2a_cpu_operand *operand)
{
	switch (operand->mux)
	{
	case _mux_temporary:
		if (operand->index == 12)
			snprintf(text, size, "p");
		else
			snprintf(text, size, "r[%d]", operand->index < 12 ? operand->index : 0);
		break;
	case _mux_input:
		snprintf(text, size, "v[%d]", instruction->input);
		break;
	case _mux_constant:
		if (instruction->relative)
			snprintf(text, size, "c[nv2a_clamp(%d + a0)]", instruction->constant);
		else
			snprintf(text, size, "c[%d]", instruction->constant < NV2A_CPU_CONSTANT_COUNT ? instruction->constant :
				NV2A_CPU_CONSTANT_COUNT - 1);
		break;
	default:
		snprintf(text, size, "nv2a_zero");
		break;
	}
}

static void operand(FILE *out, const char *name, const struct nv2a_cpu_instruction *instruction,
	const struct nv2a_cpu_operand *value)
{
	char text[64];
	int component;

	source(text, sizeof(text), instruction, value);
	for (component = 0; component < 4; component++)
		fprintf(out, "\t\t%s[%d] = %s%s[%d];\n", name, component, value->negate ? "-" : "", text,
			value->swizzle[component]);
}

static void masked(FILE *out, const char *destination, const char *value, unsigned mask)
{
	int component;

	/* (bit 3 is x) */
	for (component = 0; component < 4; component++)
	{
		if (mask & (8u >> component))
			fprintf(out, "\t\t%s[%d] = %s[%d];\n", destination, component, value, component);
	}
}

static const char *temporary_name(unsigned temporary, char *text, size_t size)
{
	if (temporary == 12)
		snprintf(text, size, "p");
	else
		snprintf(text, size, "r[%u]", temporary < 12 ? temporary : 0);
	return text;
}

/* writing a program's colour-only function: its texture coordinates, back
colours, point size and clip-space position are left out, and the compiler
drops what only they needed (d3d8_gx.c's draws whose colours alone are
the processor's) */
static int colors_only;

static void instruction_code(FILE *out, const struct nv2a_cpu_instruction *instruction, unsigned long index)
{
	char text[32];
	int component;

	fprintf(out, "\t/* %lu */\n\t{\n", index);
	if (instruction->mac != _mac_nop)
	{
		operand(out, "A", instruction, &instruction->operands[0]);
		if (instruction->mac != _mac_mov && instruction->mac != _mac_arl && instruction->mac != _mac_add)
			operand(out, "B", instruction, &instruction->operands[1]);
	}
	if (instruction->ilu != _ilu_nop || instruction->mac == _mac_add || instruction->mac == _mac_mad)
		operand(out, "C", instruction, &instruction->operands[2]);
	switch (instruction->mac)
	{
	case _mac_mov: case _mac_arl:
		for (component = 0; component < 4; component++)
			fprintf(out, "\t\tM[%d] = A[%d];\n", component, component);
		break;
	case _mac_mul: case _mac_add: case _mac_mad: case _mac_min: case _mac_max: case _mac_slt: case _mac_sge:
		for (component = 0; component < 4; component++)
		{
			switch (instruction->mac)
			{
			case _mac_mul: fprintf(out, "\t\tM[%d] = A[%d] * B[%d];\n", component, component, component); break;
			case _mac_add: fprintf(out, "\t\tM[%d] = A[%d] + C[%d];\n", component, component, component); break;
			case _mac_mad: fprintf(out, "\t\tM[%d] = A[%d] * B[%d] + C[%d];\n", component, component, component, component); break;
			case _mac_min: fprintf(out, "\t\tM[%d] = A[%d] < B[%d] ? A[%d] : B[%d];\n", component, component, component, component, component); break;
			case _mac_max: fprintf(out, "\t\tM[%d] = A[%d] > B[%d] ? A[%d] : B[%d];\n", component, component, component, component, component); break;
			case _mac_slt: fprintf(out, "\t\tM[%d] = A[%d] < B[%d] ? 1.0f : 0.0f;\n", component, component, component); break;
			default: fprintf(out, "\t\tM[%d] = A[%d] >= B[%d] ? 1.0f : 0.0f;\n", component, component, component); break;
			}
		}
		break;
	case _mac_dp3:
		fprintf(out, "\t\tM[0] = M[1] = M[2] = M[3] = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];\n");
		break;
	case _mac_dph:
		fprintf(out, "\t\tM[0] = M[1] = M[2] = M[3] = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + B[3];\n");
		break;
	case _mac_dp4:
		fprintf(out, "\t\tM[0] = M[1] = M[2] = M[3] = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + A[3] * B[3];\n");
		break;
	case _mac_dst:
		fprintf(out, "\t\tM[0] = 1.0f; M[1] = A[1] * B[1]; M[2] = A[2]; M[3] = B[3];\n");
		break;
	default:
		/* (no MAC: its result is 0) */
		fprintf(out, "\t\tM[0] = M[1] = M[2] = M[3] = 0.0f;\n");
		break;
	}
	switch (instruction->ilu)
	{
	case _ilu_mov: fprintf(out, "\t\tI[0] = C[0]; I[1] = C[1]; I[2] = C[2]; I[3] = C[3];\n"); break;
	case _ilu_rcp: fprintf(out, "\t\tI[0] = I[1] = I[2] = I[3] = 1.0f / C[0];\n"); break;
	case _ilu_rcc: fprintf(out, "\t\tI[0] = I[1] = I[2] = I[3] = reciprocal_clamped(C[0]);\n"); break;
	case _ilu_rsq: fprintf(out, "\t\tI[0] = I[1] = I[2] = I[3] = 1.0f / sqrtf(fabsf(C[0]));\n"); break;
	case _ilu_exp: case _ilu_log: case _ilu_lit:
		fprintf(out, "\t\tnv2a_cpu_ilu(%d, C, I);\n", instruction->ilu);
		break;
	default:
		fprintf(out, "\t\tI[0] = I[1] = I[2] = I[3] = 0.0f;\n");
		break;
	}
	if (instruction->captures_clip && !colors_only)
		fprintf(out, "\t\tmemcpy(o->clip, p, sizeof(o->clip));\n\t\to->clip_captured = 1;\n");
	if (instruction->mac == _mac_arl)
		fprintf(out, "\t\ta0 = (int)floorf(M[0] + 0.001f);\n");
	else if (instruction->mac != _mac_nop && instruction->mac_mask)
		masked(out, temporary_name(instruction->temporary, text, sizeof(text)), "M", instruction->mac_mask);
	if (instruction->ilu != _ilu_nop && instruction->ilu_mask)
	{
		unsigned temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;

		masked(out, temporary_name(temporary, text, sizeof(text)), "I", instruction->ilu_mask);
	}
	if (instruction->output_mask && instruction->output_is_register &&
		(instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0)
	{
		const char *value = instruction->output_from_ilu ? "I" : "M";

		switch (instruction->output_address)
		{
		case _output_position: masked(out, "p", value, instruction->output_mask); break;
		case _output_d0: masked(out, "o->d0", value, instruction->output_mask); break;
		case _output_d1: masked(out, "o->d1", value, instruction->output_mask); break;
		case _output_b0: if (!colors_only) masked(out, "o->b0", value, instruction->output_mask); break;
		case _output_b1: if (!colors_only) masked(out, "o->b1", value, instruction->output_mask); break;
		case _output_fog:
			if (instruction->output_mask & 8)
				fprintf(out, "\t\to->fog = %s[0];\n", value);
			break;
		case _output_point_size:
			if ((instruction->output_mask & 8) && !colors_only)
				fprintf(out, "\t\to->point_size = %s[0];\n", value);
			break;
		default:
			if (instruction->output_address >= _output_t0 && instruction->output_address < _output_t0 + 4 &&
				!colors_only)
			{
				snprintf(text, sizeof(text), "o->t[%d]", instruction->output_address - _output_t0);
				masked(out, text, value, instruction->output_mask);
			}
			break;
		}
	}
	fprintf(out, "\t}\n");
	(void)components;
}

/* one output's dependencies: its components' input registers, "!" where it
is not affine (nv2a_cpu_analyze) */
static void dependency_text(FILE *out, const char *name, const unsigned long *components, int count)
{
	unsigned long all = 0;
	int component, input;

	for (component = 0; component < count; component++)
		all |= components[component];
	if (!all)
		return;
	fprintf(out, " %s:", name);
	for (input = 0; input < 16; input++)
		if (all & (1UL << input))
			fprintf(out, "v%d,", input);
	fprintf(out, "%s", all & NV2A_CPU_NOT_AFFINE ? "!" : "");
}

static void dependencies_comment(FILE *out, const struct nv2a_cpu_program *program)
{
	struct nv2a_cpu_dependencies dependencies;
	char name[8];
	int stage;

	nv2a_cpu_analyze(program, &dependencies);
	fprintf(out, "/* depends on");
	dependency_text(out, dependencies.clip_captured ? "clip" : "position",
		dependencies.clip_captured ? dependencies.clip : dependencies.position, 4);
	dependency_text(out, "d0", dependencies.d0, 4);
	dependency_text(out, "d1", dependencies.d1, 4);
	for (stage = 0; stage < 4; stage++)
	{
		snprintf(name, sizeof(name), "t%d", stage);
		dependency_text(out, name, dependencies.t[stage], 2);
	}
	dependency_text(out, "fog", &dependencies.fog, 1);
	fprintf(out, " */\n");
}

int main(int argc, char **argv)
{
	static DWORD words[MAXIMUM_WORDS];
	static unsigned long hashes[MAXIMUM_PROGRAMS], counts[MAXIMUM_PROGRAMS];
	static struct nv2a_cpu_program program;
	unsigned long word_count = 0, position, program_count = 0, index;
	char line[512];
	FILE *in, *out;
	int in_comment = 0;

	if (argc != 3)
	{
		fprintf(stderr, "usage: nv2a_codegen microcode.inc output.c\n");
		return 2;
	}
	in = fopen(argv[1], "r");
	if (!in)
	{
		perror(argv[1]);
		return 1;
	}
	/* the hexadecimal words, past the comment */
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
			else if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
			{
				if (word_count >= MAXIMUM_WORDS)
				{
					fprintf(stderr, "nv2a_codegen: more than %d words\n", MAXIMUM_WORDS);
					return 1;
				}
				words[word_count++] = (DWORD)strtoul(cursor, &cursor, 16);
			}
			else
			{
				cursor++;
			}
		}
	}
	fclose(in);

	out = fopen(argv[2], "w");
	if (!out)
	{
		perror(argv[2]);
		return 1;
	}
	fprintf(out,
		"/* generated by port/wii/tools/nv2a_codegen.c from %s: do not edit */\n\n"
		"#include \"nv2a_cpu_native.h\"\n\n", argv[1]);
	/* each program: its header (the instruction count in the high half),
	then four words an instruction */
	for (position = 0; position < word_count; )
	{
		unsigned long count = words[position] >> 16;
		const DWORD *instructions = words + position + 1;
		unsigned long hash, earlier;

		position += 1 + count * 4;
		if (!count || position > word_count)
			continue;
		hash = nv2a_cpu_hash(instructions, count);
		for (earlier = 0; earlier < program_count; earlier++)
		{
			if (hashes[earlier] == hash && counts[earlier] == count)
				break;
		}
		if (earlier < program_count || program_count >= MAXIMUM_PROGRAMS)
			continue;
		hashes[program_count] = hash;
		counts[program_count] = count;
		program_count++;
		nv2a_cpu_compile(instructions, count, &program);
		dependencies_comment(out, &program);
		fprintf(out, "static void program_%08lx(const float (*c)[4], const float (*v)[4], struct nv2a_cpu_output *o)\n"
			"{\n\tfloat r[12][4] = { { 0.0f } };\n\tfloat p[4] = { 0.0f, 0.0f, 0.0f, 1.0f };\n"
			"\tfloat A[4], B[4], C[4], M[4], I[4];\n\tint a0 = 0;\n\n"
			"\t(void)c;\n\t(void)v;\n\t(void)a0;\n\t(void)A;\n\t(void)B;\n\t(void)C;\n\t(void)r;\n", hash);
		for (index = 0; index < program.count; index++)
			instruction_code(out, &program.instructions[index], index);
		fprintf(out, "\tmemcpy(o->position, p, sizeof(o->position));\n}\n\n");
		/* (its colours and fog alone) */
		fprintf(out, "static void program_%08lx_colors(const float (*c)[4], const float (*v)[4], struct nv2a_cpu_output *o)\n"
			"{\n\tfloat r[12][4] = { { 0.0f } };\n\tfloat p[4] = { 0.0f, 0.0f, 0.0f, 1.0f };\n"
			"\tfloat A[4], B[4], C[4], M[4], I[4];\n\tint a0 = 0;\n\n"
			"\t(void)c;\n\t(void)v;\n\t(void)a0;\n\t(void)A;\n\t(void)B;\n\t(void)C;\n\t(void)r;\n\t(void)p;\n", hash);
		colors_only = 1;
		for (index = 0; index < program.count; index++)
			instruction_code(out, &program.instructions[index], index);
		colors_only = 0;
		fprintf(out, "}\n\n");
	}
	fprintf(out, "const struct nv2a_cpu_native nv2a_cpu_natives[] =\n{\n");
	for (index = 0; index < program_count; index++)
		fprintf(out, "\t{ 0x%08lxUL, %lu, program_%08lx, program_%08lx_colors },\n", hashes[index], counts[index],
			hashes[index], hashes[index]);
	fprintf(out, "};\n\nconst unsigned long nv2a_cpu_native_count = %lu;\n", program_count);
	fclose(out);
	printf("nv2a_codegen: %lu programs from %lu words\n", program_count, word_count);
	return 0;
}
