/*
NV2A_CPU.H

The Xbox's vertex programs run on the Wii's processor (nv2a_cpu.c): GX has
no programmable vertex stage, so each draw's vertices go through the game's
program here, as port/linux/src/nv2a_vsh.c translates them for OpenGL, and
GX draws the results in screen space (port/wii/src/d3d8_gx.c).
*/

#ifndef __NV2A_CPU_H
#define __NV2A_CPU_H

#include "platform.h"

enum
{
	NV2A_CPU_CONSTANT_COUNT = 192,
	/* c[-96] is the first */
	NV2A_CPU_CONSTANT_BIAS = 96,
	NV2A_CPU_INPUT_COUNT = 16,
	NV2A_CPU_MAXIMUM_INSTRUCTIONS = 136,
};

struct nv2a_cpu_operand
{
	unsigned char mux, index, negate;
	unsigned char swizzle[4];
};

struct nv2a_cpu_instruction
{
	unsigned char mac, ilu;
	unsigned char mac_mask, ilu_mask, output_mask;
	unsigned char temporary, output_address, output_is_register, output_from_ilu, relative;
	/* the screen-space conversion's reciprocal of the clip-space w: the
	position is kept as it was before it (nv2a_vsh.c's clip_captured) */
	unsigned char captures_clip;
	unsigned char input, constant;
	struct nv2a_cpu_operand operands[3];
};

struct nv2a_cpu_program
{
	unsigned long count;
	struct nv2a_cpu_instruction instructions[NV2A_CPU_MAXIMUM_INSTRUCTIONS];
};

/* what a vertex leaves: the screen-space position the program computes
(pixels, and depth scaled by the viewport), the clip-space position before
its conversion if the program converts it, the colors, the texture
coordinates and the fog */
struct nv2a_cpu_output
{
	float position[4];
	float clip[4];
	int clip_captured;
	float d0[4], d1[4], b0[4], b1[4];
	float t[4][4];
	float fog;
	float point_size;
};

void nv2a_cpu_compile(const DWORD *instructions, unsigned long instruction_count, struct nv2a_cpu_program *program);
void nv2a_cpu_run(const struct nv2a_cpu_program *program, const float (*constants)[4], const float (*inputs)[4],
	float point_size, struct nv2a_cpu_output *output);

#endif
