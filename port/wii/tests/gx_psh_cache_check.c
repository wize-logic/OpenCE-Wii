/*
GX_PSH_CACHE_CHECK.C

A host check of WiiLibs' wl/xbox/psh_cache.c with the game's recipes
(port/wii/src/gx_psh.c; tools/wii_gx_psh_check.py builds and runs it after
gx_psh_check): every program the cache gives is compared with the combiners
translated afresh (gx_psh_translate), byte for byte.

- Random combiner programs, each drawn with its constants as they were,
  changed within their signatures (a channel's 0s and 1s kept), and changed
  outside them.
- The specular pass from a lightmap, its constants changed as a light's are.
- The game's model program (the 8 combiners rasterizer_xbox_models.c sets
  up, as b30's log shows them) drawn for many objects over many frames, each
  object's fog constants changing every frame, as the game sets them from
  its distance: the translations it takes are counted against the device's
  cache before this one (48 programs, each for the whole of its render
  states).

usage: gx_psh_cache_check [programs [seed]]
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_psh.h"

static unsigned long compared, differ;

static void compare(const DWORD *state, const WlXboxPshTextures *textures, const char *what);

static unsigned rand_input(int alpha_portion)
{
	static const unsigned regs[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13 };
	static const unsigned mappings[] = { 0x00, 0x20, 0x40, 0x60, 0x80, 0xa0, 0xc0, 0xe0 };
	unsigned value = regs[rand() % 12] | mappings[rand() % 8];

	if (alpha_portion ? rand() % 2 : rand() % 4 == 0)
		value |= 0x10;
	return value;
}

static unsigned rand_output(void)
{
	static const unsigned outputs[] = { 0, 0, 0, 4, 5, 8, 9, 10, 11, 12, 13 };

	return outputs[rand() % 11];
}

/* a constant: often 0 or 1 in a channel, as the game's are */
static DWORD rand_constant(void)
{
	DWORD value = 0;
	int channel;

	for (channel = 0; channel < 4; channel++)
	{
		int kind = rand() % 4;

		value = (value << 8) | (kind == 0 ? 0 : kind == 1 ? 255 : (unsigned)(rand() % 256));
	}
	if (rand() % 4 == 0)
	{
		/* (grey) */
		unsigned level = (unsigned)(rand() % 256);

		value = (value & 0xFF000000UL) | level << 16 | level << 8 | level;
	}
	return value;
}

/* the same, its channels other than 0 and 255 changed (equal ones together) */
static DWORD same_signature(DWORD value)
{
	unsigned channels[4], out[4];
	int i, j;

	for (i = 0; i < 4; i++)
		channels[i] = (unsigned)(value >> (24 - 8 * i)) & 0xff;
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
		for (;;)
		{
			int taken = 0;

			out[i] = 1 + (unsigned)(rand() % 254);
			for (j = 0; j < i; j++)
				taken |= out[j] == out[i];
			if (!taken)
				break;
		}
	}
	return (DWORD)out[0] << 24 | (DWORD)out[1] << 16 | (DWORD)out[2] << 8 | out[3];
}

static void program_random(DWORD *state, WlXboxPshTextures *textures)
{
	int count = 1 + rand() % 8, stage, portion, k;

	memset(state, 0, 200 * sizeof(DWORD));
	memset(textures, 0, sizeof(*textures));
	state[D3DRS_PSCOMBINERCOUNT] = count | (rand() % 2 ? 0x1000 : 0) | (rand() % 2 ? 0x10000 : 0);
	for (stage = 0; stage < count; stage++)
	{
		for (portion = 0; portion < 2; portion++)
		{
			DWORD in = 0, out;
			unsigned ab = rand_output(), cd = rand_output(), sum = rand_output(), flags;
			static const unsigned mapflags[] = { 0, 0, 0x08, 0x10, 0x18, 0x20, 0x30 };

			for (k = 0; k < 4; k++)
				in = (in << 8) | rand_input(portion);
			if (ab && ab == cd)
				cd = 0;
			if (sum && (sum == ab || sum == cd))
				sum = 0;
			flags = mapflags[rand() % 7] | (sum && rand() % 4 == 0 ? 0x04 : 0) |
				(!portion && rand() % 5 == 0 ? 0x02 : 0) | (!portion && rand() % 5 == 0 ? 0x01 : 0);
			out = (flags << 12) | (sum << 8) | (ab << 4) | cd;
			state[(portion ? D3DRS_PSALPHAINPUTS0 : D3DRS_PSRGBINPUTS0) + stage] = in;
			state[(portion ? D3DRS_PSALPHAOUTPUTS0 : D3DRS_PSRGBOUTPUTS0) + stage] = out;
		}
	}
	/* (stale stages beyond the count, as the game leaves them) */
	for (; stage < 8; stage++)
		state[D3DRS_PSRGBINPUTS0 + stage] = (DWORD)rand() * 2654435761u;
	if (rand() % 3)
	{
		static const unsigned r[] = { 0, 1, 2, 3, 4, 5, 8, 9, 12, 13, 14, 15 };
		static const unsigned e_values[] = { 0, 1, 4, 8, 12 }, f_values[] = { 0, 2, 5, 9, 13 };
		static const unsigned g_values[] = { 0x1c, 0x18, 0x14, 0x1d, 0x11, 0x03 };
		DWORD abcd = 0;
		unsigned e, f, g;

		for (k = 0; k < 4; k++)
			abcd = (abcd << 8) | r[rand() % 12] | (rand() % 3 == 0 ? 0x10 : 0) | (rand() % 3 == 0 ? 0x20 : 0);
		e = e_values[rand() % 5];
		f = f_values[rand() % 5];
		g = g_values[rand() % 6] | (rand() % 3 == 0 ? 0x20 : 0);
		state[D3DRS_PSFINALCOMBINERINPUTSABCD] = abcd;
		state[D3DRS_PSFINALCOMBINERINPUTSEFG] = (e << 24) | (f << 16) | (g << 8) | ((rand() % 2) ? 0x20 : 0);
	}
	textures->present = (unsigned)rand() % 16;
	textures->separate_alpha = textures->present & (unsigned)rand();
	for (k = 0; k < WL_XBOX_PSH_TEXTURES; k++)
		textures->contents[k] = (unsigned char)(textures->present & (1U << k) ? rand() % 3 : 0);
	textures->fog = (unsigned char)(rand() % 2);
	/* (now and then a bumped reflection's lookup, or a stage read as colour 1) */
	if (rand() % 8 == 0)
	{
		textures->present |= 8;
		textures->cube |= 8;
		textures->bumped = 8;
	}
	if (rand() % 8 == 0)
		textures->vertex_color = 4;
}

/* the specular pass from a lightmap (gx_psh.c's specular_lightmap_match),
its constants changed as a light's are */
static void specular(int draws)
{
	static const DWORD rgb_in[6] = { 0x484A0911, 0x48CC8A40, 0xCD4B0809, 0x2C020C01, 0x0C091C09, 0x0C150D1D };
	static const DWORD rgb_out[6] = { 0x000030C9, 0x00010D00, 0x000020D0, 0x00000C00, 0x000110CD, 0x000010CD };
	static const DWORD alpha_in[6] = { 0x4B204B20, 0x4A204A20, 0x2C120C11, 0x0D0D1415, 0x1D1D151C, 0x1D1D0000 };
	static const DWORD alpha_out[6] = { 0x00020400, 0x00020500, 0x00000C00, 0x000000D5, 0x000000D5, 0x000000D0 };
	static WlXboxPshProgram fresh;
	int draw, stage, recipes = 0;

	for (draw = 0; draw < draws; draw++)
	{
		DWORD state[200];
		WlXboxPshTextures textures;
		DWORD brightness = (DWORD)(draw % 5 == 0 ? 0xFF : 0x20 + rand() % 0xC0);

		memset(state, 0, sizeof(state));
		memset(&textures, 0, sizeof(textures));
		state[D3DRS_PSCOMBINERCOUNT] = 0x00011006;
		state[D3DRS_PSFINALCOMBINERINPUTSABCD] = 0x0C0F0000;
		state[D3DRS_PSFINALCOMBINERINPUTSEFG] = 0x1D110D00;
		for (stage = 0; stage < 6; stage++)
		{
			state[D3DRS_PSRGBINPUTS0 + stage] = rgb_in[stage];
			state[D3DRS_PSRGBOUTPUTS0 + stage] = rgb_out[stage];
			state[D3DRS_PSALPHAINPUTS0 + stage] = alpha_in[stage];
			state[D3DRS_PSALPHAOUTPUTS0 + stage] = alpha_out[stage];
		}
		state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0x80000000UL | (DWORD)(rand() % 0x1000000);
		state[D3DRS_PSCONSTANT0_0] = 0x800000FF;
		/* (read by no combiner input, but by the match) */
		state[D3DRS_PSCONSTANT0_0 + 1] = draw % 7 == 3 ? 0x00000080 : 0x000000FF;
		state[D3DRS_PSCONSTANT0_0 + 2] = brightness << 24 | (DWORD)(rand() % 0x1000000);
		state[D3DRS_PSCONSTANT1_0 + 2] = brightness << 24;
		state[D3DRS_PSCONSTANT0_0 + 3] = (DWORD)rand() * 2654435761u;
		state[D3DRS_PSCONSTANT1_0 + 3] = (DWORD)rand() * 40503u;
		textures.present = 0xF;
		textures.cube = 0xC;
		compare(state, &textures, "the specular pass");
		gx_psh_translate(state, &textures, &fresh);
		recipes += fresh.vertices == _gx_psh_vertices_specular;
	}
	printf("the specular pass: %d draws, %d of them the recipe's\n", draws, recipes);
}

static void constants_random(DWORD *state, WlXboxPshTextures *textures)
{
	int k;

	for (k = 0; k < 8; k++)
	{
		state[D3DRS_PSCONSTANT0_0 + k] = rand_constant();
		state[D3DRS_PSCONSTANT1_0 + k] = rand_constant();
	}
	/* (some equal to another, as the game's often are) */
	if (rand() % 2)
		state[D3DRS_PSCONSTANT1_0] = state[D3DRS_PSCONSTANT0_0];
	state[D3DRS_PSFINALCOMBINERCONSTANT0] = rand_constant();
	state[D3DRS_PSFINALCOMBINERCONSTANT1] = rand() % 2 ? state[D3DRS_PSCONSTANT0_0 + 1] : rand_constant();
	textures->fog_color = rand_constant() & 0x00FFFFFFUL;
}

static void constants_resigned(DWORD *state, WlXboxPshTextures *textures)
{
	DWORD before[18], after[18];
	int k, j;

	for (k = 0; k < 8; k++)
	{
		before[k] = state[D3DRS_PSCONSTANT0_0 + k];
		before[8 + k] = state[D3DRS_PSCONSTANT1_0 + k];
	}
	before[16] = state[D3DRS_PSFINALCOMBINERCONSTANT0];
	before[17] = state[D3DRS_PSFINALCOMBINERCONSTANT1];
	for (k = 0; k < 18; k++)
	{
		for (j = 0; j < k && before[j] != before[k]; j++);
		after[k] = j < k ? after[j] : same_signature(before[k]);
	}
	for (k = 0; k < 8; k++)
	{
		state[D3DRS_PSCONSTANT0_0 + k] = after[k];
		state[D3DRS_PSCONSTANT1_0 + k] = after[8 + k];
	}
	state[D3DRS_PSFINALCOMBINERCONSTANT0] = after[16];
	state[D3DRS_PSFINALCOMBINERCONSTANT1] = after[17];
	textures->fog_color = same_signature(textures->fog_color | 0xFF000000UL) & 0x00FFFFFFUL;
}

/* the cache's program against a fresh translation */
static void compare(const DWORD *state, const WlXboxPshTextures *textures, const char *what)
{
	static WlXboxPshProgram fresh;
	const WlXboxPshProgram *cached;
	BOOL usable, fresh_usable, translated;

	cached = gx_psh_cache_program(state, textures, &usable, &translated);
	fresh_usable = gx_psh_translate(state, textures, &fresh);
	compared++;
	if (usable != fresh_usable || memcmp(cached, &fresh, sizeof(fresh)))
	{
		if (differ++ < 8)
		{
			int k;

			printf("DIFFERS (%s): count %08lx, cached %s, fresh %s\n", what, (unsigned long)state[D3DRS_PSCOMBINERCOUNT],
				usable ? "usable" : "beyond GX", fresh_usable ? "usable" : "beyond GX");
			if (getenv("VERBOSE"))
			{
				printf("  stages %u/%u konst", cached->tev.stage_count, fresh.tev.stage_count);
				for (k = 0; k < 4; k++)
					printf(" %08x/%08x", cached->tev.konst[k], fresh.tev.konst[k]);
				printf(" preload");
				for (k = 0; k < 4; k++)
					printf(" %08x/%08x", cached->tev.preload[k], fresh.tev.preload[k]);
				printf("\n  c0");
				for (k = 0; k < 8; k++)
					printf(" %08lx", (unsigned long)state[D3DRS_PSCONSTANT0_0 + k]);
				printf("\n  c1");
				for (k = 0; k < 8; k++)
					printf(" %08lx", (unsigned long)state[D3DRS_PSCONSTANT1_0 + k]);
				printf("\n  final %08lx %08lx fog %08lx, inputs final %08lx %08lx\n",
					(unsigned long)state[D3DRS_PSFINALCOMBINERCONSTANT0], (unsigned long)state[D3DRS_PSFINALCOMBINERCONSTANT1],
					(unsigned long)textures->fog_color, (unsigned long)state[D3DRS_PSFINALCOMBINERINPUTSABCD],
					(unsigned long)state[D3DRS_PSFINALCOMBINERINPUTSEFG]);
				for (k = 0; k < (int)(state[D3DRS_PSCOMBINERCOUNT] & 0xff); k++)
					printf("  %d: rgb %08lx -> %08lx, alpha %08lx -> %08lx\n", k,
						(unsigned long)state[D3DRS_PSRGBINPUTS0 + k], (unsigned long)state[D3DRS_PSRGBOUTPUTS0 + k],
						(unsigned long)state[D3DRS_PSALPHAINPUTS0 + k], (unsigned long)state[D3DRS_PSALPHAOUTPUTS0 + k]);
				for (k = 0; k < cached->tev.stage_count; k++)
					if (memcmp(&cached->tev.stages[k], &fresh.tev.stages[k], sizeof(fresh.tev.stages[k])))
						printf("  stage %d differs\n", k);
				printf("  approximate %d/%d texcoords %x/%x\n", cached->approximate, fresh.approximate, cached->texcoords,
					fresh.texcoords);
			}
		}
	}
}

/* ---------- the game's model program, over frames */

/* the device's cache before the structures' one: 48 programs, each for the whole
of the pixel shader render states and the textures */
static DWORD old_states[48][D3DRS_PS_MAX];
static WlXboxPshTextures old_textures[48];
static unsigned old_count, old_next;

static int old_cache_translates(const DWORD *state, const WlXboxPshTextures *textures)
{
	unsigned index;

	for (index = 0; index < old_count; index++)
	{
		if (!memcmp(old_states[index], state, sizeof(old_states[index])) &&
			!memcmp(&old_textures[index], textures, sizeof(*textures)))
			return 0;
	}
	index = old_next;
	old_next = (old_next + 1) % 48;
	if (old_count < 48)
		old_count++;
	memcpy(old_states[index], state, sizeof(old_states[index]));
	old_textures[index] = *textures;
	return 1;
}

static void models(int objects, int frames)
{
	static const DWORD ri[] = { 0x0a020a01, 0x04200c01, 0x3c201c01, 0x00a02009, 0x0b05040c, 0x022014e1, 0x08090809, 0x08040b1d };
	static const DWORD ro[] = { 0x000030cd, 0x00000400, 0x00000c00, 0x00000900, 0x000000b4, 0x00000d00, 0x00000800, 0x00000c00 };
	static const DWORD ai[] = { 0x0a200000, 0x1c200000, 0x0c200d15, 0, 0, 0, 0, 0 };
	static const DWORD ao[] = { 0x000000c0, 0x00000090, 0x000000cd, 0, 0, 0, 0, 0 };
	WlXboxPshCacheCounts before, after;
	unsigned long old_translations = 0;
	int frame, object, stage;

	gx_psh_cache_counts(&before);
	for (frame = 0; frame < frames; frame++)
	{
		for (object = 0; object < objects; object++)
		{
			DWORD state[200];
			WlXboxPshTextures textures;
			/* (the fog's colour scaled by the object's density, from its
			distance; the planar fog's error 0; its change colours) */
			float density = 0.2f + 0.6f * (float)((object * 37 + frame * (1 + object % 3)) % 100) / 100.0f;
			unsigned fog = (unsigned)(density * 255.0f + 0.5f);
			DWORD atmospheric = 0xFF000000UL | (DWORD)(0x40 * fog / 255) << 16 | (DWORD)(0x60 * fog / 255) << 8 | (DWORD)(0x80 * fog / 255);

			memset(state, 0, sizeof(state));
			memset(&textures, 0, sizeof(textures));
			/* (stage 3's cube map sampled as a sphere map on half the objects:
			the program is then beyond GX for many constants) */
			textures.present = object % 2 ? 0xF : 7;
			textures.cube = textures.present & 8;
			textures.separate_alpha = object % 5 == 0;
			state[D3DRS_PSCOMBINERCOUNT] = 0x00011008;
			state[D3DRS_PSFINALCOMBINERINPUTSABCD] = 0x340f010d;
			state[D3DRS_PSFINALCOMBINERINPUTSEFG] = 0x0c111800;
			state[D3DRS_PSTEXTUREMODES] = 0x00018421;
			for (stage = 0; stage < 8; stage++)
			{
				state[D3DRS_PSRGBINPUTS0 + stage] = ri[stage];
				state[D3DRS_PSRGBOUTPUTS0 + stage] = ro[stage];
				state[D3DRS_PSALPHAINPUTS0 + stage] = ai[stage];
				state[D3DRS_PSALPHAOUTPUTS0 + stage] = ao[stage];
			}
			state[D3DRS_PSCONSTANT0_0] = 0x00FF0000;
			state[D3DRS_PSCONSTANT1_0] = 0x0000FF00;
			state[D3DRS_PSCONSTANT0_0 + 1] = object % 4 ? 0x00000000 : 0x00807060;
			state[D3DRS_PSCONSTANT0_0 + 2] = object % 3 ? 0xFFFFFFFF : 0xFF9070A0 + (DWORD)(object % 7);
			state[D3DRS_PSCONSTANT0_0 + 5] = 0xFF000000;
			state[D3DRS_PSCONSTANT1_0 + 5] = atmospheric;
			state[D3DRS_PSFINALCOMBINERCONSTANT0] = (DWORD)(255 - fog) << 24 | 0x203040;
			state[D3DRS_PSFINALCOMBINERCONSTANT1] = atmospheric;
			old_translations += (unsigned long)old_cache_translates(state, &textures);
			compare(state, &textures, "the model program");
		}
	}
	gx_psh_cache_counts(&after);
	printf("the model program, %d objects over %d frames: %lu translations (%.2f a frame), the cache before %lu (%.2f a frame); "
		"%lu kept, %lu with their constants changed, %lu tests, %lu audits (%lu differ)\n", objects, frames,
		after.translated - before.translated, (double)(after.translated - before.translated) / frames, old_translations,
		(double)old_translations / frames, after.found - before.found, after.patched - before.patched,
		after.tests - before.tests, after.audited - before.audited, after.audits_differ - before.audits_differ);
}

int main(int argc, char **argv)
{
	int programs = argc > 1 ? atoi(argv[1]) : 3000, program, draw;
	WlXboxPshCacheCounts counts;

	srand(argc > 2 ? (unsigned)atoi(argv[2]) : 2342);
	for (program = 0; program < programs; program++)
	{
		DWORD state[200];
		WlXboxPshTextures textures;

		program_random(state, &textures);
		constants_random(state, &textures);
		compare(state, &textures, "as first drawn");
		for (draw = 0; draw < 6; draw++)
		{
			if (draw == 5)
				constants_random(state, &textures);
			else
				constants_resigned(state, &textures);
			compare(state, &textures, draw == 5 ? "other constants" : "constants of the same signatures");
			compare(state, &textures, "again");
		}
	}
	gx_psh_cache_counts(&counts);
	printf("random programs: %lu draws compared, %lu differ; %lu translated (%lu beyond GX), %lu kept, %lu with their "
		"constants changed; %lu tests (%lu otherwise) found %lu generic; %lu audits, %lu differ\n", compared, differ, counts.translated,
		counts.unusable, counts.found, counts.patched, counts.tests, counts.tests_differ, counts.generic, counts.audited, counts.audits_differ);
	models(60, 120);
	specular(2000);
	printf("%lu draws compared, %lu differ from a fresh translation\n", compared, differ);
	return differ != 0;
}
