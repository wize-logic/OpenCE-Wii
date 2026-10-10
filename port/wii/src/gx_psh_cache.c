/*
GX_PSH_CACHE.C

The translated pixel shaders, kept.

A translation (gx_psh_translate) is long: it tries the combiners several
ways, and a program beyond GX is tried every way before it is refused. The
game sets many of its programs' constants for each object (a model's fog,
from its distance, and its change colours), so a cache keyed on the whole
of the render states translates such a program again for nearly every draw.

The translation reads the constants only through what they look like to it:
which channels are 0 or 1, which channels of one are equal, which constants
are equal (konst registers are shared), and the products of constants it
folds into one. The first three make each constant's signature; programs
whose combiners, textures and signatures agree are one entry, the
structure's. When a structure is first translated, it is translated again
twice with other constants of the same signatures, chosen at random; if the
program comes out the same each time, only the konst and preloaded register
values that hold a constant changing with it, the structure is generic: for
other constants of its signatures, those values are rewritten in place
rather than the program translated. The first change of each generic
program, and every AUDIT_INTERVAL-th after, is translated anyway and
compared (the counts say how often they differed: a structure that differs
is not generic from then on). A structure that is not generic is kept for
each set of its constants, as before.

Every constant counts, whether or not a combiner names it (the specular
recipe reads some no combiner input does): one the game left from another
shader changes the program's constants, not its structure, while its
signature holds.

What the cache keeps: the combiner render states (those of the stages beyond
the combiner count left out), the textures as the translation is given them,
and the constants' signatures; for each entry, its constants and the
program.
*/

#include "gx_psh_cache.h"

#include <stdlib.h>
#include <string.h>

/* ---------- constants */

enum
{
	/* the constants a program can read: c0 and c1 of each combiner stage,
	the final combiner's two, and the fog's colour */
	_slot_c0 = 0,
	_slot_c1 = 8,
	_slot_final_c0 = 16,
	_slot_final_c1 = 17,
	_slot_fog = 18,
	NUMBER_OF_SLOTS = 19,

	/* (in the heap: MEM1's is full, the rest is MEM2's) */
	CACHE_SETS = 64,
	CACHE_WAYS = 4,
	/* the structures pushed out of the cache, remembered by their hash to
	count those translated again */
	EVICTED_HASHES = 256,
	/* the translations with other constants that make a structure generic */
	GENERIC_TRIALS = 2,
	/* a generic program's changes translated again to check them: the first,
	then one in this many */
	AUDIT_INTERVAL = 32,
	/* the render states the translation may read: those of the pixel shader
	(D3DRS_PS_MAX) and the texture modes */
	TRIAL_STATE_WORDS = D3DRS_PSTEXTUREMODES + 1,

	_register_v0 = 4,
	_register_v1 = 5,
	_register_t0 = 8,
	_register_t3 = 11,
};

/* ---------- definitions */

struct structure
{
	DWORD state[D3DRS_PS_MAX];
	DWORD texture_modes;
	struct gx_psh_textures textures;
	/* each constant's signature (constant_signature) */
	uint32_t signatures[NUMBER_OF_SLOTS];
};

struct cache_entry
{
	struct structure key;
	/* the constants the program holds */
	DWORD constants[NUMBER_OF_SLOTS];
	uint32_t hash;
	/* when it was last used; 0: empty */
	unsigned long stamp;
	/* the structure's own entry (in the set of its structure's hash), else
	one for these constants (in the set of their hash too) */
	unsigned char structural;
	unsigned char generic;
	/* the constant each konst colour and preloaded register holds (its
	slot), -1 for a value of the translation's own */
	signed char konst_slots[WL_TEV_KONST_SLOTS];
	signed char preload_slots[WL_TEV_REG_COUNT];
	unsigned long changes;
	BOOL usable;
	struct gx_psh_program program;
};

/* ---------- globals */

static struct cache_entry (*cache)[CACHE_WAYS];
static uint32_t evicted_hashes[EVICTED_HASHES];
static unsigned evicted_next;
static unsigned long cache_stamp;
static struct gx_psh_cache_counts counts;
static uint32_t random_state = 0x2545F491u;

/* the last call's render states and textures, and its entry (unchanged
since, as nothing else has been asked) */
static DWORD last_state[D3DRS_PS_MAX], last_texture_modes;
static struct gx_psh_textures last_textures;
static struct cache_entry *last_entry;

/* ---------- private code */

/* Direct3D's ARGB as the TEV program's RGBA (as gx_psh.c's) */
static uint32_t rgba(DWORD color)
{
	return ((uint32_t)color << 8) | ((uint32_t)color >> 24);
}

/* a constant as the translation reads it (the fog's colour opaque) */
static DWORD slot_value(const DWORD *state, const struct gx_psh_textures *textures, int slot)
{
	if (slot < _slot_c1)
		return state[D3DRS_PSCONSTANT0_0 + slot];
	if (slot < _slot_final_c0)
		return state[D3DRS_PSCONSTANT1_0 + slot - _slot_c1];
	if (slot == _slot_final_c0)
		return state[D3DRS_PSFINALCOMBINERCONSTANT0];
	if (slot == _slot_final_c1)
		return state[D3DRS_PSFINALCOMBINERCONSTANT1];
	return textures->fog_color | 0xFF000000UL;
}

static void slot_set(DWORD *state, struct gx_psh_textures *textures, int slot, DWORD value)
{
	if (slot < _slot_c1)
		state[D3DRS_PSCONSTANT0_0 + slot] = value;
	else if (slot < _slot_final_c0)
		state[D3DRS_PSCONSTANT1_0 + slot - _slot_c1] = value;
	else if (slot == _slot_final_c0)
		state[D3DRS_PSFINALCOMBINERCONSTANT0] = value;
	else if (slot == _slot_final_c1)
		state[D3DRS_PSFINALCOMBINERCONSTANT1] = value;
	else
		textures->fog_color = value & 0x00FFFFFFUL;
}

/* a constant's channels (alpha, red, green, blue): each 0, 255 or another
value (two bits each), and which of them are equal (bits 8 to 13); bit 31
set */
static uint32_t constant_signature(DWORD value)
{
	unsigned channels[4], i, j, bit = 8;
	uint32_t signature = 0x80000000u;

	channels[0] = (unsigned)(value >> 24);
	channels[1] = (unsigned)(value >> 16) & 0xff;
	channels[2] = (unsigned)(value >> 8) & 0xff;
	channels[3] = (unsigned)value & 0xff;
	for (i = 0; i < 4; i++)
		signature |= (uint32_t)(channels[i] == 0 ? 1 : channels[i] == 255 ? 2 : 3) << (2 * i);
	for (i = 0; i < 4; i++)
	{
		for (j = i + 1; j < 4; j++, bit++)
		{
			if (channels[i] == channels[j])
				signature |= 1u << bit;
		}
	}
	return signature;
}

/* the structure of a draw's pixel shader, and its constants (the slots
returned: all of them) */
static unsigned long structure_make(struct structure *key, DWORD *constants, const DWORD *state,
	const struct gx_psh_textures *textures)
{
	unsigned long count = state[D3DRS_PSCOMBINERCOUNT] & 0xff, reads = (1UL << NUMBER_OF_SLOTS) - 1, stage;
	int slot, other;

	memset(key, 0, sizeof(*key));
	memset(constants, 0, NUMBER_OF_SLOTS * sizeof(DWORD));
	memcpy(key->state, state, sizeof(key->state));
	key->texture_modes = state[D3DRS_PSTEXTUREMODES];
	if (count > 8)
		count = 8;
	for (stage = count; stage < 8; stage++)
	{
		key->state[D3DRS_PSRGBINPUTS0 + stage] = 0;
		key->state[D3DRS_PSRGBOUTPUTS0 + stage] = 0;
		key->state[D3DRS_PSALPHAINPUTS0 + stage] = 0;
		key->state[D3DRS_PSALPHAOUTPUTS0 + stage] = 0;
	}
	for (slot = 0; slot < 8; slot++)
	{
		key->state[D3DRS_PSCONSTANT0_0 + slot] = 0;
		key->state[D3DRS_PSCONSTANT1_0 + slot] = 0;
	}
	key->state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0;
	key->state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	/* (as the caller has them, padding and all: the device clears them
	first) */
	memcpy(&key->textures, textures, sizeof(key->textures));
	key->textures.fog_color = 0;
	for (slot = 0; slot < NUMBER_OF_SLOTS; slot++)
	{
		if (reads & (1UL << slot))
			constants[slot] = slot_value(state, textures, slot);
	}
	for (slot = 0; slot < NUMBER_OF_SLOTS; slot++)
	{
		if (!(reads & (1UL << slot)))
			continue;
		/* (and the first constant equal to it) */
		for (other = 0; other < slot && (!(reads & (1UL << other)) || constants[other] != constants[slot]); other++);
		key->signatures[slot] = constant_signature(constants[slot]) | (uint32_t)(other + 1) << 16;
	}
	return reads;
}

static uint32_t words_hash(uint32_t hash, const void *data, size_t size)
{
	const uint32_t *word = data;
	size_t index;

	for (index = 0; index < size / sizeof(uint32_t); index++)
	{
		hash = (hash ^ word[index]) * 0x9E3779B1u;
		hash ^= hash >> 15;
	}
	return hash;
}

static unsigned random_channel(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return 1 + (random_state >> 8) % 254;
}

/* a constant of the same signature: its channels of 0 and 255 kept, the
others chosen at random (1 to 254), equal where the constant's are */
static DWORD constant_perturbed(DWORD value)
{
	unsigned channels[4], out[4], i, j;
	BOOL taken;

	channels[0] = (unsigned)(value >> 24);
	channels[1] = (unsigned)(value >> 16) & 0xff;
	channels[2] = (unsigned)(value >> 8) & 0xff;
	channels[3] = (unsigned)value & 0xff;
	for (i = 0; i < 4; i++)
	{
		if (channels[i] == 0 || channels[i] == 255)
		{
			out[i] = channels[i];
			continue;
		}
		for (j = 0; j < i && channels[j] != channels[i]; j++);
		if (j < i)
		{
			out[i] = out[j];
			continue;
		}
		do
		{
			out[i] = random_channel();
			for (taken = FALSE, j = 0; j < i; j++)
				taken |= out[j] == out[i];
		} while (taken);
	}
	return (DWORD)out[0] << 24 | (DWORD)out[1] << 16 | (DWORD)out[2] << 8 | (DWORD)out[3];
}

/* whether a constant has a channel other than 0 and 255 (else it is its
signature's only one) */
static BOOL constant_free(DWORD value)
{
	int shift;

	for (shift = 0; shift < 32; shift += 8)
	{
		unsigned channel = (unsigned)(value >> shift) & 0xff;

		if (channel != 0 && channel != 255)
			return TRUE;
	}
	return FALSE;
}

/* other constants of the same signatures (equal where they are); FALSE
when none could be found */
static BOOL constants_perturbed(unsigned long reads, const DWORD *constants, DWORD *perturbed)
{
	int slot, other, attempt;

	memset(perturbed, 0, NUMBER_OF_SLOTS * sizeof(DWORD));
	for (slot = 0; slot < NUMBER_OF_SLOTS; slot++)
	{
		if (!(reads & (1UL << slot)))
			continue;
		for (other = 0; other < slot && (!(reads & (1UL << other)) || constants[other] != constants[slot]); other++);
		if (other < slot)
		{
			perturbed[slot] = perturbed[other];
			continue;
		}
		for (attempt = 0; attempt < 64; attempt++)
		{
			perturbed[slot] = constant_perturbed(constants[slot]);
			if (slot == _slot_fog)
				perturbed[slot] |= 0xFF000000UL;
			for (other = 0; other < slot && (!(reads & (1UL << other)) || perturbed[other] != perturbed[slot]); other++);
			if (other == slot)
				break;
		}
		if (attempt == 64)
			return FALSE;
	}
	return TRUE;
}

/* the constant each of the program's konst colours and preloaded registers
holds */
static void slots_find(struct cache_entry *entry, unsigned long reads)
{
	const WlTevProgram *tev = &entry->program.tev;
	int index, slot;

	for (index = 0; index < WL_TEV_KONST_SLOTS; index++)
	{
		entry->konst_slots[index] = -1;
		for (slot = 0; slot < NUMBER_OF_SLOTS && (tev->konst_mask & (1u << index)); slot++)
		{
			if ((reads & (1UL << slot)) && rgba(entry->constants[slot]) == tev->konst[index])
			{
				entry->konst_slots[index] = (signed char)slot;
				break;
			}
		}
	}
	for (index = 0; index < WL_TEV_REG_COUNT; index++)
	{
		entry->preload_slots[index] = -1;
		for (slot = 0; slot < NUMBER_OF_SLOTS && (tev->preload_mask & (1u << index)); slot++)
		{
			if ((reads & (1UL << slot)) && rgba(entry->constants[slot]) == tev->preload[index])
			{
				entry->preload_slots[index] = (signed char)slot;
				break;
			}
		}
	}
}

static void constants_patch(const struct cache_entry *entry, struct gx_psh_program *program, const DWORD *constants)
{
	int index;

	for (index = 0; index < WL_TEV_KONST_SLOTS; index++)
	{
		if (entry->konst_slots[index] >= 0)
			program->tev.konst[index] = rgba(constants[(int)entry->konst_slots[index]]);
	}
	for (index = 0; index < WL_TEV_REG_COUNT; index++)
	{
		if (entry->preload_slots[index] >= 0)
			program->tev.preload[index] = rgba(constants[(int)entry->preload_slots[index]]);
	}
}

/* whether a combiner input may read a constant: anything but a colour or a
texture of all four channels, until a stage writes it (written: bit n,
register n) */
static BOOL input_constant(unsigned long input, unsigned long written, const struct gx_psh_textures *textures)
{
	unsigned long reg = input & 0x0f;

	if (written & (1UL << reg))
		return TRUE;
	if (reg == _register_v0 || reg == _register_v1)
		return FALSE;
	if (reg >= _register_t0 && reg <= _register_t3)
		return !(textures->present & (1U << (reg - _register_t0))) ||
			textures->contents[reg - _register_t0] != _gx_psh_texture_all;
	return TRUE;
}

/* whether the combiners may take a dot product of two constants, which the
translation works out (gx_psh.c, emit_dot and emit_signed_dot): its value,
clamped to [0, 1], then decides the stages, where the signatures cannot
follow it */
static BOOL structure_folds(const DWORD *state, const struct gx_psh_textures *textures)
{
	unsigned long count = state[D3DRS_PSCOMBINERCOUNT] & 0xff, stage, written = 0;
	int portion, shift;

	if (count > 8)
		count = 8;
	for (stage = 0; stage < count; stage++)
	{
		DWORD inputs = state[D3DRS_PSRGBINPUTS0 + stage], outputs = state[D3DRS_PSRGBOUTPUTS0 + stage];
		unsigned flags = (unsigned)(outputs >> 12);

		if (((flags & 0x02) && input_constant(inputs >> 24, written, textures) &&
				input_constant((inputs >> 16) & 0xff, written, textures)) ||
			((flags & 0x01) && input_constant((inputs >> 8) & 0xff, written, textures) &&
				input_constant(inputs & 0xff, written, textures)))
			return TRUE;
		for (portion = 0; portion < 2; portion++)
		{
			outputs = state[(portion ? D3DRS_PSALPHAOUTPUTS0 : D3DRS_PSRGBOUTPUTS0) + stage];
			for (shift = 0; shift <= 8; shift += 4)
				written |= 1UL << ((outputs >> shift) & 0x0f);
		}
	}
	return FALSE;
}

/* whether the program changes with its constants only where it holds them */
static BOOL structure_generic(const struct cache_entry *entry, unsigned long reads, const DWORD *state,
	const struct gx_psh_textures *textures)
{
	static DWORD trial_state[TRIAL_STATE_WORDS];
	static struct gx_psh_program expected, trial;
	struct gx_psh_textures trial_textures;
	DWORD perturbed[NUMBER_OF_SLOTS];
	BOOL usable, free = FALSE;
	int attempt, slot;

	/* (no constants, or none but 0s and 1s: no others of their signatures) */
	for (slot = 0; slot < NUMBER_OF_SLOTS; slot++)
		free |= (reads & (1UL << slot)) && constant_free(entry->constants[slot]);
	if (!free)
		return TRUE;
	if (structure_folds(state, textures))
		return FALSE;
	for (attempt = 0; attempt < GENERIC_TRIALS; attempt++)
	{
		if (!constants_perturbed(reads, entry->constants, perturbed))
			return FALSE;
		memcpy(trial_state, state, sizeof(trial_state));
		memcpy(&trial_textures, textures, sizeof(trial_textures));
		for (slot = 0; slot < NUMBER_OF_SLOTS; slot++)
		{
			if (reads & (1UL << slot))
				slot_set(trial_state, &trial_textures, slot, perturbed[slot]);
		}
		usable = gx_psh_translate(trial_state, &trial_textures, &trial);
		counts.tests++;
		memcpy(&expected, &entry->program, sizeof(expected));
		constants_patch(entry, &expected, perturbed);
		if (usable != entry->usable || memcmp(&expected, &trial, sizeof(expected)))
		{
			counts.tests_differ++;
			return FALSE;
		}
	}
	return TRUE;
}

/* the set's entry used longest ago */
static struct cache_entry *set_victim(struct cache_entry *set)
{
	struct cache_entry *victim = &set[0];
	int way;

	for (way = 1; way < CACHE_WAYS; way++)
	{
		if (set[way].stamp < victim->stamp)
			victim = &set[way];
	}
	return victim;
}

/* ---------- public code */

const struct gx_psh_program *gx_psh_cache_program(const DWORD *render_state,
	const struct gx_psh_textures *textures, BOOL *usable, BOOL *translated)
{
	static struct structure key;
	static struct gx_psh_program audit;
	DWORD constants[NUMBER_OF_SLOTS];
	struct cache_entry *set, *entry = NULL, *structural = NULL;
	unsigned long reads;
	uint32_t hash, exact_hash = 0;
	int way;

	*translated = FALSE;
	if (!cache)
	{
		cache = calloc(CACHE_SETS, sizeof(*cache));
		if (!cache)
		{
			static struct gx_psh_program none;

			*usable = FALSE;
			return &none;
		}
	}
	/* (as the last call: its program, still as it was left) */
	if (last_entry && !memcmp(last_state, render_state, sizeof(last_state)) &&
		last_texture_modes == render_state[D3DRS_PSTEXTUREMODES] &&
		!memcmp(&last_textures, textures, sizeof(last_textures)))
	{
		counts.found++;
		*usable = last_entry->usable;
		return &last_entry->program;
	}
	cache_stamp++;
	reads = structure_make(&key, constants, render_state, textures);
	hash = words_hash(0x811C9DC5u, &key, sizeof(key));
	set = cache[hash % CACHE_SETS];
	for (way = 0; way < CACHE_WAYS && !structural; way++)
	{
		if (set[way].stamp && set[way].structural && set[way].hash == hash && !memcmp(&set[way].key, &key, sizeof(key)))
			structural = &set[way];
	}
	if (structural && !memcmp(structural->constants, constants, sizeof(constants)))
	{
		entry = structural;
		counts.found++;
	}
	else if (structural && structural->generic)
	{
		entry = structural;
		memcpy(entry->constants, constants, sizeof(constants));
		constants_patch(entry, &entry->program, constants);
		counts.patched++;
		if (entry->changes++ % AUDIT_INTERVAL == 0)
		{
			BOOL audit_usable = gx_psh_translate(render_state, textures, &audit);

			counts.audited++;
			if (audit_usable != entry->usable || memcmp(&audit, &entry->program, sizeof(audit)))
			{
				counts.audits_differ++;
				counts.translated++;
				counts.unusable += !audit_usable;
				*translated = TRUE;
				entry->generic = FALSE;
				entry->usable = audit_usable;
				memcpy(&entry->program, &audit, sizeof(audit));
			}
		}
	}
	else if (structural)
	{
		/* (not generic: an entry of its own for these constants) */
		exact_hash = words_hash(hash, constants, sizeof(constants));
		set = cache[exact_hash % CACHE_SETS];
		for (way = 0; way < CACHE_WAYS && !entry; way++)
		{
			if (set[way].stamp && !set[way].structural && set[way].hash == exact_hash &&
				!memcmp(set[way].constants, constants, sizeof(constants)) && !memcmp(&set[way].key, &key, sizeof(key)))
			{
				entry = &set[way];
				counts.found++;
			}
		}
	}
	if (!entry)
	{
		unsigned which;

		entry = set_victim(set);
		if (entry->stamp && entry->structural)
		{
			counts.evicted++;
			evicted_hashes[evicted_next++ % EVICTED_HASHES] = entry->hash;
		}
		if (!structural)
			for (which = 0; which < EVICTED_HASHES; which++)
				if (evicted_hashes[which] == hash)
				{
					counts.again++;
					break;
				}
		memcpy(&entry->key, &key, sizeof(key));
		memcpy(entry->constants, constants, sizeof(constants));
		entry->hash = structural ? exact_hash : hash;
		entry->structural = !structural;
		entry->generic = FALSE;
		entry->changes = 0;
		entry->usable = gx_psh_translate(render_state, textures, &entry->program);
		counts.translated++;
		counts.unusable += !entry->usable;
		*translated = TRUE;
		/* (a structure first seen: whether it is generic) */
		if (!structural)
		{
			slots_find(entry, reads);
			entry->generic = (unsigned char)structure_generic(entry, reads, render_state, textures);
			counts.generic += entry->generic;
		}
	}
	entry->stamp = cache_stamp;
	memcpy(last_state, render_state, sizeof(last_state));
	last_texture_modes = render_state[D3DRS_PSTEXTUREMODES];
	memcpy(&last_textures, textures, sizeof(last_textures));
	last_entry = entry;
	*usable = entry->usable;
	return &entry->program;
}

void gx_psh_cache_counts(struct gx_psh_cache_counts *out)
{
	*out = counts;
}
