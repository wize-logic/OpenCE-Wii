/*
GX_PSH.C

The game's pixel shaders on GX's texture environment (TEV).

WiiLibs translates an Xbox pixel shader's register combiners to a TEV
program (wl/xbox/psh.c: its comment says how) and keeps the programs
(wl/xbox/psh_cache.c). Its translation takes a dot product of a register as
1 (no TEV swap reaches a register's channels), and some programs need more
stages or constants than the general translation makes of them; the game's
own programs of that kind are recognized here and made by hand (recipes),
which WiiLibs tries before it translates: the environment's specular pass
from its lightmaps, the models' program and the environment's fog.
*/

#include "gx_psh.h"

#include <string.h>

_Static_assert(sizeof(DWORD) == sizeof(uint32_t), "the render states are WiiLibs' 32-bit words");

/* ---------- operands */

static void approximate(WlXboxPshProgram *program, const char *why)
{
	if (!program->approximate)
		program->approximation = why;
	program->approximate = TRUE;
}

/* Direct3D's ARGB as the TEV program's RGBA */
static uint32_t rgba(DWORD color)
{
	return ((uint32_t)color << 8) | ((uint32_t)color >> 24);
}

/* ---------- the specular lights from lightmaps

The environment's specular pass from its lightmaps
(_rasterizer_environment_specular_lightmaps_begin and its draws, in
rasterizer_xbox_environment.c) reflects the eye's direction e (t2, the
normalization cube map) about the bump map's normal n (t0) and takes its dot
product with the light's direction l (t3) to a power: r = 2n(n.e) - e, then
(r.l)^8, or ^32 when extra shiny. Its colour is the Fresnel colour between
the parallel and perpendicular ones by n.e, times the lightmap's intensity
(t1 against 1/2), times the brightness and the self-shadowing of e and l
(their z each 16 times sharper, clamped), times (r.l)^n, halved. The second
is a dot product of a register, r, which no TEV swap reaches, and the general
translation (combiner_portion) takes it as 1. Here the pass's programs are
recognized and made by hand instead, with r.l as 2(n.e)(n.l) - e.l:

- n.e and n.l are signed dot products, made as emit_signed_dot makes them,
  of the bump map's channels loaded once for both (three stages a channel,
  nine in all), and clamped to [0, 1] (NV2A keeps them signed);
- e.l is the vertices', of their directions (normalized), in colour 0 as
  (1 - e.l) / 2 (_gx_psh_vertices_specular): no stages are left for a third
  dot product;
- the alpha (the alpha test's: a mask where the colour is 0) is 1.

When extra shiny, the two more squarings leave no room for n.l, which is
the flat normal's (l's z). With the bump map as a specular mask (the flat
normal, and the bump map's colour a factor), only e.l is approximated. And
when there is n.l, the lightmap's intensity times the Fresnel colour is
clamped after the shadows and the brightness multiply it (NV2A clamps it
first). */

struct specular_variant
{
	/* the bump map's normal, else the flat one and the bump map a mask */
	BOOL bumped;
	/* the scale of the Fresnel colour times the intensity (overbright: 4) */
	unsigned char scale;
	/* (r.l)^8, or ^32 */
	int squarings;
};

static BOOL specular_lightmap_match(const uint32_t *state, const WlXboxPshTextures *textures,
	struct specular_variant *variant)
{
	static const unsigned long words[][5] =
	{
		/* (combiner stage: rgb inputs (0: the bumped or mask variant's),
		rgb outputs (0: the variant's), alpha inputs and outputs) */
		{ 0, 0, 0x000030C9, 0x4B204B20, 0x00020400 },
		{ 1, 0, 0x00010D00, 0x4A204A20, 0x00020500 },
		{ 2, 0xCD4B0809, 0, 0x2C120C11, 0x00000C00 },
		{ 3, 0x2C020C01, 0x00000C00, 0x0D0D1415, 0x000000D5 },
		{ 4, 0x0C091C09, 0, 0x1D1D151C, 0x000000D5 },
		{ 5, 0x0C150D1D, 0x000010CD, 0x1D1D0000, 0x000000D0 },
	};
	DWORD count = state[D3DRS_PSCOMBINERCOUNT];
	unsigned index;

	if ((count != 0x00011006 && count != 0x00011008) || state[D3DRS_PSFINALCOMBINERINPUTSABCD] != 0x0C0F0000 ||
		state[D3DRS_PSFINALCOMBINERINPUTSEFG] != 0x1D110D00 || (state[D3DRS_PSFINALCOMBINERCONSTANT0] >> 24) != 0x80 ||
		(state[D3DRS_PSCONSTANT0_0] & 0xFF000000UL) != 0x80000000UL ||
		(state[D3DRS_PSCONSTANT0_0] & 0x00FFFFFFUL) != 0x000000FF || (state[D3DRS_PSCONSTANT0_0 + 1] & 0x00FFFFFFUL) !=
		0x000000FF || (state[D3DRS_PSCONSTANT0_0 + 2] >> 24) != (state[D3DRS_PSCONSTANT1_0 + 2] >> 24))
		return FALSE;
	/* (the bump map, the lightmap, and the two directions' sphere maps) */
	if ((textures->present & 0xF) != 0xF || (textures->cube & 0xC) != 0xC || (textures->cube & 3) ||
		textures->separate_alpha || textures->bumped || textures->vertex_color || textures->reflected ||
		textures->contents[0] != WL_XBOX_PSH_TEXTURE_ALL || textures->contents[1] == WL_XBOX_PSH_TEXTURE_ALPHA ||
		textures->fog || textures->d0_alpha_map)
		return FALSE;
	for (index = 0; index < sizeof(words) / sizeof(words[0]); index++)
	{
		unsigned stage = (unsigned)words[index][0];

		if ((words[index][1] && state[D3DRS_PSRGBINPUTS0 + stage] != words[index][1]) ||
			(words[index][2] && state[D3DRS_PSRGBOUTPUTS0 + stage] != words[index][2]) ||
			state[D3DRS_PSALPHAINPUTS0 + stage] != words[index][3] || state[D3DRS_PSALPHAOUTPUTS0 + stage] != words[index][4])
			return FALSE;
	}
	if (state[D3DRS_PSRGBINPUTS0] == 0x484A0911 && state[D3DRS_PSRGBINPUTS0 + 1] == 0x48CC8A40 &&
		state[D3DRS_PSRGBOUTPUTS0 + 2] == 0x000020D0)
		variant->bumped = TRUE;
	else if (state[D3DRS_PSRGBINPUTS0] == 0x014A0911 && state[D3DRS_PSRGBINPUTS0 + 1] == 0x01CC8A40 &&
		state[D3DRS_PSRGBOUTPUTS0 + 2] == 0x000020D9)
		variant->bumped = FALSE;
	else
		return FALSE;
	if (state[D3DRS_PSRGBOUTPUTS0 + 4] == 0x000110CD)
		variant->scale = WL_TEV_SCALE_2;
	else if (state[D3DRS_PSRGBOUTPUTS0 + 4] == 0x000210CD)
		variant->scale = WL_TEV_SCALE_4;
	else
		return FALSE;
	variant->squarings = 3;
	if ((count & 0xFF) == 8)
	{
		for (index = 6; index < 8; index++)
		{
			if (state[D3DRS_PSRGBINPUTS0 + index] || state[D3DRS_PSRGBOUTPUTS0 + index] ||
				state[D3DRS_PSALPHAINPUTS0 + index] != 0x1D1D0000 || state[D3DRS_PSALPHAOUTPUTS0 + index] != 0x000000D0)
				return FALSE;
		}
		variant->squarings = 5;
	}
	return TRUE;
}

/* a stage written to when a recipe has more than 16 (it then fails) */
static WlTevStage recipe_overflow;
static BOOL recipe_overflowed;

/* a recipe's stage: the texture of stage `map` (-1: none) through swap
table `swap`, and colour 0 */
static WlTevStage *recipe_stage(WlTevProgram *tev, int map, unsigned char swap, BOOL ras)
{
	WlTevStage *stage = wl_tev_add_stage(tev);

	if (!stage)
	{
		recipe_overflowed = TRUE;
		stage = &recipe_overflow;
	}
	if (map >= 0)
	{
		stage->texcoord = stage->texmap = (uint8_t)map;
		stage->tex_swap = swap;
	}
	if (ras)
		stage->channel = WL_TEV_CH_COLOR0A0;
	return stage;
}

static void recipe_color(WlTevStage *stage, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t bias, uint8_t scale,
	BOOL clamp, uint8_t dest)
{
	wl_tev_stage_color(stage, a, b, c, d, WL_TEV_ADD, bias, scale, clamp != FALSE, dest);
}

static void recipe_alpha(WlTevStage *stage, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t bias, uint8_t scale,
	BOOL clamp, uint8_t dest)
{
	wl_tev_stage_alpha(stage, a, b, c, d, WL_TEV_ADD, bias, scale, clamp != FALSE, dest);
}

enum { _map_bump = 0, _map_lightmap = 1, _map_eye = 2, _map_light = 3 };
enum { _swap_rgba = 0, _swap_red = 1, _swap_green = 2, _swap_blue = 3 };

/* a channel of the bump map loaded (REG0: 1 - x in the colour, x in the
alpha), and its terms of n.e (into PREV's colour) and, unless `light` is
FALSE, n.l (REG1's colour), each (x - 1/2)(y - 1/2) * 2 = lerp(1 - x, x, y) - 1/2
added to the sum so far; the last term doubles the sum and clamps it */
static WlTevStage *specular_dot_channel(WlTevProgram *tev, unsigned char swap, BOOL first, BOOL last, BOOL light,
	WlTevStage **light_stage)
{
	WlTevStage *stage = recipe_stage(tev, _map_bump, swap, FALSE);

	recipe_color(stage, WL_TEV_CC_ONE, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1,
		TRUE, WL_TEV_REG0);
	recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1,
		TRUE, WL_TEV_REG0);
	stage = recipe_stage(tev, _map_eye, swap, FALSE);
	recipe_color(stage, WL_TEV_CC_C0, WL_TEV_CC_A0, WL_TEV_CC_TEXC, first ? WL_TEV_CC_ZERO : WL_TEV_CC_CPREV,
		WL_TEV_BIAS_SUBHALF, last ? WL_TEV_SCALE_2 : WL_TEV_SCALE_1, last, WL_TEV_PREV);
	*light_stage = NULL;
	if (light)
	{
		*light_stage = recipe_stage(tev, _map_light, swap, FALSE);
		recipe_color(*light_stage, WL_TEV_CC_C0, WL_TEV_CC_A0, WL_TEV_CC_TEXC, first ? WL_TEV_CC_ZERO : WL_TEV_CC_C1,
			WL_TEV_BIAS_SUBHALF, last ? WL_TEV_SCALE_2 : WL_TEV_SCALE_1, last, WL_TEV_REG1);
	}
	return stage;
}

static BOOL specular_lightmap_recipe(const uint32_t *state, const WlXboxPshTextures *textures,
	WlXboxPshProgram *program)
{
	WlTevProgram *tev = &program->tev;
	struct specular_variant variant;
	WlTevStage *stage, *light_stage, *squares[5];
	uint8_t brightness, parallel;
	int index, squaring = 0;

	if (!specular_lightmap_match(state, textures, &variant))
		return FALSE;
	recipe_overflowed = FALSE;
	memset(program, 0, sizeof(*program));
	wl_tev_program_init(tev);
	wl_tev_set_swap(tev, _swap_red, WL_TEV_SWAP_R, WL_TEV_SWAP_R, WL_TEV_SWAP_R, WL_TEV_SWAP_R);
	wl_tev_set_swap(tev, _swap_green, WL_TEV_SWAP_G, WL_TEV_SWAP_G, WL_TEV_SWAP_G, WL_TEV_SWAP_G);
	wl_tev_set_swap(tev, _swap_blue, WL_TEV_SWAP_B, WL_TEV_SWAP_B, WL_TEV_SWAP_B, WL_TEV_SWAP_B);
	/* (the parallel colour a konst, the perpendicular one REG2's colour; the
	brightness a konst's alpha) */
	parallel = (uint8_t)wl_tev_konst_slot(tev, rgba(state[D3DRS_PSCONSTANT1_0 + 3] | 0xFF000000UL));
	brightness = (uint8_t)wl_tev_konst_slot(tev, (uint32_t)(state[D3DRS_PSCONSTANT0_0 + 2] >> 24));
	wl_tev_preload(tev, WL_TEV_REG2, rgba(state[D3DRS_PSCONSTANT0_0 + 3]));
	program->vertices = _gx_psh_vertices_specular;
	program->approximate = TRUE;
	if (variant.bumped && variant.squarings == 3)
	{
		/* n.e into PREV's colour, n.l into REG1's; the shadows of e (REG1's
		alpha) and of l (REG2's), from their z each made 2z - 1, clamped, then
		8 times that */
		program->approximation = "a specular light: e.l the vertices', n.e and n.l clamped, the intensity clamped last";
		stage = specular_dot_channel(tev, _swap_blue, TRUE, FALSE, TRUE, &light_stage);
		recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_BIAS_SUBHALF,
			WL_TEV_SCALE_2, TRUE, WL_TEV_REG1);
		recipe_alpha(light_stage, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_BIAS_SUBHALF,
			WL_TEV_SCALE_2, TRUE, WL_TEV_REG2);
		stage = specular_dot_channel(tev, _swap_red, FALSE, FALSE, TRUE, &light_stage);
		recipe_alpha(stage, WL_TEV_CA_A1, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_A1, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_4, TRUE, WL_TEV_REG1);
		recipe_alpha(light_stage, WL_TEV_CA_A2, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_A2, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_4, TRUE, WL_TEV_REG2);
		/* (the shadows' product, then the brightness's) */
		stage = specular_dot_channel(tev, _swap_green, FALSE, TRUE, TRUE, &light_stage);
		recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_A1, WL_TEV_CA_A2, WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_REG1);
		light_stage->kasel = wl_tev_kasel(brightness, WL_TEV_K_A);
		recipe_alpha(light_stage, WL_TEV_CA_ZERO, WL_TEV_CA_A1, WL_TEV_CA_KONST, WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_REG1);
		/* r.l = 2(n.e)(n.l) - e.l into REG0's colour: ((1 - e.l) / 2 + (n.e)(n.l) - 1/2) * 2 */
		squares[0] = recipe_stage(tev, _map_lightmap, _swap_red, TRUE);
		recipe_color(squares[0], WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_CC_C1, WL_TEV_CC_RASC, WL_TEV_BIAS_SUBHALF,
			WL_TEV_SCALE_2, TRUE, WL_TEV_REG0);
		/* the lightmap's intensity into PREV's alpha (its channels each times
		1/2, clamped at the end), while r.l is squared */
		for (index = 0; index < 3; index++)
		{
			stage = index ? recipe_stage(tev, _map_lightmap, (unsigned char)(_swap_red + index), FALSE) : squares[0];
			stage->kasel = WL_TEV_KSEL_1_2;
			recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_CA_KONST, index ? WL_TEV_CA_APREV : WL_TEV_CA_ZERO,
				WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, index == 2, WL_TEV_PREV);
			if (index)
				recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
					WL_TEV_SCALE_1, TRUE, WL_TEV_REG0);
		}
		/* (r.l)^8; the intensity times the shadows and the brightness */
		stage = recipe_stage(tev, -1, 0, FALSE);
		recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_REG0);
		recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_APREV, WL_TEV_CA_A1, WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
		/* the Fresnel colour, by n.e; times all that, scaled and clamped; times
		(r.l)^8, halved */
		stage = recipe_stage(tev, -1, 0, FALSE);
		stage->kcsel = wl_tev_kcsel(parallel, WL_TEV_K_RGB);
		recipe_color(stage, WL_TEV_CC_KONST, WL_TEV_CC_C2, WL_TEV_CC_CPREV, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
		stage = recipe_stage(tev, -1, 0, FALSE);
		recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_CC_APREV, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
			variant.scale, TRUE, WL_TEV_PREV);
	}
	else
	{
		WlTevStage *free_colors[5];
		int free_count = 0;

		/* n.e into PREV's colour (the bump map's), or e's z (the flat
		normal's: a mask); l's z into REG2's alpha, e's into REG1's; the
		Fresnel colour into REG1's colour; r.l into REG0's colour, its powers
		where a stage's colour is free */
		if (variant.bumped)
		{
			program->approximation = "a specular light: e.l the vertices', n.l the flat normal's, n.e clamped";
			stage = specular_dot_channel(tev, _swap_blue, TRUE, FALSE, FALSE, &light_stage);
			recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_BIAS_SUBHALF,
				WL_TEV_SCALE_2, TRUE, WL_TEV_REG1);
			stage = specular_dot_channel(tev, _swap_red, FALSE, FALSE, FALSE, &light_stage);
			/* (e's shadow into PREV's alpha) */
			recipe_alpha(stage, WL_TEV_CA_A1, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_A1, WL_TEV_BIAS_ZERO,
				WL_TEV_SCALE_4, TRUE, WL_TEV_PREV);
			specular_dot_channel(tev, _swap_green, FALSE, TRUE, FALSE, &light_stage);
		}
		else
		{
			program->approximation = "a specular light: e.l the vertices'";
			/* (n.e = e's z, into PREV's colour, and REG1's alpha; e's shadow
			into PREV's alpha) */
			stage = recipe_stage(tev, _map_eye, _swap_blue, FALSE);
			recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_BIAS_SUBHALF,
				WL_TEV_SCALE_2, TRUE, WL_TEV_PREV);
			recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_BIAS_SUBHALF,
				WL_TEV_SCALE_2, TRUE, WL_TEV_REG1);
			stage = recipe_stage(tev, -1, 0, FALSE);
			recipe_alpha(stage, WL_TEV_CA_A1, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_A1, WL_TEV_BIAS_ZERO,
				WL_TEV_SCALE_4, TRUE, WL_TEV_PREV);
		}
		/* l's z, 2z - 1 clamped (n.l of the flat normal), into REG2's alpha;
		the Fresnel colour by n.e into REG1's colour */
		stage = recipe_stage(tev, _map_light, _swap_blue, FALSE);
		recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_BIAS_SUBHALF,
			WL_TEV_SCALE_2, TRUE, WL_TEV_REG2);
		stage->kcsel = wl_tev_kcsel(parallel, WL_TEV_K_RGB);
		recipe_color(stage, WL_TEV_CC_KONST, WL_TEV_CC_C2, WL_TEV_CC_CPREV, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_REG1);
		/* r.l = 2(n.e)(n.l) - e.l into REG0's colour; l's shadow, 8 times its
		z, into REG2's alpha */
		stage = recipe_stage(tev, -1, 0, TRUE);
		recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_CC_A2, WL_TEV_CC_RASC, WL_TEV_BIAS_SUBHALF,
			WL_TEV_SCALE_2, TRUE, WL_TEV_REG0);
		recipe_alpha(stage, WL_TEV_CA_A2, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_A2, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_4, TRUE, WL_TEV_REG2);
		/* the shadows' product, then the brightness's, into REG2's alpha; the
		lightmap's intensity into PREV's alpha */
		stage = recipe_stage(tev, -1, 0, FALSE);
		recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_APREV, WL_TEV_CA_A2, WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_REG2);
		free_colors[free_count++] = stage;
		stage = recipe_stage(tev, -1, 0, FALSE);
		stage->kasel = wl_tev_kasel(brightness, WL_TEV_K_A);
		recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_A2, WL_TEV_CA_KONST, WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_REG2);
		free_colors[free_count++] = stage;
		for (index = 0; index < 3; index++)
		{
			stage = recipe_stage(tev, _map_lightmap, (unsigned char)(_swap_red + index), FALSE);
			stage->kasel = WL_TEV_KSEL_1_2;
			recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_CA_KONST, index ? WL_TEV_CA_APREV : WL_TEV_CA_ZERO,
				WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, index == 2, WL_TEV_PREV);
			free_colors[free_count++] = stage;
		}
		/* (r.l)'s powers in the free colours, the rest in stages of their own */
		for (squaring = 0; squaring < variant.squarings; squaring++)
		{
			stage = squaring < free_count ? free_colors[squaring] : recipe_stage(tev, -1, 0, FALSE);
			recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
				WL_TEV_SCALE_1, TRUE, WL_TEV_REG0);
		}
		/* the mask's colour times the Fresnel colour (NV2A: the bump map times
		the lightmap's intensity, then times that) */
		if (!variant.bumped)
		{
			stage = recipe_stage(tev, _map_bump, _swap_rgba, FALSE);
			recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_C1, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
				WL_TEV_SCALE_1, TRUE, WL_TEV_REG1);
		}
		/* times the intensity, scaled and clamped; times the shadows and the
		brightness */
		stage = recipe_stage(tev, -1, 0, FALSE);
		recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_C1, WL_TEV_CC_APREV, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
			variant.scale, TRUE, WL_TEV_PREV);
		stage = recipe_stage(tev, -1, 0, FALSE);
		recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_CC_A2, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
	}
	/* times (r.l)^n, halved (the final combiner's 128/255 as 1/2); alpha 1 */
	stage = recipe_stage(tev, -1, 0, FALSE);
	recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
		WL_TEV_DIVIDE_2, TRUE, WL_TEV_PREV);
	stage->kasel = WL_TEV_KSEL_1;
	recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_KONST, WL_TEV_BIAS_ZERO,
		WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
	(void)squaring;
	if (recipe_overflowed)
	{
		program->reason = "more than 16 stages";
		return FALSE;
	}
	if (!wl_tev_validate(tev, NULL))
	{
		program->reason = "an invalid TEV program";
		return FALSE;
	}
	return TRUE;
}

/* ---------- the models' program

The models' pixel shader (rasterizer_xbox_models.c's
set_environment_shader_pixel_shader) is eight combiners over four
textures: the base map t0, the detail map t1, the multipurpose map t2 and
the reflection's cube map t3. Its first stage picks the multipurpose map's
red and green by dot products with c0 and c1 (always 0x00FF0000 and
0x0000FF00), and from there it keeps more values alive than TEV has
registers: the general translation (combiner_portion) gives up on it with
the cube map bound ("no free register"), and the models were drawn as their
base map times the diffuse colour, the detail map, self-illumination,
change colour, reflection and fog left out, and every vertex's sphere map
coordinates computed for a reflection nothing read.

Here the program is recognized and made by hand, exactly: the dot products
are the multipurpose map read through swap tables, the change colour and
the reflection's mask (with v1's alpha) come of one read of it, and the
values are scheduled into PREV, REG0 and REG1 with REG2 holding the
atmospheric fog's colour (c1 of stage 5), a fifth constant past the four
konst colours:

- REG0: R = lerp(1, c0_2, t2.b) (the change colour), its alpha t2.r v1.a
  (the reflection's mask); then t3 v1, the reflection;
- REG1: V = v0 + t2.g c0_1 (self-illumination), then V R;
- PREV's alpha: the detail mask m (a channel of the multipurpose map, or 1),
  its colour the detail lerp(h, t1, m) (h 1/2 or 1, the detail function's);
- then, by double_diffuse_intensity, the detail function f of t0 and the
  detail, times V R, plus the reflection times its mask; or the base map
  times V R plus the reflection, then f of that and the detail; in PREV;
- the final combiner: (1 - a) r0 F.a + a F + clamp(A - a E), a D0's alpha
  (the planar fog's: rasterized, or a ramp's: gx_psh_textures.d0_alpha_map),
  F the fog's colour, E the fog's error colour (c0 of stage 5); or, with the
  true atmospheric fog, the fog register's lerp; alpha t0's.

Each constant is a konst colour (or REG2's preload) as the game sets it, so
the cache (WiiLibs' psh_cache.c) finds the program generic and rewrites only those
for new constants. */

enum
{
	_model_function_biased_multiply,
	_model_function_multiply,
	_model_function_biased_add,
	NUMBER_OF_MODEL_FUNCTIONS
};

struct model_variant
{
	BOOL double_diffuse, true_fog;
	int function;
	/* the detail mask: 1 (no texture read), or a channel (WL_TEV_SWAP_*) of
	the multipurpose map, inverted or not */
	BOOL mask_one, mask_invert;
	unsigned char mask_channel;
	/* the detail function's h: 1, else 1/2 */
	BOOL half_one;
};

/* the detail functions' combiner inputs over x (t0, or r0 when doubled) */
static int model_function_of(DWORD inputs, unsigned long x)
{
	if (inputs == (x << 24 | 0x09UL << 16 | x << 8 | 0x09UL))
		return _model_function_biased_multiply;
	if (inputs == (x << 24 | 0x09UL << 16))
		return _model_function_multiply;
	if (inputs == (x << 24 | 0x20UL << 16 | 0x49UL << 8 | 0x20UL))
		return _model_function_biased_add;
	return -1;
}

static BOOL model_match(const uint32_t *state, const WlXboxPshTextures *textures, struct model_variant *variant)
{
	static const unsigned long words[][5] =
	{
		/* (combiner stage, rgb inputs, rgb outputs, alpha inputs and outputs:
		the stages every variant has) */
		{ 0, 0x0A020A01, 0x000030CD, 0x0A200000, 0x000000C0 },
		{ 1, 0x04200C01, 0x00000400, 0x1C200000, 0x00000090 },
		{ 2, 0x3C201C01, 0x00000C00, 0x0C200D15, 0x000000CD },
		{ 3, 0, 0x00000900, 0, 0 },
		{ 4, 0x0B05040C, 0x000000B4, 0, 0 },
		{ 5, 0x022014E1, 0x00000D00, 0, 0 },
		{ 6, 0, 0, 0, 0 },
		{ 7, 0, 0, 0, 0 },
	};
	DWORD detail = state[D3DRS_PSRGBINPUTS0 + 3];
	unsigned long mask_byte = (detail >> 8) & 0xFF, h_byte = (detail >> 16) & 0xFF;
	unsigned index;

	if (state[D3DRS_PSCOMBINERCOUNT] != 0x00011008 || state[D3DRS_PSCONSTANT0_0] != 0x00FF0000UL ||
		state[D3DRS_PSCONSTANT1_0] != 0x0000FF00UL)
		return FALSE;
	/* (no texture of a kind this does not read as a texture: a bumped or
	device-made lookup, an alpha-only multipurpose or base map; one not
	sampled reads as 0, as it does in the general translation) */
	if (textures->bumped || textures->vertex_color || textures->reflected ||
		textures->contents[0] == WL_XBOX_PSH_TEXTURE_ALPHA || textures->contents[2] == WL_XBOX_PSH_TEXTURE_ALPHA)
		return FALSE;
	for (index = 0; index < sizeof(words) / sizeof(words[0]); index++)
	{
		unsigned stage = (unsigned)words[index][0];

		if ((words[index][1] && state[D3DRS_PSRGBINPUTS0 + stage] != words[index][1]) ||
			(words[index][2] && state[D3DRS_PSRGBOUTPUTS0 + stage] != words[index][2]) ||
			state[D3DRS_PSALPHAINPUTS0 + stage] != words[index][3] || state[D3DRS_PSALPHAOUTPUTS0 + stage] != words[index][4])
			return FALSE;
	}
	/* the detail: (mask ^ invert, h, mask, t1) */
	if (h_byte == 0xA0)
		variant->half_one = FALSE;
	else if (h_byte == 0x20)
		variant->half_one = TRUE;
	else
		return FALSE;
	if ((detail & 0xFF) != 0x09 || (detail >> 24) != (mask_byte ^ 0x20))
		return FALSE;
	variant->mask_one = mask_byte == 0x20;
	variant->mask_invert = (mask_byte & 0x20) != 0;
	variant->mask_channel = WL_TEV_SWAP_R;
	if (!variant->mask_one)
	{
		/* (r1's colour: t2's red; r0's alpha: t2's green, as stage 2 left
		it; t1's alpha: t2's blue, as stage 1 left it; t2's alpha) */
		switch (mask_byte & 0x1F)
		{
		case 0x0D: variant->mask_channel = WL_TEV_SWAP_R; break;
		case 0x1C: variant->mask_channel = WL_TEV_SWAP_G; break;
		case 0x19: variant->mask_channel = WL_TEV_SWAP_B; break;
		case 0x1A: variant->mask_channel = WL_TEV_SWAP_A; break;
		default: return FALSE;
		}
	}
	/* stages 6 and 7: the detail function of t0, then the lighting; or the
	lighting, then the detail function of r0 */
	if (state[D3DRS_PSRGBINPUTS0 + 7] == 0x08040B1D && state[D3DRS_PSRGBOUTPUTS0 + 7] == 0x00000C00 &&
		state[D3DRS_PSRGBOUTPUTS0 + 6] == 0x00000800)
	{
		variant->double_diffuse = FALSE;
		variant->function = model_function_of(state[D3DRS_PSRGBINPUTS0 + 6], 0x08);
	}
	else if (state[D3DRS_PSRGBINPUTS0 + 6] == 0x08040B1D && state[D3DRS_PSRGBOUTPUTS0 + 6] == 0x00000C00 &&
		state[D3DRS_PSRGBOUTPUTS0 + 7] == 0x00000C00)
	{
		variant->double_diffuse = TRUE;
		variant->function = model_function_of(state[D3DRS_PSRGBINPUTS0 + 7], 0x0C);
	}
	else
		return FALSE;
	if (variant->function < 0)
		return FALSE;
	if (state[D3DRS_PSFINALCOMBINERINPUTSABCD] == 0x340F010D && state[D3DRS_PSFINALCOMBINERINPUTSEFG] == 0x0C111800)
		variant->true_fog = FALSE;
	else if (state[D3DRS_PSFINALCOMBINERINPUTSABCD] == 0x330C0300 &&
		state[D3DRS_PSFINALCOMBINERINPUTSEFG] == 0x00001800)
		variant->true_fog = TRUE;
	else
		return FALSE;
	return TRUE;
}

/* a recipe's stage reading texture map `map` at texture coordinate
`texcoord` (no texture where map is WL_TEV_NONE), through swap table
`swap`, and colour `channel` (WL_TEV_NONE: none) */
static WlTevStage *model_stage(WlTevProgram *tev, uint8_t texcoord, uint8_t map, unsigned char swap, uint8_t channel)
{
	WlTevStage *stage = recipe_stage(tev, -1, 0, FALSE);

	if (map != WL_TEV_NONE)
	{
		stage->texcoord = texcoord;
		stage->texmap = map;
		stage->tex_swap = swap;
	}
	if (channel != WL_TEV_NONE)
		stage->channel = channel;
	return stage;
}

/* a texture stage's colour as the recipe reads it: its texel's, 1 for an
alpha-only texture, 0 where it is not sampled */
static uint8_t model_texc(const WlXboxPshTextures *textures, const uint8_t *maps, int stage)
{
	if (maps[stage] == WL_TEV_NONE)
		return WL_TEV_CC_ZERO;
	return textures->contents[stage] == WL_XBOX_PSH_TEXTURE_ALPHA ? WL_TEV_CC_ONE : WL_TEV_CC_TEXC;
}

/* the swap tables the recipe reads the multipurpose map through */
enum { _model_swap_rgba, _model_swap_bbbr, _model_swap_gggg, _model_swap_rgbb };

static BOOL model_recipe(const uint32_t *state, const WlXboxPshTextures *textures, WlXboxPshProgram *program)
{
	WlTevProgram *tev = &program->tev;
	struct model_variant variant;
	WlTevStage *stage;
	/* each texture stage's map (WL_TEV_NONE: not sampled, read as 0) */
	uint8_t maps[4];
	uint8_t c1_slot, c2_slot, f_slot, e_slot;
	/* D0's alpha: the rasterized colour's, or a ramp's (its map at texture
	coordinate 5) */
	uint8_t a_map = textures->d0_alpha_map ? textures->d0_alpha_map : WL_TEV_NONE;
	uint8_t a_input = a_map != WL_TEV_NONE ? WL_TEV_CC_TEXA : WL_TEV_CC_RASA;
	uint8_t a_channel = a_map != WL_TEV_NONE ? WL_TEV_NONE : WL_TEV_CH_COLOR0A0;
	/* where r0 is made (non-doubled: PREV; doubled: REG1, then PREV) */
	int index;

	if (!model_match(state, textures, &variant))
		return FALSE;
	recipe_overflowed = FALSE;
	memset(program, 0, sizeof(*program));
	wl_tev_program_init(tev);
	for (index = 0; index < 4; index++)
		maps[index] = (textures->present & (1U << index)) ? (uint8_t)index : WL_TEV_NONE;
	wl_tev_set_swap(tev, _model_swap_bbbr, WL_TEV_SWAP_B, WL_TEV_SWAP_B, WL_TEV_SWAP_B, WL_TEV_SWAP_R);
	wl_tev_set_swap(tev, _model_swap_gggg, WL_TEV_SWAP_G, WL_TEV_SWAP_G, WL_TEV_SWAP_G, WL_TEV_SWAP_G);
	wl_tev_set_swap(tev, _model_swap_rgbb, WL_TEV_SWAP_R, WL_TEV_SWAP_G, WL_TEV_SWAP_B, WL_TEV_SWAP_B);
	c2_slot = (uint8_t)wl_tev_konst_slot(tev, rgba(state[D3DRS_PSCONSTANT0_0 + 2]));
	c1_slot = (uint8_t)wl_tev_konst_slot(tev, rgba(state[D3DRS_PSCONSTANT0_0 + 1]));
	if (variant.true_fog)
	{
		f_slot = (uint8_t)wl_tev_konst_slot(tev, rgba(textures->fog_color | 0xFF000000UL));
		e_slot = f_slot;
	}
	else
	{
		f_slot = (uint8_t)wl_tev_konst_slot(tev, rgba(state[D3DRS_PSFINALCOMBINERCONSTANT0]));
		e_slot = (uint8_t)wl_tev_konst_slot(tev, rgba(state[D3DRS_PSCONSTANT0_0 + 5]));
		/* (the atmospheric fog's colour, A) */
		wl_tev_preload(tev, WL_TEV_REG2, rgba(state[D3DRS_PSCONSTANT1_0 + 5]));
	}
	/* (wl_tev_konst_slot is -1 past four; there are four at most) */

	/* REG0: lerp(1, c0_2, t2.b), its alpha t2.r v1.a */
	stage = model_stage(tev, 2, maps[2], _model_swap_bbbr, WL_TEV_CH_COLOR1A1);
	stage->kcsel = wl_tev_kcsel(c2_slot, WL_TEV_K_RGB);
	recipe_color(stage, WL_TEV_CC_ONE, WL_TEV_CC_KONST, maps[2] != WL_TEV_NONE ? WL_TEV_CC_TEXC : WL_TEV_CC_ZERO,
		WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE, WL_TEV_REG0);
	recipe_alpha(stage, WL_TEV_CA_ZERO, maps[2] != WL_TEV_NONE ? WL_TEV_CA_TEXA : WL_TEV_CA_ZERO, WL_TEV_CA_RASA,
		WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE, WL_TEV_REG0);
	/* REG1: v0 + t2.g c0_1, then times REG0 */
	stage = model_stage(tev, 2, maps[2], _model_swap_gggg, WL_TEV_CH_COLOR0A0);
	stage->kcsel = wl_tev_kcsel(c1_slot, WL_TEV_K_RGB);
	recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_KONST, maps[2] != WL_TEV_NONE ? WL_TEV_CC_TEXC : WL_TEV_CC_ZERO,
		WL_TEV_CC_RASC, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE, WL_TEV_REG1);
	stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
	recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_C1, WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1,
		TRUE, WL_TEV_REG1);
	/* REG0's colour: the reflection, t3 v1 */
	if (maps[3] != WL_TEV_NONE)
	{
		stage = model_stage(tev, 3, maps[3], _model_swap_rgba, WL_TEV_CH_COLOR1A1);
		recipe_color(stage, WL_TEV_CC_ZERO, model_texc(textures, maps, 3), WL_TEV_CC_RASC, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_REG0);
	}
	/* PREV: the detail mask in its alpha, the detail in its colour */
	if (!variant.mask_one)
	{
		BOOL separate = variant.mask_channel == WL_TEV_SWAP_A && (textures->separate_alpha & 4);
		BOOL ones = variant.mask_channel == WL_TEV_SWAP_A && textures->contents[2] == WL_XBOX_PSH_TEXTURE_COLOR;
		uint8_t source = maps[2] == WL_TEV_NONE ? WL_TEV_CA_ZERO : ones ? WL_TEV_CA_KONST : WL_TEV_CA_TEXA;
		static const unsigned char swaps[4] = { _model_swap_bbbr, _model_swap_gggg, _model_swap_rgbb, _model_swap_rgba };

		stage = model_stage(tev, 2, separate && maps[2] != WL_TEV_NONE ? 6 : maps[2], swaps[variant.mask_channel],
			WL_TEV_NONE);
		stage->kasel = WL_TEV_KSEL_1;
		if (variant.mask_invert)
			recipe_alpha(stage, WL_TEV_CA_KONST, WL_TEV_CA_ZERO, source, WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO,
				WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
		else
			recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, source, WL_TEV_BIAS_ZERO,
				WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
	}
	stage = model_stage(tev, 1, maps[1], _model_swap_rgba, WL_TEV_NONE);
	if (variant.mask_one)
		recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, model_texc(textures, maps, 1),
			WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
	else
		recipe_color(stage, variant.half_one ? WL_TEV_CC_ONE : WL_TEV_CC_HALF, model_texc(textures, maps, 1),
			WL_TEV_CC_APREV, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
	{
		/* the base map, and the detail function of x and the detail (PREV),
		into PREV */
		uint8_t t0 = model_texc(textures, maps, 0);
		uint8_t x;

		if (variant.double_diffuse)
		{
			/* r0 = t0 V R + the reflection, into REG1 */
			stage = model_stage(tev, 0, maps[0], _model_swap_rgba, WL_TEV_NONE);
			recipe_color(stage, WL_TEV_CC_ZERO, t0, WL_TEV_CC_C1, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE,
				WL_TEV_REG1);
			if (maps[3] != WL_TEV_NONE)
			{
				stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
				recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_CC_A0, WL_TEV_CC_C1, WL_TEV_BIAS_ZERO,
					WL_TEV_SCALE_1, TRUE, WL_TEV_REG1);
			}
			stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
			x = WL_TEV_CC_C1;
		}
		else
		{
			stage = model_stage(tev, 0, maps[0], _model_swap_rgba, WL_TEV_NONE);
			x = t0;
		}
		switch (variant.function)
		{
		case _model_function_biased_multiply:
			recipe_color(stage, WL_TEV_CC_ZERO, x, WL_TEV_CC_CPREV, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_2,
				TRUE, WL_TEV_PREV);
			break;
		case _model_function_multiply:
			recipe_color(stage, WL_TEV_CC_ZERO, x, WL_TEV_CC_CPREV, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1,
				TRUE, WL_TEV_PREV);
			break;
		default:
			/* x + 2 detail - 1, as (detail + x / 2 - 1/2) 2 */
			recipe_color(stage, WL_TEV_CC_ZERO, x, WL_TEV_CC_HALF, WL_TEV_CC_CPREV, WL_TEV_BIAS_SUBHALF, WL_TEV_SCALE_2,
				TRUE, WL_TEV_PREV);
			break;
		}
		if (!variant.double_diffuse)
		{
			/* times V R, plus the reflection */
			stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
			recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_CC_C1, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
				WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
			if (maps[3] != WL_TEV_NONE)
			{
				stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
				recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_CC_A0, WL_TEV_CC_CPREV, WL_TEV_BIAS_ZERO,
					WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
			}
		}
	}
	/* the final combiner */
	if (variant.true_fog)
	{
		/* lerp(r0, the fog's colour, the fog's factor: its ramp's, else 1) */
		if (textures->fog)
		{
			stage = model_stage(tev, WL_XBOX_PSH_FOG_TEXCOORD, WL_XBOX_PSH_FOG_TEXMAP, _model_swap_rgba, WL_TEV_NONE);
			stage->kcsel = wl_tev_kcsel(f_slot, WL_TEV_K_RGB);
			recipe_color(stage, WL_TEV_CC_CPREV, WL_TEV_CC_KONST, WL_TEV_CC_TEXA, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
				WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
		}
		else
		{
			stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
			stage->kcsel = wl_tev_kcsel(f_slot, WL_TEV_K_RGB);
			recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_KONST, WL_TEV_BIAS_ZERO,
				WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
		}
	}
	else
	{
		/* (1 - a) r0 F.a + a F, into PREV */
		stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
		stage->kcsel = wl_tev_kcsel(f_slot, WL_TEV_K_A);
		recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_KONST, WL_TEV_CC_CPREV, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
		stage = model_stage(tev, WL_XBOX_PSH_D0_ALPHA_TEXCOORD, a_map, _model_swap_rgba, a_channel);
		stage->kcsel = wl_tev_kcsel(f_slot, WL_TEV_K_RGB);
		recipe_color(stage, WL_TEV_CC_CPREV, WL_TEV_CC_KONST, a_input, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
		/* clamp(A - a E), into REG0 (A is REG2's) */
		stage = model_stage(tev, WL_XBOX_PSH_D0_ALPHA_TEXCOORD, a_map, _model_swap_rgba, a_channel);
		stage->kcsel = wl_tev_kcsel(e_slot, WL_TEV_K_RGB);
		recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_KONST, a_input, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_REG0);
		stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
		wl_tev_stage_color(stage, WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C2, WL_TEV_SUB,
			WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_REG0);
		/* plus it */
		stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
		recipe_color(stage, WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_BIAS_ZERO,
			WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
	}
	/* the alpha: t0's (its separate map's, 1 for a map without one, 0 without
	the map), in the last stage */
	{
		uint8_t map = maps[0] == WL_TEV_NONE ? WL_TEV_NONE : (textures->separate_alpha & 1) ? 4 : 0;
		uint8_t alpha = maps[0] == WL_TEV_NONE ? WL_TEV_CA_ZERO :
			textures->contents[0] == WL_XBOX_PSH_TEXTURE_COLOR ? WL_TEV_CA_KONST : WL_TEV_CA_TEXA;

		if (stage->texmap != WL_TEV_NONE && map != WL_TEV_NONE && alpha == WL_TEV_CA_TEXA)
			stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
		if (alpha == WL_TEV_CA_TEXA)
		{
			stage->texcoord = 0;
			stage->texmap = map;
			stage->tex_swap = _model_swap_rgba;
		}
		stage->kasel = WL_TEV_KSEL_1;
		recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, alpha, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE,
			WL_TEV_PREV);
	}
	program->vertices = WL_XBOX_PSH_VERTICES_PROGRAM;
	if (recipe_overflowed)
	{
		program->reason = "more than 16 stages";
		return FALSE;
	}
	if (!wl_tev_validate(tev, NULL))
	{
		program->reason = "an invalid TEV program";
		return FALSE;
	}
	return TRUE;
}

/* ---------- the environment's fog

The structure's fog pass (rasterizer_xbox_environment_fog.c's
_rasterizer_environment_fog_begin), blended over the structure (ONE,
INVSRCALPHA, its depths EQUAL): t0 the atmospheric fog's density by
distance (its alpha, a), t1 the planar fog's (its alpha and blue), and two
combiners:

  p = c1_0.b t1.a + c1_0.a t1.b (the planar fog's density)
  colour: c0_1 c0_0 a (1 - (1 - c0_1.a) p) + c1_1 p (1 - c0_0.a a)
  alpha:  1 - (1 - c0_0.b a)(1 - p)

The general translation runs out of registers on it with both textures
bound and the game's constants (not trivial in the open: b30's sea), and
the pass was the fallback's t0 times v0. Made by hand in seven stages,
each constant a konst colour as the game sets it, its alpha and blue read
through the konst selections. */

static BOOL environment_fog_recipe(const uint32_t *state, const WlXboxPshTextures *textures,
	WlXboxPshProgram *program)
{
	WlTevProgram *tev = &program->tev;
	WlTevStage *stage;
	uint8_t a00, a10, a01, a11, t0, t1;

	if (state[D3DRS_PSCOMBINERCOUNT] != 0x00011002 || state[D3DRS_PSRGBINPUTS0] != 0x11180118 ||
		state[D3DRS_PSRGBOUTPUTS0] != 0x00000048 || state[D3DRS_PSALPHAINPUTS0] != 0x02191209 ||
		state[D3DRS_PSALPHAOUTPUTS0] != 0x00000C00 || state[D3DRS_PSRGBINPUTS0 + 1] != 0x0108021C ||
		state[D3DRS_PSRGBOUTPUTS0 + 1] != 0x000000CD || state[D3DRS_PSALPHAINPUTS0 + 1] != 0x283C311C ||
		state[D3DRS_PSALPHAOUTPUTS0 + 1] != 0x000000CD || state[D3DRS_PSFINALCOMBINERINPUTSABCD] != 0x0C3D000F ||
		state[D3DRS_PSFINALCOMBINERINPUTSEFG] != 0x0D243C00)
		return FALSE;
	/* (the densities' textures as the general translation reads them: their
	alphas and blue, or 0 not sampled; nothing bumped or made per vertex) */
	if (textures->bumped || textures->vertex_color || textures->reflected || textures->separate_alpha ||
		textures->contents[0] != WL_XBOX_PSH_TEXTURE_ALL || textures->contents[1] != WL_XBOX_PSH_TEXTURE_ALL)
		return FALSE;
	recipe_overflowed = FALSE;
	memset(program, 0, sizeof(*program));
	wl_tev_program_init(tev);
	/* (t1's blue in its alpha) */
	wl_tev_set_swap(tev, _swap_blue, WL_TEV_SWAP_R, WL_TEV_SWAP_G, WL_TEV_SWAP_B, WL_TEV_SWAP_B);
	a00 = (uint8_t)wl_tev_konst_slot(tev, rgba(state[D3DRS_PSCONSTANT0_0]));
	a10 = (uint8_t)wl_tev_konst_slot(tev, rgba(state[D3DRS_PSCONSTANT1_0]));
	a01 = (uint8_t)wl_tev_konst_slot(tev, rgba(state[D3DRS_PSCONSTANT0_0 + 1]));
	a11 = (uint8_t)wl_tev_konst_slot(tev, rgba(state[D3DRS_PSCONSTANT1_0 + 1]));
	t0 = (textures->present & 1) ? WL_TEV_CA_TEXA : WL_TEV_CA_ZERO;
	t1 = (textures->present & 2) ? WL_TEV_CA_TEXA : WL_TEV_CA_ZERO;

	/* p into REG0's alpha: c1_0.b t1.a, then plus c1_0.a t1.b */
	stage = model_stage(tev, 1, (textures->present & 2) ? 1 : WL_TEV_NONE, _swap_rgba, WL_TEV_NONE);
	stage->kasel = wl_tev_kasel(a10, WL_TEV_K_B);
	recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_KONST, t1, WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE,
		WL_TEV_REG0);
	stage = model_stage(tev, 1, (textures->present & 2) ? 1 : WL_TEV_NONE, _swap_blue, WL_TEV_NONE);
	stage->kasel = wl_tev_kasel(a10, WL_TEV_K_A);
	recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_KONST, t1, WL_TEV_CA_A0, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE,
		WL_TEV_REG0);
	/* REG1: c0_0 a, its alpha c0_0.a a (v0's) */
	stage = model_stage(tev, 0, (textures->present & 1) ? 0 : WL_TEV_NONE, _swap_rgba, WL_TEV_NONE);
	stage->kcsel = wl_tev_kcsel(a00, WL_TEV_K_RGB);
	stage->kasel = wl_tev_kasel(a00, WL_TEV_K_A);
	recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_KONST, t0 == WL_TEV_CA_TEXA ? WL_TEV_CC_TEXA : WL_TEV_CC_ZERO,
		WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE, WL_TEV_REG1);
	recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_KONST, t0, WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE,
		WL_TEV_REG1);
	/* REG1's colour times c0_1 (r0); PREV's alpha c0_0.b a */
	stage = model_stage(tev, 0, (textures->present & 1) ? 0 : WL_TEV_NONE, _swap_rgba, WL_TEV_NONE);
	stage->kcsel = wl_tev_kcsel(a01, WL_TEV_K_RGB);
	stage->kasel = wl_tev_kasel(a00, WL_TEV_K_B);
	recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_KONST, WL_TEV_CC_C1, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1,
		TRUE, WL_TEV_REG1);
	recipe_alpha(stage, WL_TEV_CA_ZERO, WL_TEV_CA_KONST, t0, WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, TRUE,
		WL_TEV_PREV);
	/* REG2: c1_1 p (r1), its alpha (1 - c0_1.a) p */
	stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
	stage->kcsel = wl_tev_kcsel(a11, WL_TEV_K_RGB);
	stage->kasel = wl_tev_kasel(a01, WL_TEV_K_A);
	recipe_color(stage, WL_TEV_CC_ZERO, WL_TEV_CC_KONST, WL_TEV_CC_A0, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1,
		TRUE, WL_TEV_REG2);
	recipe_alpha(stage, WL_TEV_CA_A0, WL_TEV_CA_ZERO, WL_TEV_CA_KONST, WL_TEV_CA_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1,
		TRUE, WL_TEV_REG2);
	/* the colour: r0 (1 - r1.a), plus r1 (1 - v0); the alpha: 1 - (1 - c0_0.b a)(1 - p) */
	stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
	recipe_color(stage, WL_TEV_CC_C1, WL_TEV_CC_ZERO, WL_TEV_CC_A2, WL_TEV_CC_ZERO, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1,
		TRUE, WL_TEV_REG1);
	stage = model_stage(tev, 0, WL_TEV_NONE, 0, WL_TEV_NONE);
	recipe_color(stage, WL_TEV_CC_C2, WL_TEV_CC_ZERO, WL_TEV_CC_A1, WL_TEV_CC_C1, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1,
		TRUE, WL_TEV_PREV);
	recipe_alpha(stage, WL_TEV_CA_A0, WL_TEV_CA_ZERO, WL_TEV_CA_APREV, WL_TEV_CA_APREV, WL_TEV_BIAS_ZERO,
		WL_TEV_SCALE_1, TRUE, WL_TEV_PREV);
	program->vertices = WL_XBOX_PSH_VERTICES_PROGRAM;
	if (recipe_overflowed)
	{
		program->reason = "more than 16 stages";
		return FALSE;
	}
	if (!wl_tev_validate(tev, NULL))
	{
		program->reason = "an invalid TEV program";
		return FALSE;
	}
	return TRUE;
}

/* ---------- public code */

BOOL gx_psh_specular_recipes = TRUE;

/* the game's programs made by hand: the specular lights' (unless
gx_psh_specular_recipes is FALSE), the models' and the environment's fog */
static bool recipe_make(const uint32_t *state, const WlXboxPshTextures *textures, WlXboxPshProgram *program,
	void *user)
{
	(void)user;
	return (gx_psh_specular_recipes && specular_lightmap_recipe(state, textures, program)) ||
		model_recipe(state, textures, program) || environment_fog_recipe(state, textures, program);
}

static const WlXboxPshRecipe recipe = { recipe_make, NULL };
static WlXboxPshCache cache;
static BOOL cache_ready;

BOOL gx_psh_translate(const DWORD *render_state, const WlXboxPshTextures *textures, WlXboxPshProgram *program)
{
	return wl_xbox_psh_translate((const uint32_t *)render_state, textures, &recipe, program) ? TRUE : FALSE;
}

const WlXboxPshProgram *gx_psh_cache_program(const DWORD *render_state, const WlXboxPshTextures *textures,
	BOOL *usable, BOOL *translated)
{
	const WlXboxPshProgram *program;
	bool program_usable, program_translated;

	if (!cache_ready)
	{
		wl_xbox_psh_cache_init(&cache, &recipe);
		cache_ready = TRUE;
	}
	/* (the texture modes in the key: the textures' description is made of
	them) */
	program = wl_xbox_psh_cache_program(&cache, (const uint32_t *)render_state,
		(uint32_t)render_state[D3DRS_PSTEXTUREMODES], textures, &program_usable, &program_translated);
	*usable = program_usable ? TRUE : FALSE;
	*translated = program_translated ? TRUE : FALSE;
	return program;
}

void gx_psh_cache_counts(WlXboxPshCacheCounts *counts)
{
	if (cache_ready)
		wl_xbox_psh_cache_counts(&cache, counts);
	else
		memset(counts, 0, sizeof(*counts));
}
