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
#include <stdint.h>
#include <string.h>

struct nv2a_cpu_native
{
	unsigned long hash, count;
	/* (for the outputs a draw reads: NV2A_CPU_LIVE_...) */
	nv2a_cpu_live_proc run;
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

/* ARL's index, (int)floorf(x) without floorf's call: the same for every x a
float's whole part fits in an int, floorf's past that */
static inline int nv2a_floor_index(float x)
{
	int whole;

	if (!(x > -2147483648.0f && x < 2147483648.0f))
		return (int)floorf(x);
	whole = (int)x;
	return (float)whole > x ? whole - 1 : whole;
}

/* the float bits away from x's (a neighbour of x > 0) */
static inline float nv2a_float_step(float x, int32_t step)
{
	uint32_t bits;

	memcpy(&bits, &x, sizeof(bits));
	bits += (uint32_t)step;
	memcpy(&x, &bits, sizeof(x));
	return x;
}

/* whether x is past the midpoint of two neighbouring floats (exact: the
midpoint's square fits a double, and is never a float, so never x) */
static inline int nv2a_past_middle(double x, float low, float high)
{
	double middle = 0.5 * ((double)low + (double)high);

	return x > middle * middle;
}

/* sqrtf correctly rounded, as newlib's is, without its loop of a bit a
step: an estimate of 1 / sqrt(x) made exact enough by Newton's steps in
double, rounded to float, then moved to the neighbour on x's side of the
midpoint between them. The steps leave the root within a unit of the last
place; were it ever further, the library's. For x > 0 and finite. */
static inline float nv2a_sqrt_refined(float value, double estimate)
{
	double x = value, y = estimate;
	float root, neighbour;

	/* (each step about doubles the estimate's bits: 5 at worst, from
	frsqrte, give 36 or more) */
	y = y * (1.5 - 0.5 * x * y * y);
	y = y * (1.5 - 0.5 * x * y * y);
	y = y * (1.5 - 0.5 * x * y * y);
	root = (float)(x * y);
	neighbour = nv2a_float_step(root, 1);
	if (nv2a_past_middle(x, root, neighbour))
	{
		if (nv2a_past_middle(x, neighbour, nv2a_float_step(neighbour, 1)))
			return sqrtf(value);
		return neighbour;
	}
	neighbour = nv2a_float_step(root, -1);
	if (!nv2a_past_middle(x, neighbour, root))
	{
		if (!nv2a_past_middle(x, nv2a_float_step(neighbour, -1), neighbour))
			return sqrtf(value);
		return neighbour;
	}
	return root;
}

static inline double nv2a_reciprocal_root_estimate(double x)
{
#if defined(__PPC__) || defined(__powerpc__)
	double estimate;

	__asm__("frsqrte %0,%1" : "=f"(estimate) : "f"(x));
	return estimate;
#else
	return 1.0 / sqrt(x);
#endif
}

static inline float nv2a_sqrtf(float x)
{
	/* (0, infinity, NaN and negatives: the library's) */
	if (!(x > 0.0f && x <= 3.40282347e+38f))
		return sqrtf(x);
	return nv2a_sqrt_refined(x, nv2a_reciprocal_root_estimate(x));
}

#endif
