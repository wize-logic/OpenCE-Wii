/*
NV2A_NATIVE_CHECK.C

A host check of the vertex programs compiled ahead of time
(port/wii/tools/nv2a_codegen.c; tools/wii_nv2a_check.py builds and runs it):
each of the game's programs is run as its function and on the interpreter
(port/wii/src/nv2a_cpu.c) over random constants and inputs, and the two
must give the same outputs, bit for bit.

usage: nv2a_native_check microcode.inc
*/

#include "nv2a_cpu_native.h"

#include <stdio.h>
#include <stdlib.h>

static float random_float(void)
{
	return (float)(rand() % 20001 - 10000) / 1000.0f;
}

int main(int argc, char **argv)
{
	static DWORD words[65536];
	static float constants[NV2A_CPU_CONSTANT_COUNT][4], inputs[NV2A_CPU_INPUT_COUNT][4];
	static struct nv2a_cpu_program program;
	unsigned long count = 0, position, checked = 0, differ = 0, native = 0;
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
		for (trial = 0; trial < 64; trial++)
		{
			struct nv2a_cpu_output compiled, interpreted;
			nv2a_cpu_native_proc run = program.native;
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
		}
	}
	printf("%lu programs with functions, %lu runs compared, %lu differ\n", native, checked, differ);
	return differ != 0;
}
