/*
WII_MATH.C

sqrtf for the Wii's processor, which has no square root instruction
(the 750 lacks fsqrt). newlib's sqrtf finds the root a bit at a time, about
230 instructions a call; the game calls it for every vector's length and
normal (real_math.h's square_root, whose sqrt of a float clang makes a
call to sqrtf), hundreds of times in a frame. The link wraps sqrtf
(-Wl,--wrap=sqrtf, tools/wii_build.py), so every call, the libraries' too,
comes here.

The result is newlib's, and x86's sqrtss, bit for bit: the root correctly
rounded. An estimate of 1 / sqrt(x) (frsqrte, good to 5 bits at worst) is
refined by Newton's steps in double to 36 bits or more, the root rounded to
float, then moved to whichever neighbour x's side of the midpoint between
them says. The midpoints' squares are exact in a double (25 bits squared),
and never a float, so the comparison decides every root exactly
(port/wii/src/nv2a_cpu_native.h's nv2a_sqrt_refined does the same for the
vertex programs). A root still more than a neighbour away, which the steps
cannot leave, and zeros, infinities, NaNs and negatives are newlib's.
tools/wii_math_check.py compares it with sqrtf for every float.
*/

#include <stdint.h>
#include <string.h>

float __real_sqrtf(float x);
float __wrap_sqrtf(float x);

/* the float bits away from x's (a neighbour of x > 0) */
static inline float float_step(float x, int32_t step)
{
	uint32_t bits;

	memcpy(&bits, &x, sizeof(bits));
	bits += (uint32_t)step;
	memcpy(&x, &bits, sizeof(x));
	return x;
}

/* whether x is past the midpoint of the neighbouring floats low and high */
static inline int past_middle(double x, float low, float high)
{
	double middle = 0.5 * ((double)low + (double)high);

	return x > middle * middle;
}

static inline double reciprocal_root_estimate(double x)
{
#if defined(__PPC__) || defined(__powerpc__)
	double estimate;

	__asm__("frsqrte %0,%1" : "=f"(estimate) : "f"(x));
	return estimate;
#else
	/* (the host check's estimate, made coarse as frsqrte's by WII_MATH_ESTIMATE) */
#ifdef WII_MATH_ESTIMATE
	return WII_MATH_ESTIMATE(x);
#else
	return 1.0 / __builtin_sqrt(x);
#endif
#endif
}

float __wrap_sqrtf(float value)
{
	double x = value, y;
	float root, neighbour;

	/* (0, infinity, NaN and negatives) */
	if (!(value > 0.0f && value <= 3.40282347e+38f))
		return __real_sqrtf(value);
	y = reciprocal_root_estimate(x);
	y = y * (1.5 - 0.5 * x * y * y);
	y = y * (1.5 - 0.5 * x * y * y);
	y = y * (1.5 - 0.5 * x * y * y);
	root = (float)(x * y);
	neighbour = float_step(root, 1);
	if (past_middle(x, root, neighbour))
	{
		if (past_middle(x, neighbour, float_step(neighbour, 1)))
			return __real_sqrtf(value);
		return neighbour;
	}
	neighbour = float_step(root, -1);
	if (!past_middle(x, neighbour, root))
	{
		if (!past_middle(x, float_step(neighbour, -1), neighbour))
			return __real_sqrtf(value);
		return neighbour;
	}
	return root;
}
