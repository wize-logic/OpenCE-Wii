/*
NV2A_CPU_NATIVE.H

The game's vertex programs compiled ahead of time
(port/wii/tools/nv2a_codegen.c writes them, nv2a_cpu.c runs them): what
their functions share with the interpreter.
*/

#ifndef __NV2A_CPU_NATIVE_H
#define __NV2A_CPU_NATIVE_H

#include "nv2a_cpu.h"

#include <math.h>
#include <string.h>

struct nv2a_cpu_native
{
	unsigned long hash, count;
	nv2a_cpu_native_proc run;
	/* its colours (d0, d1) and fog alone */
	nv2a_cpu_native_proc colors;
};

extern const struct nv2a_cpu_native nv2a_cpu_natives[];
extern const unsigned long nv2a_cpu_native_count;

/* a program's words hashed (FNV-1a), which finds its function */
unsigned long nv2a_cpu_hash(const DWORD *instructions, unsigned long instruction_count);
void nv2a_cpu_ilu(int ilu, const float c[4], float out[4]);

static const float nv2a_zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

/* a relative constant's index, kept in the constants */
static inline int nv2a_clamp(int index)
{
	return index < 0 ? 0 : index >= NV2A_CPU_CONSTANT_COUNT ? NV2A_CPU_CONSTANT_COUNT - 1 : index;
}

/* RCC: the reciprocal, clamped away from 0 and infinity */
static inline float reciprocal_clamped(float x)
{
	float r = 1.0f / x;

	if (r > 0.0f)
		return r < 5.42101e-20f ? 5.42101e-20f : r > 1.884467e+19f ? 1.884467e+19f : r;
	return r > -5.42101e-20f ? -5.42101e-20f : r < -1.884467e+19f ? -1.884467e+19f : r;
}

#endif
