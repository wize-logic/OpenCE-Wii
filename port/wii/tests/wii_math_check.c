/*
WII_MATH_CHECK.C

A host check of port/wii/src/wii_math.c's sqrtf (tools/wii_math_check.py
builds and runs it): its result against the host's sqrtf, bit for bit, from
estimates of 1 / sqrt(x) as coarse as the 750's frsqrte (5 bits) and off by
1/32 either way, for every 199th positive float (each of the smallest), or
with --all every one; and its zeros, infinities, NaNs and negatives. A
result the refinement could not decide would be the library's (counted as
a fallback, which it never should be).

usage: wii_math_check [--all]
*/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int estimate_kind;
static unsigned long fallbacks;

/* an estimate of 1 / sqrt(x) to 5 bits (frsqrte's worst), off by 1/32
either way as estimate_kind says */
static double coarse_estimate(double x)
{
	double exact = 1.0 / sqrt(x), scale;
	int exponent;

	scale = frexp(exact, &exponent);
	scale = floor(scale * 32.0) / 32.0;
	return ldexp(scale, exponent) *
		(estimate_kind == 0 ? 1.0 : estimate_kind == 1 ? 1.0 + 1.0 / 32.0 : 1.0 - 1.0 / 32.0);
}

#define WII_MATH_ESTIMATE coarse_estimate
#include "../src/wii_math.c"

float __real_sqrtf(float x)
{
	if (x > 0.0f && x <= 3.40282347e+38f)
		fallbacks++;
	return sqrtf(x);
}

int main(int argc, char **argv)
{
	static const float specials[] = { 0.0f, -0.0f, 1.0f / 0.0f, -1.0f, -1.0f / 0.0f, 3.40282347e+38f, 1.0e-45f,
		1.17549435e-38f, 1.0f, 2.0f, 4.0f };
	int all = argc > 1 && !strcmp(argv[1], "--all");
	unsigned long checked = 0, differ = 0;
	uint32_t bits;
	unsigned i;

	for (estimate_kind = 0; estimate_kind < 3; estimate_kind++)
	{
		for (bits = 1; bits < 0x7F800000u; bits += all || bits < 0x00100000u ? 1 : 199)
		{
			float x, exact, root;

			memcpy(&x, &bits, sizeof(x));
			exact = sqrtf(x);
			root = __wrap_sqrtf(x);
			checked++;
			if (memcmp(&root, &exact, sizeof(exact)))
			{
				if (differ < 8)
					printf("sqrtf(%a): %a, the host's %a\n", x, root, exact);
				differ++;
			}
		}
	}
	for (i = 0; i < sizeof(specials) / sizeof(specials[0]); i++)
	{
		float root = __wrap_sqrtf(specials[i]), exact = sqrtf(specials[i]);

		checked++;
		if (memcmp(&root, &exact, sizeof(exact)) && !(root != root && exact != exact))
		{
			printf("sqrtf(%a): %a, the host's %a\n", specials[i], root, exact);
			differ++;
		}
	}
	{
		float nan = __wrap_sqrtf(0.0f / 0.0f);

		checked++;
		if (nan == nan)
		{
			printf("sqrtf(NaN) is not a NaN\n");
			differ++;
		}
	}
	printf("%lu square roots compared, %lu differ, %lu left to the library\n", checked, differ, fallbacks);
	return differ || fallbacks ? 1 : 0;
}
