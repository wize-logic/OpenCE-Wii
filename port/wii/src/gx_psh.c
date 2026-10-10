/*
GX_PSH.C

The register combiners on GX's texture environment (TEV).

An Xbox pixel shader (port/linux/src/nv2a_psh.c has the whole of it) runs up
to eight general combiner stages, each computing A*B and C*D on its
registers, for the colour and the alpha apart, and writing them or their
sum (or a choice of the two) back; then the final combiner, A*B + (1-A)*C + D
for the colour and G for the alpha. A TEV stage computes
d + (1-c)*a + c*b, for the colour and the alpha apart, on 8-bit inputs, with
one texture and one rasterized colour, and writes it to one of four
registers. So:

- each product is a stage (c*b, or (1-c)*a for an inverted input), a sum
  two, and the final combiner one, its lerp TEV's own;
- registers are renamed as they are written: a combiner register is wherever
  its value last went (a TEV register, or the texture or colour it still is
  until written), so a stage's inputs are all read before its outputs land;
- an input the stage cannot reach as it is (a second texture or colour, a
  second constant, both inputs of a product inverted, a signed mapping) is
  first computed into a free register by a stage of its own;
- the colour and alpha halves of a combiner stage share TEV stages where
  their textures and colours agree.

GX's inputs are unsigned and clamped: the signed mappings (expand, half
bias) come out clamped to [0, 1], a negation as 0, and a dot product as 1
(the bump mapping it serves is not drawn). The fog register is no fog (the
fog colour black, its factor 1).
*/

#include "gx_psh.h"

#include <string.h>

#define BOOL GCBOOL
#include <ogc/gx.h>
#undef BOOL

/* ---------- constants */

enum
{
	_register_zero = 0,
	_register_c0 = 1,
	_register_c1 = 2,
	_register_fog = 3,
	_register_v0 = 4,
	_register_v1 = 5,
	_register_t0 = 8,
	_register_t1 = 9,
	_register_t2 = 10,
	_register_t3 = 11,
	_register_r0 = 12,
	_register_r1 = 13,
	_register_v1r0_sum = 14,
	_register_ef_product = 15,
};

/* the combiner registers a stage can write, by name */
enum
{
	_name_v0, _name_v1, _name_t0, _name_t1, _name_t2, _name_t3, _name_r0, _name_r1,
	NUMBER_OF_NAMES
};

enum { _portion_color, _portion_alpha, NUMBER_OF_PORTIONS };

/* an operand: where a value is */
enum
{
	_kind_zero, _kind_one, _kind_half,
	/* TEV's registers: PREV, REG0, REG1, REG2 */
	_kind_register,
	/* a texture map's texel (index: the map) */
	_kind_texture,
	/* a rasterized colour (index: 0 diffuse, 1 specular) */
	_kind_ras,
	/* a constant (index: its slot) */
	_kind_konst,
};

enum
{
	/* the colour (in the colour half), or the alpha (in the alpha half) */
	_channel_natural,
	/* the alpha, read in the colour half */
	_channel_alpha,
	/* the blue, read in the alpha half */
	_channel_blue,
	/* the red, green or blue in all three of the colour half's channels
	(a dot product's terms, through the swap tables or the constant's
	selection) */
	_channel_red_all,
	_channel_green_all,
	_channel_blue_all,
};

#define NO_RESOURCE 0xFF
#define MAXIMUM_OPERATIONS 48

/* ---------- structures */

struct operand
{
	unsigned char kind, index, channel, invert;
	/* signed negation: a product with it is subtracted (TEV's SUB) */
	unsigned char negative;
};

struct operation
{
	struct operand in[4];
	unsigned char subtract, bias, scale, dest;
};

struct translation
{
	const DWORD *state;
	const struct gx_psh_textures *textures;
	struct gx_psh_program *program;
	BOOL failed;
	const char *reason;
	/* the second attempt, for a program the exact one does not fit: the
	mappings that would take a register of their own, and the dot
	products that would take three stages, approximated */
	BOOL economical;
	/* where each name is, for each portion */
	struct operand names[NUMBER_OF_PORTIONS][NUMBER_OF_NAMES];
	/* what holds each TEV register, for each portion (names and temporaries) */
	unsigned char holders[NUMBER_OF_PORTIONS][4];
	/* registers whose names moved this combiner stage, freed at its end */
	unsigned char released[NUMBER_OF_PORTIONS][4];
	/* the combiner stage being translated, and its portion */
	int stage, portion_now;
	/* the colour portion's operands of this combiner stage */
	struct operand color_operands[4];
	/* this combiner stage's operations, for each portion, merged at its end */
	struct operation queue[NUMBER_OF_PORTIONS][MAXIMUM_OPERATIONS];
	unsigned queue_count[NUMBER_OF_PORTIONS];
};

/* ---------- operands */

static void fail(struct translation *translation, const char *reason)
{
	if (!translation->failed)
		translation->reason = reason;
	translation->failed = TRUE;
}

static struct operand operand(unsigned char kind, unsigned char index, unsigned char channel)
{
	struct operand result;

	result.kind = kind;
	result.index = index;
	result.channel = channel;
	result.invert = 0;
	result.negative = 0;
	return result;
}

static const struct operand zero = { _kind_zero, 0, 0, 0, 0 };
static const struct operand one = { _kind_one, 0, 0, 0, 0 };

/* an operand with its inversion folded where it can be (constants) */
static struct operand folded(struct operand value)
{
	if (value.invert)
	{
		if (value.kind == _kind_zero)
			return one;
		if (value.kind == _kind_one)
			return zero;
		if (value.kind == _kind_half)
			value.invert = 0;
	}
	return value;
}

static unsigned char konst_slot(struct translation *translation, DWORD color)
{
	struct gx_psh_program *program = translation->program;
	unsigned slot;

	for (slot = 0; slot < program->konst_count; slot++)
	{
		if (program->konst[slot] == color)
			return (unsigned char)slot;
	}
	if (program->konst_count < 4)
	{
		program->konst[program->konst_count] = color;
		return (unsigned char)program->konst_count++;
	}
	/* (more than four constants: the last stands in) */
	program->approximate = TRUE;
	return 3;
}

/* a texture stage's texel, read in the portion */
static struct operand texture_operand(struct translation *translation, int stage, int portion)
{
	const struct gx_psh_textures *textures = translation->textures;

	if (!(textures->present & (1U << stage)))
		return zero;
	if (portion == _portion_color)
	{
		if (textures->contents[stage] == _gx_psh_texture_alpha)
			return one;
		return operand(_kind_texture, (unsigned char)stage, _channel_natural);
	}
	if (textures->contents[stage] == _gx_psh_texture_color)
		return one;
	if (textures->separate_alpha & (1U << stage))
		return operand(_kind_texture, (unsigned char)(stage + 4), _channel_natural);
	return operand(_kind_texture, (unsigned char)stage, _channel_natural);
}

/* where the names start: t0-t3 their textures, v0 and v1 the colours, r0
the alpha of t0 (as the hardware starts it), r1 nothing */
static void names_initialize(struct translation *translation)
{
	int portion, stage;

	for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++)
	{
		for (stage = 0; stage < 4; stage++)
			translation->names[portion][_name_t0 + stage] = texture_operand(translation, stage, portion);
		translation->names[portion][_name_v0] = operand(_kind_ras, 0, _channel_natural);
		translation->names[portion][_name_v1] = operand(_kind_ras, 1, _channel_natural);
		translation->names[portion][_name_r1] = zero;
	}
	translation->names[_portion_color][_name_r0] = zero;
	translation->names[_portion_alpha][_name_r0] = texture_operand(translation, 0, _portion_alpha);
}

static int register_of_name(int name)
{
	static const unsigned char registers[NUMBER_OF_NAMES] =
		{ 4, 5, 8, 9, 10, 11, 12, 13 };

	return registers[name];
}

static int name_of(unsigned long reg)
{
	switch (reg)
	{
	case _register_v0: return _name_v0;
	case _register_v1: return _name_v1;
	case _register_t0: return _name_t0;
	case _register_t1: return _name_t1;
	case _register_t2: return _name_t2;
	case _register_t3: return _name_t3;
	case _register_r0: return _name_r0;
	case _register_r1: return _name_r1;
	default: return -1;
	}
}

/* a register read in a portion (alpha_channel: its alpha in the colour
portion, in the alpha portion its alpha rather than its blue) */
static struct operand register_operand(struct translation *translation, unsigned long reg, BOOL alpha_channel,
	int portion, int stage)
{
	const DWORD *state = translation->state;
	struct operand result;
	int name = name_of(reg);

	if (reg == _register_c0 || reg == _register_c1)
	{
		DWORD color;

		if (stage < 0)
		{
			color = state[reg == _register_c0 ? D3DRS_PSFINALCOMBINERCONSTANT0 : D3DRS_PSFINALCOMBINERCONSTANT1];
		}
		else
		{
			DWORD count = state[D3DRS_PSCOMBINERCOUNT];
			BOOL unique = reg == _register_c0 ? (count & 0x1000) != 0 : (count & 0x10000) != 0;

			color = state[(reg == _register_c0 ? D3DRS_PSCONSTANT0_0 : D3DRS_PSCONSTANT1_0) + (unique ? stage : 0)];
		}
		result = operand(_kind_konst, konst_slot(translation, color), _channel_natural);
	}
	else if (reg == _register_fog)
	{
		/* the fog register: its colour the fog's, its alpha the vertex's fog
		factor, read from the fog ramp (GX_PSH_FOG_MAP: an intensity equal to
		its coordinate, the factor); 1 without fog */
		const struct gx_psh_textures *textures = translation->textures;

		if (portion == _portion_alpha || alpha_channel)
		{
			if (!textures->fog)
				return one;
			result = operand(_kind_texture, GX_PSH_FOG_MAP, _channel_natural);
			if (portion == _portion_color)
				result.channel = _channel_alpha;
			return result;
		}
		result = operand(_kind_konst, konst_slot(translation, textures->fog_color | 0xFF000000UL), _channel_natural);
		return result;
	}
	else if (name >= 0)
	{
		if (portion == _portion_color)
		{
			result = translation->names[alpha_channel ? _portion_alpha : _portion_color][name];
			if (alpha_channel && result.kind >= _kind_register)
				result.channel = _channel_alpha;
		}
		else if (alpha_channel)
		{
			result = translation->names[_portion_alpha][name];
		}
		else
		{
			/* the blue: a texture's, a colour's or a constant's through the
			swap tables; a register's alpha stands in for its blue */
			result = translation->names[_portion_color][name];
			if (result.kind == _kind_register)
				result = translation->names[_portion_alpha][name];
			else if (result.kind >= _kind_texture)
				result.channel = _channel_blue;
		}
		return result;
	}
	else
	{
		return zero;
	}
	if (portion == _portion_color)
		result.channel = alpha_channel ? _channel_alpha : _channel_natural;
	else
		result.channel = alpha_channel ? _channel_natural : _channel_blue;
	return result;
}

/* ---------- registers */

static int register_allocate(struct translation *translation, int portion)
{
	int index;

	/* (PREV last: the final combiner's) */
	for (index = 1; index <= 4; index++)
	{
		int reg = index & 3;

		if (!translation->holders[portion][reg] && !translation->released[portion][reg])
		{
			translation->holders[portion][reg] = 1;
			return reg;
		}
	}
	return -1;
}

static void register_free(struct translation *translation, int portion, int reg)
{
	if (reg >= 0 && translation->holders[portion][reg])
		translation->holders[portion][reg]--;
}

/* ---------- operations */

static void queue(struct translation *translation, int portion, const struct operation *operation)
{
	if (translation->queue_count[portion] >= MAXIMUM_OPERATIONS)
	{
		fail(translation, "too many operations");
		return;
	}
	translation->queue[portion][translation->queue_count[portion]++] = *operation;
}

static struct operation operation_new(int dest)
{
	struct operation result;

	memset(&result, 0, sizeof(result));
	result.in[0] = result.in[1] = result.in[2] = result.in[3] = zero;
	result.dest = (unsigned char)dest;
	return result;
}

/* the resources one operand takes: a texture map (and the swap it reads
with), a rasterized colour (and its swap), a constant selection */
struct resources
{
	unsigned char texture_map, texture_swap, ras, ras_swap, konst;
};

static void resources_clear(struct resources *resources)
{
	memset(resources, NO_RESOURCE, sizeof(*resources));
}

/* the constant selection an operand takes in a portion */
static unsigned char konst_selection(const struct operand *value, int portion)
{
	if (portion == _portion_color)
	{
		if (value->kind != _kind_konst)
			return NO_RESOURCE;
		switch (value->channel)
		{
		case _channel_alpha: return (unsigned char)(GX_TEV_KCSEL_K0_A + value->index);
		case _channel_red_all: return (unsigned char)(GX_TEV_KCSEL_K0_R + value->index);
		case _channel_green_all: return (unsigned char)(GX_TEV_KCSEL_K0_G + value->index);
		case _channel_blue_all: return (unsigned char)(GX_TEV_KCSEL_K0_B + value->index);
		default: return (unsigned char)(GX_TEV_KCSEL_K0 + value->index);
		}
	}
	switch (value->kind)
	{
	case _kind_one: return GX_TEV_KASEL_1;
	case _kind_half: return GX_TEV_KASEL_1_2;
	case _kind_konst:
		return (unsigned char)(value->channel == _channel_blue ? GX_TEV_KASEL_K0_B + value->index :
			GX_TEV_KASEL_K0_A + value->index);
	default: return NO_RESOURCE;
	}
}

/* adds what an operand takes; FALSE when it clashes with what is there */
static BOOL resources_add(struct resources *resources, const struct operand *value, int portion)
{
	unsigned char konst = konst_selection(value, portion);
	unsigned char swap;

	if (konst != NO_RESOURCE)
	{
		if (resources->konst != NO_RESOURCE && resources->konst != konst)
			return FALSE;
		resources->konst = konst;
	}
	if (value->kind != _kind_texture && value->kind != _kind_ras)
		return TRUE;
	/* the swap tables the read can take (gx_psh_initialize), as a set: 0
	as it is (RGBA), 1 the red everywhere, 2 the green, 3 the blue in the
	alpha (RGBB, the colour as it is) */
	switch (value->channel)
	{
	case _channel_red_all: swap = 1u << 1; break;
	case _channel_green_all: swap = 1u << 2; break;
	case _channel_blue_all: case _channel_blue: swap = 1u << 3; break;
	case _channel_alpha: swap = 1u << 0; break;
	default: swap = portion == _portion_color ? (1u << 0) | (1u << 3) : 1u << 0; break;
	}
	if (value->kind == _kind_texture)
	{
		if (resources->texture_map != NO_RESOURCE && resources->texture_map != value->index)
			return FALSE;
		if (!(resources->texture_swap & swap))
			return FALSE;
		resources->texture_map = value->index;
		resources->texture_swap &= swap;
	}
	else
	{
		if (resources->ras != NO_RESOURCE && resources->ras != value->index)
			return FALSE;
		if (!(resources->ras_swap & swap))
			return FALSE;
		resources->ras = value->index;
		resources->ras_swap &= swap;
	}
	return TRUE;
}

static BOOL resources_merge(struct resources *into, const struct resources *other)
{
	struct resources merged = *into;

	if (other->texture_map != NO_RESOURCE)
	{
		if (merged.texture_map != NO_RESOURCE && merged.texture_map != other->texture_map)
			return FALSE;
		merged.texture_map = other->texture_map;
	}
	/* (swaps are sets of the tables that serve: their intersection) */
	merged.texture_swap &= other->texture_swap;
	if (!merged.texture_swap)
		return FALSE;
	if (other->ras != NO_RESOURCE)
	{
		if (merged.ras != NO_RESOURCE && merged.ras != other->ras)
			return FALSE;
		merged.ras = other->ras;
	}
	merged.ras_swap &= other->ras_swap;
	if (!merged.ras_swap)
		return FALSE;
	*into = merged;
	return TRUE;
}

static struct operand materialized(struct translation *translation, int portion, struct operand value);
static BOOL register_available(const struct translation *translation, int portion);
static BOOL operand_reads(const struct operand *value, int reading_portion, int reg, int pool);

/* whether an operation reads a register of its portion's pool, other than
through input `except` */
static BOOL operation_reads(const struct operation *operation, int portion, int reg, int except)
{
	int index;

	for (index = 0; index < 4; index++)
	{
		if (index != except && operand_reads(&operation->in[index], portion, reg, portion))
			return TRUE;
	}
	return FALSE;
}

/* queues an operation whose inputs may clash: the ones that do are first
computed into registers of their own */
static void emit(struct translation *translation, int portion, struct operation *operation)
{
	struct resources resources;
	int temporaries[4] = { -1, -1, -1, -1 };
	int index;

	resources_clear(&resources);
	for (index = 0; index < 4; index++)
	{
		if (resources_add(&resources, &operation->in[index], portion))
			continue;
		if (register_available(translation, portion))
		{
			operation->in[index] = materialized(translation, portion, operation->in[index]);
			if (operation->in[index].kind == _kind_register)
				temporaries[index] = operation->in[index].index;
		}
		else if (!operation_reads(operation, portion, operation->dest, index))
		{
			/* (no free register: the input goes to the operation's own
			destination first, which nothing else it reads is) */
			struct operation copy = operation_new(operation->dest);
			unsigned char negative = operation->in[index].negative;

			copy.in[3] = operation->in[index];
			copy.in[3].negative = 0;
			if (copy.in[3].invert)
			{
				copy.in[3].invert = 0;
				copy.in[0] = one;
				copy.in[2] = copy.in[3];
				copy.in[3] = zero;
			}
			queue(translation, portion, &copy);
			operation->in[index] = operand(_kind_register, operation->dest, _channel_natural);
			operation->in[index].negative = negative;
		}
		else
		{
			fail(translation, "no free register");
		}
	}
	queue(translation, portion, operation);
	for (index = 0; index < 4; index++)
		register_free(translation, portion, temporaries[index]);
}

/* the value in a register of its own (an inverted value inverted there):
the caller frees it */
static struct operand materialized(struct translation *translation, int portion, struct operand value)
{
	struct operation operation;
	struct operand copy;
	unsigned char negative;
	int reg = register_allocate(translation, portion);

	if (reg < 0)
	{
		fail(translation, "no free register");
		return zero;
	}
	operation = operation_new(reg);
	/* (a negation stays with the copy: -x is x's copy, negated) */
	negative = value.negative;
	value.negative = 0;
	if (value.invert)
	{
		/* (1 - c) * a */
		value.invert = 0;
		operation.in[0] = one;
		operation.in[2] = value;
	}
	else
	{
		operation.in[3] = value;
	}
	emit(translation, portion, &operation);
	copy = operand(_kind_register, (unsigned char)reg, _channel_natural);
	copy.negative = negative;
	return copy;
}

/* a combiner output's mapping (its flags' 0x38) as TEV's bias and scale */
static void output_mapping(struct operation *operation, unsigned flags)
{
	switch (flags & 0x38)
	{
	case 0x08: operation->bias = GX_TB_SUBHALF; break;
	case 0x10: operation->scale = GX_CS_SCALE_2; break;
	case 0x18: operation->bias = GX_TB_SUBHALF; operation->scale = GX_CS_SCALE_2; break;
	case 0x20: operation->scale = GX_CS_SCALE_4; break;
	case 0x30: operation->scale = GX_CS_DIVIDE_2; break;
	default: break;
	}
}

/* x * y + d, mapped, into dest: c * b, or (1 - c) * a for an inverted input */
static void emit_product(struct translation *translation, int portion, struct operand x, struct operand y,
	struct operand d, unsigned flags, int dest)
{
	struct operation operation = operation_new(dest);
	int temporary = -1, d_temporary = -1;

	x = folded(x);
	y = folded(y);
	/* (a negated factor: the product subtracted from d) */
	operation.subtract = (unsigned char)(x.negative ^ y.negative);
	x.negative = y.negative = 0;
	if (x.invert && y.invert)
	{
		x = materialized(translation, portion, x);
		temporary = x.index;
	}
	if (x.invert)
	{
		struct operand swap = x;

		x = y;
		y = swap;
	}
	if (d.invert)
	{
		d = materialized(translation, portion, d);
		d_temporary = d.kind == _kind_register ? d.index : -1;
	}
	if (x.kind == _kind_zero || y.kind == _kind_zero)
	{
		/* (nothing but d) */
	}
	else if (y.invert)
	{
		y.invert = 0;
		operation.in[0] = x;
		operation.in[2] = y;
	}
	else if (y.kind == _kind_one)
	{
		operation.in[0] = x;
	}
	else if (x.kind == _kind_one)
	{
		operation.in[0] = y;
	}
	else
	{
		operation.in[1] = x;
		operation.in[2] = y;
	}
	operation.in[3] = d;
	output_mapping(&operation, flags);
	emit(translation, portion, &operation);
	register_free(translation, portion, temporary);
	register_free(translation, portion, d_temporary);
}

/* ---------- dot products

dot(x, y) over red, green and blue, in all three: TEV has no dot product,
but its swap tables put one channel of a texture or rasterized colour in
all three (gx_psh_initialize), and its constant selections one channel of
a constant: three products, summed in a register. Of two constants, the dot
product is a constant. Neither can be a register (no swap reaches them): the
product is then taken as 1, and the program marked approximate. */

/* an operand's value per channel, when it is a constant */
static BOOL constant_value(const struct translation *translation, const struct operand *value, float out[3])
{
	int channel;
	float v[4];

	switch (value->kind)
	{
	case _kind_zero: v[0] = v[1] = v[2] = v[3] = 0.0f; break;
	case _kind_one: v[0] = v[1] = v[2] = v[3] = 1.0f; break;
	case _kind_half: v[0] = v[1] = v[2] = v[3] = 0.5f; break;
	case _kind_konst:
	{
		DWORD color = translation->program->konst[value->index];

		v[0] = (float)((color >> 16) & 0xFF) / 255.0f;
		v[1] = (float)((color >> 8) & 0xFF) / 255.0f;
		v[2] = (float)(color & 0xFF) / 255.0f;
		v[3] = (float)(color >> 24) / 255.0f;
		break;
	}
	default:
		return FALSE;
	}
	for (channel = 0; channel < 3; channel++)
	{
		float x = value->channel == _channel_alpha ? v[3] : v[channel];

		out[channel] = value->invert ? 1.0f - x : x;
	}
	return TRUE;
}

/* one channel of an operand in all three of the colour's */
static struct operand broadcast(struct operand value, int channel)
{
	static const unsigned char channels[3] = { _channel_red_all, _channel_green_all, _channel_blue_all };

	/* (the same in every channel already: constants, alphas) */
	if (value.kind == _kind_zero || value.kind == _kind_one || value.kind == _kind_half ||
		value.channel == _channel_alpha)
		return value;
	value.channel = channels[channel];
	return value;
}

static BOOL broadcastable(const struct operand *value)
{
	return value->kind != _kind_register || value->channel == _channel_alpha;
}

static struct operand emit_dot(struct translation *translation, struct operand x, struct operand y, int *temporary)
{
	float a[3], b[3];
	unsigned char negative = (unsigned char)(x.negative ^ y.negative);
	struct operand result;
	int reg, channel;

	*temporary = -1;
	x = folded(x);
	y = folded(y);
	x.negative = y.negative = 0;
	if (constant_value(translation, &x, a) && constant_value(translation, &y, b))
	{
		float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		DWORD level;

		if (dot > 1.0f)
			dot = 1.0f;
		level = (DWORD)(dot * 255.0f + 0.5f);
		result = operand(_kind_konst, konst_slot(translation, (level << 24) | (level << 16) | (level << 8) | level),
			_channel_natural);
		result.negative = negative;
		return result;
	}
	if (!broadcastable(&x) || !broadcastable(&y) || translation->economical)
	{
		translation->program->approximate = TRUE;
		return one;
	}
	reg = register_allocate(translation, translation->portion_now);
	if (reg < 0)
	{
		translation->program->approximate = TRUE;
		return one;
	}
	result = operand(_kind_register, (unsigned char)reg, _channel_natural);
	for (channel = 0; channel < 3; channel++)
		emit_product(translation, _portion_color, broadcast(x, channel), broadcast(y, channel), channel ? result : zero, 0,
			reg);
	*temporary = reg;
	result.negative = negative;
	return result;
}

/* ---------- combiner inputs */

/* one general combiner input, mapped: signed mappings clamped */
static struct operand combiner_input(struct translation *translation, unsigned long input, int portion, int stage,
	int *temporary)
{
	struct operand value = register_operand(translation, input & 0x0f, (input & 0x10) != 0, portion, stage);
	unsigned mapping = input & 0xe0;
	struct operation operation;
	int reg;

	*temporary = -1;
	switch (mapping)
	{
	case 0x00: case 0xc0:
		return value;
	case 0x20:
		value.invert = 1;
		return folded(value);
	case 0xe0:
		/* signed negation: -x, which a sum takes as a subtraction */
		if (value.kind == _kind_zero)
			return zero;
		value.negative = 1;
		return value;
	default:
		break;
	}
	/* the mappings computed into a register of their own, clamped to [0, 1]
	as TEV's registers are: expand 2x - 1, half bias x - 0.5, expand
	negate 1 - 2x, half bias negate 0.5 - x */
	if (value.kind == _kind_zero)
		return mapping == 0x40 || mapping == 0x80 ? zero : mapping == 0x60 ? one : operand(_kind_half, 0, 0);
	if (value.kind == _kind_one)
		return mapping == 0x40 ? one : mapping == 0x80 ? operand(_kind_half, 0, 0) : zero;
	if (value.kind == _kind_half)
		return zero;
	reg = translation->economical && (mapping == 0x60 || mapping == 0xa0) ? -1 : register_allocate(translation, portion);
	if (reg < 0 && (mapping == 0x60 || mapping == 0xa0))
	{
		/* (no register: expand negate as an inversion, half bias negate as
		0, as they were before they had one) */
		translation->program->approximate = TRUE;
		if (mapping == 0xa0)
			return zero;
		value.invert = 1;
		return folded(value);
	}
	if (reg < 0)
	{
		fail(translation, "no free register");
		return zero;
	}
	operation = operation_new(reg);
	if (mapping == 0x40 || mapping == 0x80)
	{
		/* (x - 0.5), times 2 for the expansion */
		operation.in[3] = value;
		operation.bias = GX_TB_SUBHALF;
		operation.scale = mapping == 0x40 ? GX_CS_SCALE_2 : GX_CS_SCALE_1;
	}
	else
	{
		/* (0.5 - x), times 2 for the expansion: TEV's subtraction, d - a */
		operation.in[3] = operand(_kind_half, 0, 0);
		operation.in[0] = value;
		operation.subtract = 1;
		operation.scale = mapping == 0x60 ? GX_CS_SCALE_2 : GX_CS_SCALE_1;
	}
	emit(translation, portion, &operation);
	*temporary = reg;
	return operand(_kind_register, (unsigned char)reg, _channel_natural);
}

/* ---------- liveness */

/* which of a name's channels an input byte reads in a portion: its colour
(_portion_color) or its alpha; the alpha portion's blue reads the colour */
static int input_reads(unsigned long input, int portion)
{
	(void)portion;
	return (input & 0x10) ? _portion_alpha : _portion_color;
}

/* whether a name's channel (portion) is read after the combiner stage (the
later stages and the final combiner), as far as can be told */
static BOOL name_read_later(const struct translation *translation, int after_stage, int name, int channel)
{
	const DWORD *state = translation->state;
	unsigned long count = state[D3DRS_PSCOMBINERCOUNT] & 0xff;
	DWORD abcd = state[D3DRS_PSFINALCOMBINERINPUTSABCD];
	DWORD efg = state[D3DRS_PSFINALCOMBINERINPUTSEFG];
	unsigned long stage;
	int portion, shift;

	if (count > 8)
		count = 8;
	for (stage = (unsigned long)(after_stage + 1); stage < count; stage++)
	{
		DWORD outputs = state[D3DRS_PSRGBOUTPUTS0 + stage];

		for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++)
		{
			DWORD inputs = state[(portion == _portion_alpha ? D3DRS_PSALPHAINPUTS0 : D3DRS_PSRGBINPUTS0) + stage];

			for (shift = 0; shift < 32; shift += 8)
			{
				unsigned long input = (inputs >> shift) & 0xff;

				if (name_of(input & 0x0f) == name && input_reads(input, portion) == channel)
					return TRUE;
			}
			outputs = state[(portion == _portion_alpha ? D3DRS_PSALPHAOUTPUTS0 : D3DRS_PSRGBOUTPUTS0) + stage];
			/* (the mux reads r0's alpha) */
			if (((outputs >> 12) & 0x04) && name == _name_r0 && channel == _portion_alpha)
				return TRUE;
		}
	}
	if (!abcd && !efg)
		return name == _name_r0;
	for (shift = 0; shift < 32; shift += 8)
	{
		unsigned long input = (abcd >> shift) & 0xff;

		if (name_of(input & 0x0f) == name && input_reads(input, _portion_color) == channel)
			return TRUE;
		if ((input & 0x0f) == _register_v1r0_sum && channel == _portion_color && (name == _name_r0 || name == _name_v1))
			return TRUE;
		if ((input & 0x0f) == _register_ef_product)
		{
			unsigned long e = (efg >> 24) & 0xff, f = (efg >> 16) & 0xff;

			if ((name_of(e & 0x0f) == name && input_reads(e, _portion_color) == channel) ||
				(name_of(f & 0x0f) == name && input_reads(f, _portion_color) == channel))
				return TRUE;
		}
	}
	{
		unsigned long g = (efg >> 8) & 0xff;

		if (name_of(g & 0x0f) == name && input_reads(g, _portion_alpha) == channel)
			return TRUE;
	}
	return FALSE;
}

/* after a combiner stage: the registers of names nothing reads again are free */
static void names_retire(struct translation *translation, int stage)
{
	int portion, name;

	for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++)
	{
		for (name = 0; name < NUMBER_OF_NAMES; name++)
		{
			struct operand *location = &translation->names[portion][name];

			if (location->kind == _kind_register && !name_read_later(translation, stage, name, portion))
			{
				register_free(translation, portion, location->index);
				*location = zero;
			}
		}
	}
}

/* a product that is one of its operands as it is (or 0): no stage */
static BOOL product_operand(struct operand x, struct operand y, struct operand *result)
{
	x = folded(x);
	y = folded(y);
	if (x.kind == _kind_zero || y.kind == _kind_zero)
	{
		*result = zero;
		return TRUE;
	}
	if (x.negative || y.negative)
		return FALSE;
	if (x.kind == _kind_one && !y.invert)
	{
		*result = y;
		return TRUE;
	}
	if (y.kind == _kind_one && !x.invert)
	{
		*result = x;
		return TRUE;
	}
	return FALSE;
}

/* whether an operand, read in a portion, reads a register of a portion's
pool (the colour half reads the alpha pool through an alpha channel) */
/* A*B + C*D as x/2 + y/2: one of each pair a half, the others plain (not
inverted or negative) */
static BOOL half_pair(const struct operand *inputs, struct operand *x, struct operand *y)
{
	int first, second;

	for (first = 0; first < 2; first++)
	{
		for (second = 2; second < 4; second++)
		{
			const struct operand *a = &inputs[first], *c = &inputs[second];
			const struct operand *b = &inputs[1 - first], *d = &inputs[5 - second];

			if (a->kind == _kind_half && !a->invert && !a->negative && c->kind == _kind_half && !c->invert &&
				!c->negative && !b->invert && !b->negative && !d->invert && !d->negative)
			{
				*x = *b;
				*y = *d;
				return TRUE;
			}
		}
	}
	return FALSE;
}

static BOOL operand_reads(const struct operand *value, int reading_portion, int reg, int pool)
{
	int read_pool = reading_portion == _portion_color && value->channel != _channel_alpha ? _portion_color :
		_portion_alpha;

	return value->kind == _kind_register && value->index == reg && read_pool == pool;
}

/* the register a combiner output's name goes to: where it was, when nothing
after reads that (later: the operands still to be used in this portion,
and for the alpha the colour portion's reads of alpha), else a free one */
static BOOL aliasable(const struct operand *value, int portion)
{
	return value->kind != _kind_register || portion == _portion_alpha || value->channel == _channel_natural;
}

/* a name now standing for an operand (a register's shared) */
static void name_alias(struct translation *translation, int portion, unsigned long reg, struct operand value)
{
	struct operand *location = &translation->names[portion][name_of(reg)];

	if (location->kind == _kind_register)
		translation->released[portion][location->index]++;
	if (value.kind == _kind_register)
		translation->holders[portion][value.index]++;
	*location = value;
}

static BOOL register_available(const struct translation *translation, int portion)
{
	int reg;

	for (reg = 0; reg < 4; reg++)
	{
		if (!translation->holders[portion][reg] && !translation->released[portion][reg])
			return TRUE;
	}
	return FALSE;
}

/* whether a name can be written over its own register */
static BOOL in_place(const struct translation *translation, int portion, unsigned long reg,
	const struct operand *later, int later_count)
{
	int name = name_of(reg);
	const struct operand *location;
	int index;

	if (name < 0)
		return FALSE;
	location = &translation->names[portion][name];
	if (location->kind != _kind_register || translation->holders[portion][location->index] != 1)
		return FALSE;
	for (index = 0; index < later_count; index++)
	{
		if (operand_reads(&later[index], portion, location->index, portion))
			return FALSE;
	}
	if (portion == _portion_alpha)
	{
		for (index = 0; index < 4; index++)
		{
			if (operand_reads(&translation->color_operands[index], _portion_color, location->index, portion))
				return FALSE;
		}
	}
	return TRUE;
}

static int output_register(struct translation *translation, int portion, unsigned long reg,
	const struct operand *later, int later_count)
{
	int name = name_of(reg);
	int dest;
	struct operand *location;

	if (name < 0)
		return -1;
	location = &translation->names[portion][name];
	if (in_place(translation, portion, reg, later, later_count))
		return location->index;
	dest = register_allocate(translation, portion);
	if (dest < 0)
	{
		/* (none free: the register of a name read no more after this
		stage, when nothing still to come here reads it) */
		int other;

		for (other = 0; other < NUMBER_OF_NAMES && dest < 0; other++)
		{
			struct operand *victim = &translation->names[portion][other];

			if (other != name && victim->kind == _kind_register &&
				!name_read_later(translation, translation->stage, other, portion) &&
				in_place(translation, portion, (unsigned long)register_of_name(other), later, later_count))
			{
				dest = victim->index;
				*victim = zero;
				translation->holders[portion][dest]--;
				translation->holders[portion][dest]++;
				if (location->kind == _kind_register)
					translation->released[portion][location->index]++;
				*location = operand(_kind_register, (unsigned char)dest, _channel_natural);
				return dest;
			}
		}
		fail(translation, "no free register");
		return -1;
	}
	if (location->kind == _kind_register)
		translation->released[portion][location->index]++;
	*location = operand(_kind_register, (unsigned char)dest, _channel_natural);
	return dest;
}

static void combiner_portion(struct translation *translation, int stage, int portion)
{
	const DWORD *state = translation->state;
	DWORD inputs = state[(portion == _portion_alpha ? D3DRS_PSALPHAINPUTS0 : D3DRS_PSRGBINPUTS0) + stage];
	DWORD outputs = state[(portion == _portion_alpha ? D3DRS_PSALPHAOUTPUTS0 : D3DRS_PSRGBOUTPUTS0) + stage];
	unsigned flags = (unsigned)(outputs >> 12);
	struct operand inputs_mapped[4], later[4], sum_saved[4];
	int later_count;
	int dot_temporaries[2] = { -1, -1 };
	int temporaries[4];
	unsigned long ab_name = (outputs >> 4) & 0x0f, cd_name = outputs & 0x0f, sum_name = (outputs >> 8) & 0x0f;
	int dest, index;

	/* (writes nothing reads again are left out) */
	if (name_of(ab_name) >= 0 && !name_read_later(translation, stage, name_of(ab_name), portion))
		ab_name = 0;
	if (name_of(cd_name) >= 0 && !name_read_later(translation, stage, name_of(cd_name), portion))
		cd_name = 0;
	if (name_of(sum_name) >= 0 && !name_read_later(translation, stage, name_of(sum_name), portion))
		sum_name = 0;
	if (name_of(ab_name) < 0 && name_of(cd_name) < 0 && name_of(sum_name) < 0)
		return;
	for (index = 0; index < 4; index++)
	{
		unsigned long input = (inputs >> (24 - 8 * index)) & 0xff;
		int earlier;

		/* (an input given twice is mapped once) */
		for (earlier = 0; earlier < index; earlier++)
		{
			if (((inputs >> (24 - 8 * earlier)) & 0xff) == input)
				break;
		}
		if (earlier < index)
		{
			inputs_mapped[index] = inputs_mapped[earlier];
			temporaries[index] = -1;
		}
		else
		{
			inputs_mapped[index] = combiner_input(translation, input, portion, stage, &temporaries[index]);
		}
	}
	/* dot products: into a register of their own (emit_dot), then a
product with 1 */
	if (portion == _portion_color && (flags & 0x02))
	{
		inputs_mapped[0] = emit_dot(translation, inputs_mapped[0], inputs_mapped[1], &dot_temporaries[0]);
		inputs_mapped[1] = one;
	}
	if (portion == _portion_color && (flags & 0x01))
	{
		inputs_mapped[2] = emit_dot(translation, inputs_mapped[2], inputs_mapped[3], &dot_temporaries[1]);
		inputs_mapped[3] = one;
	}
	if (portion == _portion_color)
		memcpy(translation->color_operands, inputs_mapped, sizeof(inputs_mapped));
	/* what the writes after the sum still read: A and B for A*B's own
	output, C and D for C*D's */
	later_count = 0;
	if (name_of(ab_name) >= 0)
	{
		later[later_count++] = inputs_mapped[0];
		later[later_count++] = inputs_mapped[1];
	}
	if (name_of(cd_name) >= 0)
	{
		later[later_count++] = inputs_mapped[2];
		later[later_count++] = inputs_mapped[3];
	}

	if (name_of(sum_name) >= 0)
	{
		struct operand ab_value, cd_value;
		BOOL ab_folds = product_operand(inputs_mapped[0], inputs_mapped[1], &ab_value);
		BOOL cd_folds = product_operand(inputs_mapped[2], inputs_mapped[3], &cd_value);
		BOOL same = !memcmp(&inputs_mapped[0], &inputs_mapped[2], 2 * sizeof(inputs_mapped[0]));

		if ((flags & 0x04) && same)
		{
			/* (a choice of one product and itself: the product) */
			dest = output_register(translation, portion, sum_name, later, later_count);
			if (dest >= 0)
				emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero, flags, dest);
			sum_name = 0;
		}
		else if (!(flags & 0x38) && !(flags & 0x04) && ab_folds && cd_folds &&
			(ab_value.kind == _kind_zero || cd_value.kind == _kind_zero) &&
			aliasable(ab_value.kind == _kind_zero ? &cd_value : &ab_value, portion))
		{
			/* (a value and nothing: the value) */
			name_alias(translation, portion, sum_name, ab_value.kind == _kind_zero ? cd_value : ab_value);
			sum_name = 0;
		}
	}
	/* (the sum may order its products: the inputs as they were after it) */
	memcpy(sum_saved, inputs_mapped, sizeof(sum_saved));
	if (name_of(sum_name) >= 0)
	{
		if (flags & 0x04)
		{
			/* the mux: r0's alpha chooses, as a blend of the two (each product
			in a register of its own, unless it is a value as it is; a
			negative product clamps to 0 there) */
			struct operand ab, cd;
			int ab_register = -1, cd_register = -1;
			struct operand chooser = translation->names[_portion_alpha][_name_r0];
			struct operation operation;

			for (index = 0; index < 4; index++)
			{
				if (inputs_mapped[index].negative)
					translation->program->approximate = TRUE;
			}

			if (!product_operand(inputs_mapped[0], inputs_mapped[1], &ab))
			{
				ab_register = register_allocate(translation, portion);
				if (ab_register < 0)
				{
					fail(translation, "no free register");
					return;
				}
				emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero, 0, ab_register);
				ab = operand(_kind_register, (unsigned char)ab_register, _channel_natural);
			}
			if (!product_operand(inputs_mapped[2], inputs_mapped[3], &cd))
			{
				cd_register = register_allocate(translation, portion);
				if (cd_register < 0)
				{
					fail(translation, "no free register");
					return;
				}
				emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3], zero, 0, cd_register);
				cd = operand(_kind_register, (unsigned char)cd_register, _channel_natural);
			}
			if (portion == _portion_color && chooser.kind >= _kind_register)
				chooser.channel = _channel_alpha;
			/* (the inputs' own registers are done with, if nothing after
			reads them) */
			if (!later_count)
			{
				for (index = 0; index < 4; index++)
				{
					register_free(translation, portion, temporaries[index]);
					temporaries[index] = -1;
				}
			}
			dest = output_register(translation, portion, sum_name, later, later_count);
			if (dest < 0)
				return;
			operation = operation_new(dest);
			operation.in[0] = ab;
			operation.in[1] = cd;
			operation.in[2] = chooser;
			output_mapping(&operation, flags);
			emit(translation, portion, &operation);
			register_free(translation, portion, ab_register);
			register_free(translation, portion, cd_register);
		}
		else
		{
			struct operand ab;
			int ab_register = -1;
			unsigned mapping = flags & 0x38;
			BOOL ab_negative = (inputs_mapped[0].negative ^ inputs_mapped[1].negative) &&
				inputs_mapped[0].kind != _kind_zero && inputs_mapped[1].kind != _kind_zero;
			BOOL cd_negative = (inputs_mapped[2].negative ^ inputs_mapped[3].negative) &&
				inputs_mapped[2].kind != _kind_zero && inputs_mapped[3].kind != _kind_zero;

			/* (a negative product is the one subtracted: second; both
			negative, the sum is below 0, which clamps to 0) */
			if (ab_negative && cd_negative)
			{
				inputs_mapped[0] = inputs_mapped[2] = zero;
			}
			else if (ab_negative)
			{
				struct operand swap[2];

				memcpy(swap, inputs_mapped, sizeof(swap));
				memcpy(inputs_mapped, inputs_mapped + 2, sizeof(swap));
				memcpy(inputs_mapped + 2, swap, sizeof(swap));
			}

			struct operand half_x, half_y;

			if (half_pair(inputs_mapped, &half_x, &half_y))
			{
				/* (x/2 + y/2: TEV's blend of the two, by a half) */
				struct operation operation;

				dest = output_register(translation, portion, sum_name, later, later_count);
				if (dest >= 0)
				{
					operation = operation_new(dest);
					operation.in[0] = half_x;
					operation.in[1] = half_y;
					operation.in[2] = operand(_kind_half, 0, 0);
					output_mapping(&operation, flags);
					emit(translation, portion, &operation);
				}
			}
			else if (!memcmp(&inputs_mapped[0], &inputs_mapped[2], 2 * sizeof(inputs_mapped[0])) &&
				(mapping == 0 || mapping == 0x10 || mapping == 0x30))
			{
				/* (A*B + A*B: A*B twice) */
				dest = output_register(translation, portion, sum_name, later, later_count);
				if (dest >= 0)
				{
					emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero,
						(flags & ~0x38u) | (mapping == 0 ? 0x10u : mapping == 0x10 ? 0x20u : 0x00u), dest);
				}
			}
			else if (product_operand(inputs_mapped[0], inputs_mapped[1], &ab))
			{
				/* (A*B is a value as it is: the sum's d) */
				dest = output_register(translation, portion, sum_name, later, later_count);
				if (dest >= 0)
					emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3], ab, flags, dest);
			}
			else if ((ab_register = register_allocate(translation, portion)) >= 0)
			{
				emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero, 0, ab_register);
				dest = output_register(translation, portion, sum_name, later, later_count);
				if (dest >= 0)
				{
					emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3],
						operand(_kind_register, (unsigned char)ab_register, _channel_natural), flags, dest);
				}
				register_free(translation, portion, ab_register);
			}
			else
			{
				/* (no register for A*B: it goes to the sum's own, which C and
				D must not read) */
				struct operand sum_later[2];

				sum_later[0] = inputs_mapped[2];
				sum_later[1] = inputs_mapped[3];
				dest = output_register(translation, portion, sum_name, later, later_count);
				if (dest < 0)
					return;
				if (operand_reads(&sum_later[0], portion, dest, portion) ||
					operand_reads(&sum_later[1], portion, dest, portion))
				{
					fail(translation, "no free register");
					return;
				}
				emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero, 0, dest);
				emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3],
					operand(_kind_register, (unsigned char)dest, _channel_natural), flags, dest);
			}
		}
	}
	memcpy(inputs_mapped, sum_saved, sizeof(sum_saved));
	/* a product that is an operand as it is, unmapped, is that operand: the
	name stands for it, no stage made (not a register of the other pool) */
	if (!(flags & 0x38))
	{
		struct operand value;

		if (name_of(ab_name) >= 0 && product_operand(inputs_mapped[0], inputs_mapped[1], &value) &&
			aliasable(&value, portion))
		{
			name_alias(translation, portion, ab_name, value);
			ab_name = 0;
		}
		if (name_of(cd_name) >= 0 && product_operand(inputs_mapped[2], inputs_mapped[3], &value) &&
			aliasable(&value, portion))
		{
			name_alias(translation, portion, cd_name, value);
			cd_name = 0;
		}
	}
	/* A*B, then C*D, or C*D first when A*B has no register to go to and
	C*D can go over its name's own (A and B not reading it) */
	{
		BOOL cd_first = FALSE;

		if (name_of(ab_name) >= 0 && name_of(cd_name) >= 0 && !register_available(translation, portion) &&
			!in_place(translation, portion, ab_name, &inputs_mapped[2], 2))
		{
			cd_first = in_place(translation, portion, cd_name, inputs_mapped, 2);
		}
		if (cd_first)
		{
			dest = output_register(translation, portion, cd_name, inputs_mapped, 2);
			if (dest >= 0)
				emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3], zero, flags, dest);
			cd_name = 0;
		}
		if (name_of(ab_name) >= 0)
		{
			/* (C and D are still to be read) */
			dest = output_register(translation, portion, ab_name, &inputs_mapped[2], name_of(cd_name) >= 0 ? 2 : 0);
			if (dest >= 0)
				emit_product(translation, portion, inputs_mapped[0], inputs_mapped[1], zero, flags, dest);
		}
		if (name_of(cd_name) >= 0)
		{
			dest = output_register(translation, portion, cd_name, inputs_mapped, 0);
			if (dest >= 0)
				emit_product(translation, portion, inputs_mapped[2], inputs_mapped[3], zero, flags, dest);
		}
	}
	for (index = 0; index < 4; index++)
		register_free(translation, portion, temporaries[index]);
	register_free(translation, portion, dot_temporaries[0]);
	register_free(translation, portion, dot_temporaries[1]);
}

/* ---------- stages */

static unsigned char input_code(const struct operand *value, int portion)
{
	static const unsigned char color_registers[4] = { GX_CC_CPREV, GX_CC_C0, GX_CC_C1, GX_CC_C2 };
	static const unsigned char alpha_registers[4] = { GX_CC_APREV, GX_CC_A0, GX_CC_A1, GX_CC_A2 };
	static const unsigned char alpha_half_registers[4] = { GX_CA_APREV, GX_CA_A0, GX_CA_A1, GX_CA_A2 };

	if (portion == _portion_color)
	{
		switch (value->kind)
		{
		case _kind_one: return GX_CC_ONE;
		case _kind_half: return GX_CC_HALF;
		case _kind_register:
			return value->channel == _channel_alpha ? alpha_registers[value->index & 3] : color_registers[value->index & 3];
		/* (the blue everywhere: the alpha, which table 3 makes the blue) */
		case _kind_texture:
			return value->channel == _channel_alpha || value->channel == _channel_blue_all ? GX_CC_TEXA : GX_CC_TEXC;
		case _kind_ras:
			return value->channel == _channel_alpha || value->channel == _channel_blue_all ? GX_CC_RASA : GX_CC_RASC;
		case _kind_konst: return GX_CC_KONST;
		default: return GX_CC_ZERO;
		}
	}
	switch (value->kind)
	{
	case _kind_one: case _kind_half: case _kind_konst: return GX_CA_KONST;
	case _kind_register: return alpha_half_registers[value->index & 3];
	case _kind_texture: return GX_CA_TEXA;
	case _kind_ras: return GX_CA_RASA;
	default: return GX_CA_ZERO;
	}
}

static void operation_resources(const struct operation *operation, int portion, struct resources *resources)
{
	int index;

	resources_clear(resources);
	for (index = 0; index < 4; index++)
		resources_add(resources, &operation->in[index], portion);
}

static void stage_half(struct gx_psh_stage *stage, const struct operation *operation, int portion)
{
	unsigned char *in = portion == _portion_color ? stage->color_in : stage->alpha_in;
	int index;

	for (index = 0; index < 4; index++)
		in[index] = input_code(&operation->in[index], portion);
	if (portion == _portion_color)
	{
		stage->color_op = operation->subtract ? GX_TEV_SUB : GX_TEV_ADD;
		stage->color_bias = operation->bias;
		stage->color_scale = operation->scale;
		stage->color_dest = operation->dest;
	}
	else
	{
		stage->alpha_op = operation->subtract ? GX_TEV_SUB : GX_TEV_ADD;
		stage->alpha_bias = operation->bias;
		stage->alpha_scale = operation->scale;
		stage->alpha_dest = operation->dest;
	}
}

/* the first of the swap tables a set allows */
static unsigned char swap_table(unsigned char tables)
{
	unsigned char table;

	for (table = 0; table < 4; table++)
	{
		if (tables & (1u << table))
			return (unsigned char)(GX_TEV_SWAP0 + table);
	}
	return GX_TEV_SWAP0;
}

static BOOL stage_append(struct translation *translation, const struct operation *color,
	const struct operation *alpha)
{
	struct gx_psh_program *program = translation->program;
	struct gx_psh_stage *stage;
	struct resources resources, other;
	struct operation nop;

	if (program->stage_count >= GX_PSH_MAXIMUM_STAGES)
	{
		fail(translation, "more than 16 stages");
		return FALSE;
	}
	stage = &program->stages[program->stage_count++];
	memset(stage, 0, sizeof(*stage));
	resources_clear(&resources);
	if (color)
		operation_resources(color, _portion_color, &resources);
	stage->konst_color = resources.konst == NO_RESOURCE ? GX_TEV_KCSEL_1 : resources.konst;
	if (alpha)
	{
		operation_resources(alpha, _portion_alpha, &other);
		resources_merge(&resources, &other);
		stage->konst_alpha = other.konst == NO_RESOURCE ? GX_TEV_KASEL_1 : other.konst;
	}
	else
	{
		stage->konst_alpha = GX_TEV_KASEL_1;
	}
	stage->texture_map = resources.texture_map;
	stage->texture_swap = swap_table(resources.texture_swap);
	stage->ras_channel = resources.ras;
	stage->ras_swap = swap_table(resources.ras_swap);
	/* (a half with nothing to do keeps PREV as it is) */
	nop = operation_new(0);
	if (color)
		stage_half(stage, color, _portion_color);
	else
	{
		nop.in[3] = operand(_kind_register, 0, _channel_natural);
		stage_half(stage, &nop, _portion_color);
	}
	if (alpha)
		stage_half(stage, alpha, _portion_alpha);
	else
	{
		nop.in[3] = operand(_kind_register, 0, _channel_natural);
		stage_half(stage, &nop, _portion_alpha);
	}
	return TRUE;
}

/* this combiner stage's operations into TEV stages: the colour's and the
alpha's side by side where their textures and colours agree, the shorter
list's at the end of the longer's (so the alpha's last writes, PREV's in the
final combiner, land no earlier than the colour's last reads of them); where
the two cannot share a stage, the colour's goes first */
static void queues_flush(struct translation *translation)
{
	unsigned color = 0, alpha = 0;
	int portion, reg;

	while (color < translation->queue_count[_portion_color] || alpha < translation->queue_count[_portion_alpha])
	{
		unsigned color_left = translation->queue_count[_portion_color] - color;
		unsigned alpha_left = translation->queue_count[_portion_alpha] - alpha;
		const struct operation *color_operation =
			color_left >= alpha_left && color_left ? &translation->queue[_portion_color][color] : NULL;
		const struct operation *alpha_operation =
			alpha_left >= color_left && alpha_left ? &translation->queue[_portion_alpha][alpha] : NULL;

		if (color_operation && alpha_operation)
		{
			struct resources color_resources, alpha_resources;

			operation_resources(color_operation, _portion_color, &color_resources);
			operation_resources(alpha_operation, _portion_alpha, &alpha_resources);
			if (!resources_merge(&color_resources, &alpha_resources))
				alpha_operation = NULL;
		}
		if (!stage_append(translation, color_operation, alpha_operation))
			return;
		if (color_operation)
			color++;
		if (alpha_operation)
			alpha++;
	}
	for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++)
	{
		translation->queue_count[portion] = 0;
		for (reg = 0; reg < 4; reg++)
		{
			while (translation->released[portion][reg])
			{
				translation->released[portion][reg]--;
				register_free(translation, portion, reg);
			}
		}
	}
}

/* ---------- the final combiner */

static struct operand final_input(struct translation *translation, unsigned long input, int portion)
{
	struct operand value = register_operand(translation, input & 0x0f, (input & 0x10) != 0, portion, -1);

	if (input & 0x20)
		value.invert = 1;
	return folded(value);
}

static void final_combiner(struct translation *translation)
{
	const DWORD *state = translation->state;
	DWORD abcd = state[D3DRS_PSFINALCOMBINERINPUTSABCD];
	DWORD efg = state[D3DRS_PSFINALCOMBINERINPUTSEFG];
	unsigned long settings = efg & 0xff;
	struct operand a, b, c, d, g;
	struct operation color, alpha;
	unsigned long input;
	int shift;

	if (!abcd && !efg)
	{
		/* the result is r0 */
		color = operation_new(0);
		color.in[3] = translation->names[_portion_color][_name_r0];
		emit(translation, _portion_color, &color);
		alpha = operation_new(0);
		alpha.in[3] = translation->names[_portion_alpha][_name_r0];
		emit(translation, _portion_alpha, &alpha);
		queues_flush(translation);
		return;
	}
	/* (the sum's and the product's alpha is 0: the zero register's) */
	for (shift = 0; shift < 32; shift += 8)
	{
		input = (abcd >> shift) & 0xff;
		if (((input & 0x0f) == _register_v1r0_sum || (input & 0x0f) == _register_ef_product) && (input & 0x10))
			abcd = (abcd & ~(0xffUL << shift)) | ((input & 0x20UL) << shift);
	}
	/* the sum and the product the inputs name, first, where they are named */
	for (shift = 0; shift < 32; shift += 8)
	{
		input = (abcd >> shift) & 0x0f;
		if ((abcd >> shift) & 0x80)
			continue;
		if (input == _register_ef_product)
		{
			struct operand e = final_input(translation, (efg >> 24) & 0xff, _portion_color);
			struct operand f = final_input(translation, (efg >> 16) & 0xff, _portion_color);
			int reg = register_allocate(translation, _portion_color);

			if (reg < 0)
			{
				fail(translation, "no free register");
				return;
			}
			emit_product(translation, _portion_color, e, f, zero, 0, reg);
			/* (marked 0x80: the input is in reg now) */
			abcd = (abcd & ~(0xffUL << shift)) | ((unsigned long)(0x80 | reg) << shift) |
				(((abcd >> shift) & 0x30UL) << shift);
		}
		else if (input == _register_v1r0_sum)
		{
			struct operand v1 = register_operand(translation, _register_v1, FALSE, _portion_color, -1);
			struct operand r0 = register_operand(translation, _register_r0, FALSE, _portion_color, -1);
			int reg = register_allocate(translation, _portion_color);

			if (reg < 0)
			{
				fail(translation, "no free register");
				return;
			}
			v1.invert = (settings & 0x40) != 0;
			r0.invert = (settings & 0x20) != 0;
			emit_product(translation, _portion_color, v1, one, r0, 0, reg);
			abcd = (abcd & ~(0xffUL << shift)) | ((unsigned long)(0x80 | reg) << shift) |
				(((abcd >> shift) & 0x30UL) << shift);
		}
	}

#define FINAL_OPERAND(value, byte) \
	do { \
		unsigned long final_byte = (byte); \
		if (final_byte & 0x80) \
		{ \
			(value) = operand(_kind_register, (unsigned char)(final_byte & 3), \
				(final_byte & 0x10) ? _channel_alpha : _channel_natural); \
			(value).invert = (final_byte & 0x20) ? 1 : 0; \
		} \
		else \
			(value) = final_input(translation, final_byte, _portion_color); \
	} while (0)

	FINAL_OPERAND(a, (abcd >> 24) & 0xff);
	FINAL_OPERAND(b, (abcd >> 16) & 0xff);
	FINAL_OPERAND(c, (abcd >> 8) & 0xff);
	FINAL_OPERAND(d, abcd & 0xff);
#undef FINAL_OPERAND

	/* A*B + (1-A)*C + D: TEV's lerp, d + (1-c)*a + c*b */
	color = operation_new(0);
	if (a.invert)
	{
		struct operand swap = b;

		b = c;
		c = swap;
		a.invert = 0;
	}
	if (b.invert)
		b = materialized(translation, _portion_color, b);
	if (c.invert)
		c = materialized(translation, _portion_color, c);
	if (d.invert)
		d = materialized(translation, _portion_color, d);
	color.in[0] = c;
	color.in[1] = b;
	color.in[2] = a;
	color.in[3] = d;
	emit(translation, _portion_color, &color);

	g = final_input(translation, (efg >> 8) & 0xff, _portion_alpha);
	alpha = operation_new(0);
	if (g.invert)
	{
		g.invert = 0;
		alpha.in[0] = one;
		alpha.in[2] = g;
	}
	else
	{
		alpha.in[3] = g;
	}
	emit(translation, _portion_alpha, &alpha);
	queues_flush(translation);
}

/* ---------- public code */

static BOOL translate_once(const DWORD *render_state, const struct gx_psh_textures *textures,
	struct gx_psh_program *program, BOOL economical)
{
	static struct translation translation;
	unsigned long combiner_count = render_state[D3DRS_PSCOMBINERCOUNT] & 0xff;
	unsigned long stage;
	int portion;

	memset(&translation, 0, sizeof(translation));
	memset(program, 0, sizeof(*program));
	translation.state = render_state;
	translation.textures = textures;
	translation.program = program;
	translation.economical = economical;
	names_initialize(&translation);
	if (combiner_count > 8)
		combiner_count = 8;
	for (stage = 0; stage < combiner_count && !translation.failed; stage++)
	{
		memset(translation.color_operands, 0, sizeof(translation.color_operands));
		translation.stage = (int)stage;
		for (portion = 0; portion < NUMBER_OF_PORTIONS; portion++)
		{
			translation.portion_now = portion;
			combiner_portion(&translation, (int)stage, portion);
		}
		queues_flush(&translation);
		names_retire(&translation, (int)stage);
	}
	if (!translation.failed)
		final_combiner(&translation);
	program->reason = translation.failed ? translation.reason : NULL;
	return !translation.failed && program->stage_count > 0;
}

BOOL gx_psh_translate(const DWORD *render_state, const struct gx_psh_textures *textures,
	struct gx_psh_program *program)
{
	return translate_once(render_state, textures, program, FALSE) ||
		translate_once(render_state, textures, program, TRUE);
}

void gx_psh_apply(const struct gx_psh_program *program)
{
	unsigned index;

	for (index = 0; index < program->konst_count; index++)
	{
		DWORD value = program->konst[index];
		GXColor color = { (u8)(value >> 16), (u8)(value >> 8), (u8)value, (u8)(value >> 24) };

		GX_SetTevKColor((u8)(GX_KCOLOR0 + index), color);
	}
	GX_SetNumTevStages((u8)program->stage_count);
	for (index = 0; index < program->stage_count; index++)
	{
		const struct gx_psh_stage *stage = &program->stages[index];
		u8 tev = (u8)(GX_TEVSTAGE0 + index);

		GX_SetTevOrder(tev,
			stage->texture_map == NO_RESOURCE ? GX_TEXCOORDNULL :
				stage->texture_map == GX_PSH_FOG_MAP ? GX_PSH_FOG_TEXCOORD : (u8)(GX_TEXCOORD0 + (stage->texture_map & 3)),
			stage->texture_map == NO_RESOURCE ? GX_TEXMAP_NULL :
				stage->texture_map == GX_PSH_FOG_MAP ? GX_PSH_FOG_TEXMAP : (u8)(GX_TEXMAP0 + stage->texture_map),
			stage->ras_channel == NO_RESOURCE ? GX_COLORNULL : stage->ras_channel ? GX_COLOR1A1 : GX_COLOR0A0);
		GX_SetTevSwapMode(tev, stage->ras_swap, stage->texture_swap);
		GX_SetTevKColorSel(tev, stage->konst_color);
		GX_SetTevKAlphaSel(tev, stage->konst_alpha);
		GX_SetTevColorIn(tev, stage->color_in[0], stage->color_in[1], stage->color_in[2], stage->color_in[3]);
		GX_SetTevColorOp(tev, stage->color_op, stage->color_bias, stage->color_scale, GX_TRUE, stage->color_dest);
		GX_SetTevAlphaIn(tev, stage->alpha_in[0], stage->alpha_in[1], stage->alpha_in[2], stage->alpha_in[3]);
		GX_SetTevAlphaOp(tev, stage->alpha_op, stage->alpha_bias, stage->alpha_scale, GX_TRUE, stage->alpha_dest);
	}
}

void gx_psh_initialize(void)
{
	GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
	GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_RED);
	GX_SetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN);
	GX_SetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_BLUE);
}
