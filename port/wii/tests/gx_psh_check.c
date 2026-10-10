/*
GX_PSH_CHECK.C

A host check of port/wii/src/gx_psh.c (tools/wii_gx_psh_check.py builds and
runs it): random register combiner programs, and two of the game's, are
translated, and the TEV program, simulated here, is compared with the
combiners evaluated directly, with the translation's own approximations
(signed mappings clamped, expand-negate as an inversion, no fog, dot
products as 1, the mux as a blend by r0's alpha). An alpha portion's reads
are of alphas (a register's blue is read as its alpha), and a program with
more constants than GX's four is left out, as their values are approximate.

usage: gx_psh_check [programs [seed]] (SIM_TRACE=1 prints the stages)
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_psh.h"

typedef struct { float c[4]; } v4; /* r g b a */
static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float frand(void) { return (float)(rand() % 256) / 255.0f; }
static v4 rand4(void) { v4 v; for (int i = 0; i < 4; i++) v.c[i] = frand(); return v; }
static v4 argb_to_v4(DWORD d) { v4 v = {{ ((d >> 16) & 255) / 255.0f, ((d >> 8) & 255) / 255.0f, (d & 255) / 255.0f, (d >> 24) / 255.0f }}; return v; }

/* ---------- the reference */
struct inputs_set { v4 t[4]; v4 v0, v1; };
/* the fog: on or not, its colour, and a pixel's factor */
static int fog_on;
static DWORD fog_color;
static float fog_factor;
static v4 reg_value(const DWORD *s, int reg, int stage, v4 *regs /* indexed by reg number */)
{
	v4 z = {{0}};
	switch (reg) {
	case 1: { DWORD cnt = s[D3DRS_PSCOMBINERCOUNT]; int u = (cnt & 0x1000) != 0;
		return argb_to_v4(stage < 0 ? s[D3DRS_PSFINALCOMBINERCONSTANT0] : s[D3DRS_PSCONSTANT0_0 + (u ? stage : 0)]); }
	case 2: { DWORD cnt = s[D3DRS_PSCOMBINERCOUNT]; int u = (cnt & 0x10000) != 0;
		return argb_to_v4(stage < 0 ? s[D3DRS_PSFINALCOMBINERCONSTANT1] : s[D3DRS_PSCONSTANT1_0 + (u ? stage : 0)]); }
	case 3: { v4 f = argb_to_v4(fog_color); f.c[3] = fog_on ? fog_factor : 1.0f; return f; }
	case 4: case 5: case 8: case 9: case 10: case 11: case 12: case 13: return regs[reg];
	default: return z;
	}
}
static float map_input(float x, unsigned mapping)
{
	switch (mapping) {
	case 0x00: case 0xc0: return clamp01(x);
	case 0x20: return 1 - clamp01(x);
	case 0x40: return clamp01(2 * x - 1);
	case 0x60: return clamp01(1 - 2 * clamp01(x));
	case 0x80: return clamp01(x - 0.5f);
	case 0xa0: return clamp01(0.5f - clamp01(x));
	default: return -clamp01(x);
	}
}
static float out_map(float x, unsigned flags)
{
	switch (flags & 0x38) {
	case 0x08: x = x - 0.5f; break;
	case 0x10: x = x * 2; break;
	case 0x18: x = (x - 0.5f) * 2; break;
	case 0x20: x = x * 4; break;
	case 0x30: x = x * 0.5f; break;
	}
	return clamp01(x);
}
static v4 reference(const DWORD *s, const struct inputs_set *in, const unsigned present)
{
	v4 regs[16]; memset(regs, 0, sizeof(regs));
	for (int i = 0; i < 4; i++) if (present & (1u << i)) regs[8 + i] = in->t[i];
	regs[4] = in->v0; regs[5] = in->v1;
	regs[12].c[3] = regs[8].c[3];
	int count = s[D3DRS_PSCOMBINERCOUNT] & 0xff;
	for (int st = 0; st < count; st++) {
		v4 next[16]; memcpy(next, regs, sizeof(regs));
		for (int portion = 0; portion < 2; portion++) {
			DWORD inputs = s[(portion ? D3DRS_PSALPHAINPUTS0 : D3DRS_PSRGBINPUTS0) + st];
			DWORD outputs = s[(portion ? D3DRS_PSALPHAOUTPUTS0 : D3DRS_PSRGBOUTPUTS0) + st];
			unsigned flags = outputs >> 12;
			int channels = portion ? 1 : 3, first = portion ? 3 : 0;
			float ab[4], cd[4], sum[4], mapped[4][4];
			/* the inputs, mapped, in every channel the portion has */
			for (int ch = first; ch < first + channels; ch++) {
				for (int k = 0; k < 4; k++) {
					unsigned byte = (inputs >> (24 - 8 * k)) & 0xff;
					v4 r = reg_value(s, byte & 15, st, regs);
					float x;
					if (portion) x = (byte & 0x10) ? r.c[3] : r.c[2];
					else x = (byte & 0x10) ? r.c[3] : r.c[ch];
					mapped[k][ch] = map_input(x, byte & 0xe0);
				}
			}
			for (int ch = first; ch < first + channels; ch++) {
				float v[4];
				for (int k = 0; k < 4; k++)
					v[k] = mapped[k][ch];
				ab[ch] = v[0] * v[1]; cd[ch] = v[2] * v[3];
				/* (dot products: over red, green and blue; saturating at 1,
				as the TEV's sum of the three terms does) */
				if (!portion && (flags & 2)) {
					float dot = mapped[0][0] * mapped[1][0] + mapped[0][1] * mapped[1][1] + mapped[0][2] * mapped[1][2];
					ab[ch] = dot > 1 ? 1 : dot < -1 ? -1 : dot;
				}
				if (!portion && (flags & 1)) {
					float dot = mapped[2][0] * mapped[3][0] + mapped[2][1] * mapped[3][1] + mapped[2][2] * mapped[3][2];
					cd[ch] = dot > 1 ? 1 : dot < -1 ? -1 : dot;
				}
				if (flags & 4) { float m = regs[12].c[3]; sum[ch] = (1 - m) * ab[ch] + m * cd[ch]; }
				else sum[ch] = ab[ch] + cd[ch];
			}
			int abn = (outputs >> 4) & 15, cdn = outputs & 15, sumn = (outputs >> 8) & 15;
			for (int ch = first; ch < first + channels; ch++) {
				if (abn >= 4 && abn != 6 && abn != 7) next[abn].c[ch] = out_map(ab[ch], flags);
				if (cdn >= 4 && cdn != 6 && cdn != 7) next[cdn].c[ch] = out_map(cd[ch], flags);
				if (sumn >= 4 && sumn != 6 && sumn != 7) next[sumn].c[ch] = out_map(sum[ch], flags);
			}
		}
		memcpy(regs, next, sizeof(regs));
	}
	DWORD abcd = s[D3DRS_PSFINALCOMBINERINPUTSABCD], efg = s[D3DRS_PSFINALCOMBINERINPUTSEFG];
	if (!abcd && !efg) return regs[12];
	v4 result;
	/* ef and v1r0 */
	v4 ef = {{0}}, sumv = {{0}};
	unsigned settings = efg & 0xff;
	for (int ch = 0; ch < 3; ch++) {
		unsigned e = (efg >> 24) & 0xff, f = (efg >> 16) & 0xff;
		v4 re = reg_value(s, e & 15, -1, regs), rf = reg_value(s, f & 15, -1, regs);
		float xe = clamp01((e & 0x10) ? re.c[3] : re.c[ch]), xf = clamp01((f & 0x10) ? rf.c[3] : rf.c[ch]);
		if (e & 0x20)
			xe = 1 - xe;
		if (f & 0x20)
			xf = 1 - xf;
		ef.c[ch] = xe * xf;
		float a = clamp01(regs[5].c[ch]), b = clamp01(regs[12].c[ch]);
		if (settings & 0x40)
			a = 1 - a;
		if (settings & 0x20)
			b = 1 - b;
		sumv.c[ch] = clamp01(a + b);
	}
	regs[14] = sumv; regs[15] = ef;
	for (int ch = 0; ch < 3; ch++) {
		float v[4];
		for (int k = 0; k < 4; k++) {
			unsigned byte = (abcd >> (24 - 8 * k)) & 0xff;
			int reg = byte & 15;
			v4 r = (reg == 14 || reg == 15) ? regs[reg] : reg_value(s, reg, -1, regs);
			float x = clamp01((byte & 0x10) ? r.c[3] : r.c[ch]);
			if (byte & 0x20) x = 1 - x;
			v[k] = x;
		}
		result.c[ch] = clamp01(v[0] * v[1] + (1 - v[0]) * v[2] + v[3]);
		if (getenv("SIM_TRACE")) printf("  ref final ch%d: A %.3f B %.3f C %.3f D %.3f\n", ch, v[0], v[1], v[2], v[3]);
	}
	{
		unsigned g = (efg >> 8) & 0xff;
		v4 r = reg_value(s, g & 15, -1, regs);
		float x = clamp01((g & 0x10) ? r.c[3] : r.c[2]);
		if (g & 0x20) x = 1 - x;
		result.c[3] = x;
	}
	return result;
}

/* ---------- the TEV simulator */
static float konst_sel(const struct gx_psh_program *p, unsigned sel, int ch, int alpha_half)
{
	static const float fractions[8] = { 1, 7/8.f, 6/8.f, 5/8.f, 4/8.f, 3/8.f, 2/8.f, 1/8.f };
	if (sel < 8) return fractions[sel];
	if (sel >= 0x0c && sel <= 0x0f && !alpha_half) { v4 k = argb_to_v4(p->konst[sel - 0x0c]); return k.c[ch]; }
	if (sel >= 0x10) { int which = (sel - 0x10) / 4, slot = (sel - 0x10) % 4; v4 k = argb_to_v4(p->konst[slot]); return k.c[which]; }
	return 0;
}
/* gx_psh_initialize's swap tables: as it is, red, green, the blue in the alpha */
static v4 swapped(v4 value, unsigned table)
{
	v4 out = value;
	switch (table) {
	case 1: out.c[0] = out.c[1] = out.c[2] = out.c[3] = value.c[0]; break;
	case 2: out.c[0] = out.c[1] = out.c[2] = out.c[3] = value.c[1]; break;
	case 3: out.c[3] = value.c[2]; break;
	default: break;
	}
	return out;
}

static v4 simulate(const struct gx_psh_program *p, const struct inputs_set *in, v4 *maps /* 9: 8 the fog ramp */)
{
	v4 regs[4]; memset(regs, 0, sizeof(regs));
	/* (constants the program set in TEV registers) */
	for (unsigned i = 0; i < p->preload_count; i++)
		regs[p->preload_register[i]] = argb_to_v4(p->preload_color[i]);
	for (unsigned i = 0; i < p->stage_count; i++) {
		const struct gx_psh_stage *st = &p->stages[i];
		v4 tex = {{0}}, ras = {{0}};
		if (st->texture_map != 0xFF) tex = maps[st->texture_map];
		tex = swapped(tex, st->texture_swap);
		if (st->ras_channel != 0xFF) ras = st->ras_channel ? in->v1 : in->v0;
		ras = swapped(ras, st->ras_swap);
		v4 out = regs[0]; v4 nregs[4]; memcpy(nregs, regs, sizeof(regs));
		for (int ch = 0; ch < 3; ch++) {
			float v[4];
			for (int k = 0; k < 4; k++) {
				unsigned code = st->color_in[k];
				float x;
				switch (code) {
				case 0: x = regs[0].c[ch]; break; case 1: x = regs[0].c[3]; break;
				case 2: x = regs[1].c[ch]; break; case 3: x = regs[1].c[3]; break;
				case 4: x = regs[2].c[ch]; break; case 5: x = regs[2].c[3]; break;
				case 6: x = regs[3].c[ch]; break; case 7: x = regs[3].c[3]; break;
				case 8: x = tex.c[ch]; break; case 9: x = tex.c[3]; break;
				case 10: x = ras.c[ch]; break; case 11: x = ras.c[3]; break;
				case 12: x = 1; break; case 13: x = 0.5f; break;
				case 14: x = konst_sel(p, st->konst_color, ch, 0); break;
				default: x = 0; break;
				}
				v[k] = (k < 3) ? clamp01(x) : x;
			}
			float r = v[3] + (1 - v[2]) * v[0] + v[2] * v[1];
			if (st->color_op == 1) r = v[3] - ((1 - v[2]) * v[0] + v[2] * v[1]);
			r += st->color_bias == 1 ? 0.5f : st->color_bias == 2 ? -0.5f : 0;
			r *= st->color_scale == 1 ? 2 : st->color_scale == 2 ? 4 : st->color_scale == 3 ? 0.5f : 1;
			nregs[st->color_dest].c[ch] = clamp01(r);
		}
		{
			float v[4];
			for (int k = 0; k < 4; k++) {
				unsigned code = st->alpha_in[k];
				float x;
				switch (code) {
				case 0: x = regs[0].c[3]; break; case 1: x = regs[1].c[3]; break;
				case 2: x = regs[2].c[3]; break; case 3: x = regs[3].c[3]; break;
				case 4: x = tex.c[3]; break; case 5: x = ras.c[3]; break;
				case 6: x = konst_sel(p, st->konst_alpha, 3, 1); break;
				default: x = 0; break;
				}
				v[k] = (k < 3) ? clamp01(x) : x;
			}
			float r = v[3] + (1 - v[2]) * v[0] + v[2] * v[1];
			if (st->alpha_op == 1) r = v[3] - ((1 - v[2]) * v[0] + v[2] * v[1]);
			r += st->alpha_bias == 1 ? 0.5f : st->alpha_bias == 2 ? -0.5f : 0;
			r *= st->alpha_scale == 1 ? 2 : st->alpha_scale == 2 ? 4 : st->alpha_scale == 3 ? 0.5f : 1;
			nregs[st->alpha_dest].c[3] = clamp01(r);
		}
		(void)out;
		memcpy(regs, nregs, sizeof(regs));
		if (getenv("SIM_TRACE")) { printf("  tev %u:", i); for (int r = 0; r < 4; r++) printf(" [%.3f %.3f %.3f %.3f]", regs[r].c[0], regs[r].c[1], regs[r].c[2], regs[r].c[3]); printf("\n"); }
	}
	return regs[0];
}

/* ---------- programs */
static unsigned rand_input(int alpha_portion)
{
	static const unsigned regs[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 12, 13 };
	static const unsigned maps[] = { 0x00, 0x20, 0x40, 0x60, 0x80, 0xa0, 0xc0, 0xe0 };
	unsigned reg = regs[rand() % 11];
	unsigned alpha = (rand() % 3 == 0) ? 0x10 : 0;
	if (alpha_portion) alpha = 0x10; /* (blue reads of registers are approximate) */
	return reg | alpha | maps[rand() % 8];
}
static unsigned rand_output(void)
{
	static const unsigned names[] = { 0, 0, 0, 12, 13, 8, 9, 4 };
	return names[rand() % 8];
}
static int check(const DWORD *state, unsigned present, const char *name, int verbose)
{
	struct gx_psh_textures textures; struct gx_psh_program program;
	memset(&textures, 0, sizeof(textures)); textures.present = present;
	textures.fog = (unsigned char)fog_on;
	textures.fog_color = fog_color & 0x00FFFFFFUL;
	if (!gx_psh_translate(state, &textures, &program) || program.approximate) { if (verbose) printf("%s: not translated (%s)\n", name, program.reason); return -1; }
	float worst = 0;
	for (int trial = 0; trial < 50; trial++) {
		struct inputs_set in; v4 maps[9];
		for (int i = 0; i < 4; i++) in.t[i] = rand4();
		in.t[0].c[3] = rand() % 2; /* (r0.a starts as t0.a: binary, for the mux) */
		in.v0 = rand4(); in.v1 = rand4();
		memset(maps, 0, sizeof(maps));
		fog_factor = frand();
		for (int ch = 0; ch < 4; ch++) maps[8].c[ch] = fog_factor;
		for (int i = 0; i < 4; i++) maps[i] = in.t[i];
		v4 want = reference(state, &in, present), got = simulate(&program, &in, maps);
		for (int ch = 0; ch < 4; ch++) { float d = fabsf(want.c[ch] - got.c[ch]); if (d > worst) worst = d; }
		if (worst > 0.01f) {
			if (verbose) { printf("  v0 %.3f %.3f %.3f %.3f v1 %.3f %.3f %.3f %.3f t0 %.3f %.3f %.3f %.3f\n", in.v0.c[0], in.v0.c[1], in.v0.c[2], in.v0.c[3], in.v1.c[0], in.v1.c[1], in.v1.c[2], in.v1.c[3], in.t[0].c[0], in.t[0].c[1], in.t[0].c[2], in.t[0].c[3]);
				for (unsigned k = 0; k < program.konst_count; k++) printf("  K%u %08x\n", k, program.konst[k]);
				printf("  c0 %08x c1 %08x f0 %08x f1 %08x\n", state[D3DRS_PSCONSTANT0_0], state[D3DRS_PSCONSTANT1_0], state[D3DRS_PSFINALCOMBINERCONSTANT0], state[D3DRS_PSFINALCOMBINERCONSTANT1]); }
			if (verbose) printf("%s: MISMATCH want %.3f %.3f %.3f %.3f got %.3f %.3f %.3f %.3f (%u stages)\n", name,
				want.c[0], want.c[1], want.c[2], want.c[3], got.c[0], got.c[1], got.c[2], got.c[3], program.stage_count);
			return 0;
		}
	}
	return 1;
}
/* two of the game's (its main menu's), which must translate */
static int game_programs(void)
{
	static const DWORD a_ri[] = {0x0a020a01,0x04200c01,0x3c201c01,0x00a02009,0x0b05040c,0x022014e1,0x08090809,0x08040b1d};
	static const DWORD a_ro[] = {0x000030cd,0x00000400,0x00000c00,0x00000900,0x000000b4,0x00000d00,0x00000800,0x00000c00};
	static const DWORD a_ai[] = {0x0a200000,0x1c200000,0x0c200d15,0,0,0,0,0};
	static const DWORD a_ao[] = {0x000000c0,0x00000090,0x000000cd,0,0,0,0,0};
	static const DWORD b_ri[] = {0x08010902,0x0a010804,0x1920b820,0,0,0x1ca01da0,0x0c010a04};
	static const DWORD b_ro[] = {0x00000089,0x000000ac,0x00000c00,0,0,0x00000c00,0x00000c00};
	static const DWORD b_ai[] = {0x18111912,0x1a111814,0x0820a920,0x1c1c0c0c,0x00005c5c,0,0x1c201a20};
	static const DWORD b_ao[] = {0x00000089,0x000000ac,0x00000c00,0x00024c00,0x00004d00,0x00000c00,0x00000c00};
	struct { DWORD count, abcd, efg; const DWORD *ri, *ro, *ai, *ao; } programs[2] = {
		{ 0x00011008, 0x340f010d, 0x0c111800, a_ri, a_ro, a_ai, a_ao },
		{ 0x00011107, 0x0000000c, 0x00001c00, b_ri, b_ro, b_ai, b_ao },
	};
	int failures = 0;

	for (int index = 0; index < 2; index++)
	{
		for (unsigned separate = 0; separate < 8; separate++)
		{
			DWORD state[200];
			struct gx_psh_textures textures;
			struct gx_psh_program program;

			memset(state, 0, sizeof(state));
			memset(&textures, 0, sizeof(textures));
			textures.present = 7;
			textures.separate_alpha = separate;
			state[D3DRS_PSCOMBINERCOUNT] = programs[index].count;
			/* (the constants the menu gives the first: a channel each to pick) */
			if (index == 0)
			{
				/* (the menu's planet: its constants, as the game sets them) */
				static const DWORD c0[8] = { 0x00ff0000, 0x00ffffff, 0x00ffffff, 0, 0, 0, 0, 0 };
				static const DWORD c1[8] = { 0x0000ff00, 0, 0, 0, 0, 0, 0, 0 };

				for (unsigned constant = 0; constant < 8; constant++)
				{
					state[D3DRS_PSCONSTANT0_0 + constant] = c0[constant];
					state[D3DRS_PSCONSTANT1_0 + constant] = c1[constant];
				}
				state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0xffffffff;
			}
			state[D3DRS_PSFINALCOMBINERINPUTSABCD] = programs[index].abcd;
			state[D3DRS_PSFINALCOMBINERINPUTSEFG] = programs[index].efg;
			for (unsigned stage = 0; stage < (programs[index].count & 0xff); stage++)
			{
				state[D3DRS_PSRGBINPUTS0 + stage] = programs[index].ri[stage];
				state[D3DRS_PSRGBOUTPUTS0 + stage] = programs[index].ro[stage];
				state[D3DRS_PSALPHAINPUTS0 + stage] = programs[index].ai[stage];
				state[D3DRS_PSALPHAOUTPUTS0 + stage] = programs[index].ao[stage];
			}
			if (getenv("DUMP_GAME") && atoi(getenv("DUMP_GAME")) == index * 8 + (int)separate)
			{
				gx_psh_translate(state, &textures, &program);
				printf("  %u stages (%s)\n", program.stage_count, program.reason ? program.reason : "ok");
				for (unsigned i = 0; i < program.stage_count; i++)
				{
					const struct gx_psh_stage *q = &program.stages[i];

					printf("  tev %2u: map %2d ras %2d sw %d/%d kc %02x ka %02x | C %2d %2d %2d %2d b%d s%d op%d -> %d | A %d %d %d %d b%d s%d op%d -> %d\n",
						i, q->texture_map == 255 ? -1 : q->texture_map, q->ras_channel == 255 ? -1 : q->ras_channel,
						q->texture_swap, q->ras_swap, q->konst_color, q->konst_alpha, q->color_in[0], q->color_in[1],
						q->color_in[2], q->color_in[3], q->color_bias, q->color_scale, q->color_op, q->color_dest,
						q->alpha_in[0], q->alpha_in[1], q->alpha_in[2], q->alpha_in[3], q->alpha_bias, q->alpha_scale,
						q->alpha_op, q->alpha_dest);
				}
			}
			if (!gx_psh_translate(state, &textures, &program))
			{
				printf("game program %d (separate alphas %x): not translated (%s)\n", index, separate, program.reason);
				failures++;
			}
			else if (separate == 0)
			{
				printf("game program %d: %u stages, %s\n", index, program.stage_count,
					program.approximate ? "approximate" : "exact");
			}
		}
	}
	return failures;
}

int main(int argc, char **argv)
{
	int seed_count = argc > 1 ? atoi(argv[1]) : 2000;
	int only = argc > 2 ? atoi(argv[2]) : -1;
	int ok = 0, bad = 0, untranslated = 0;
	for (int seed = 0; seed < seed_count; seed++) {
		DWORD state[200]; memset(state, 0, sizeof(state));
		srand(seed);
		fog_on = rand() % 2;
		fog_color = (DWORD)rand() * 2654435761u;
		int count = 1 + rand() % 4;
		state[D3DRS_PSCOMBINERCOUNT] = count | (rand() % 2 ? 0x1000 : 0) | (rand() % 2 ? 0x10000 : 0);
		for (int i = 0; i < 8; i++) { state[D3DRS_PSCONSTANT0_0 + i] = (DWORD)rand() * 2654435761u; state[D3DRS_PSCONSTANT1_0 + i] = (DWORD)rand() * 40503u * 65537u; }
		state[D3DRS_PSFINALCOMBINERCONSTANT0] = (DWORD)rand() * 2246822519u;
		state[D3DRS_PSFINALCOMBINERCONSTANT1] = (DWORD)rand() * 3266489917u;
		for (int st = 0; st < count; st++) {
			for (int portion = 0; portion < 2; portion++) {
				DWORD in = 0;
				for (int k = 0; k < 4; k++) in = (in << 8) | rand_input(portion);
				unsigned ab = rand_output(), cd = rand_output(), sum = rand_output();
				if (ab && ab == cd) cd = 0;
				if (sum && (sum == ab || sum == cd)) sum = 0;
				static const unsigned mapflags[] = { 0, 0, 0x08, 0x10, 0x18, 0x20, 0x30 };
				unsigned flags = mapflags[rand() % 7] | (sum && rand() % 4 == 0 ? 0x04 : 0) |
					(!portion && rand() % 5 == 0 ? 0x02 : 0) | (!portion && rand() % 5 == 0 ? 0x01 : 0);
				DWORD out = (flags << 12) | (sum << 8) | (ab << 4) | cd;
				state[(portion ? D3DRS_PSALPHAINPUTS0 : D3DRS_PSRGBINPUTS0) + st] = in;
				state[(portion ? D3DRS_PSALPHAOUTPUTS0 : D3DRS_PSRGBOUTPUTS0) + st] = out;
			}
		}
		if (rand() % 3) {
			DWORD abcd = 0;
			for (int k = 0; k < 4; k++) { static const unsigned r[] = { 0, 1, 2, 3, 4, 5, 8, 9, 12, 13, 14, 15 }; abcd = (abcd << 8) | r[rand() % 12] | (rand() % 3 == 0 ? 0x10 : 0) | (rand() % 3 == 0 ? 0x20 : 0); }
			unsigned e = (unsigned[]){ 0, 1, 4, 8, 12 }[rand() % 5], f = (unsigned[]){ 0, 2, 5, 9, 13 }[rand() % 5];
			unsigned g = (unsigned[]){ 0x1c, 0x18, 0x14, 0x1d, 0x11 }[rand() % 5] | (rand() % 3 == 0 ? 0x20 : 0);
			state[D3DRS_PSFINALCOMBINERINPUTSABCD] = abcd;
			state[D3DRS_PSFINALCOMBINERINPUTSEFG] = (e << 24) | (f << 16) | (g << 8) | ((rand() % 2) ? 0x20 : 0) | ((rand() % 2) ? 0x40 : 0);
		}
		char name[32]; snprintf(name, sizeof(name), "seed %d", seed);
		if (only >= 0 && seed != only) continue;
		if (only >= 0) {
			printf("count %08x final %08x %08x\n", state[D3DRS_PSCOMBINERCOUNT], state[D3DRS_PSFINALCOMBINERINPUTSABCD], state[D3DRS_PSFINALCOMBINERINPUTSEFG]);
			for (int st = 0; st < count; st++) printf("  %d: rgb %08x -> %08x  alpha %08x -> %08x\n", st, state[D3DRS_PSRGBINPUTS0+st], state[D3DRS_PSRGBOUTPUTS0+st], state[D3DRS_PSALPHAINPUTS0+st], state[D3DRS_PSALPHAOUTPUTS0+st]);
			struct gx_psh_textures tx; struct gx_psh_program pg; memset(&tx, 0, sizeof(tx)); tx.present = 7;
			gx_psh_translate(state, &tx, &pg);
			for (unsigned i = 0; i < pg.stage_count; i++) { const struct gx_psh_stage *q = &pg.stages[i];
				printf("  tev %u: map %d ras %d kc %02x ka %02x | C %d %d %d %d b%d s%d -> %d | A %d %d %d %d b%d s%d -> %d\n", i, q->texture_map == 255 ? -1 : q->texture_map, q->ras_channel == 255 ? -1 : q->ras_channel, q->konst_color, q->konst_alpha,
					q->color_in[0], q->color_in[1], q->color_in[2], q->color_in[3], q->color_bias, q->color_scale, q->color_dest,
					q->alpha_in[0], q->alpha_in[1], q->alpha_in[2], q->alpha_in[3], q->alpha_bias, q->alpha_scale, q->alpha_dest); }
		}
		int r = check(state, 0x7, name, bad < 6);
		if (r > 0) ok++; else if (r == 0) bad++; else untranslated++;
	}
	printf("%d agree, %d differ, %d not translated\n", ok, bad, untranslated);
	{
		int game = only < 0 ? game_programs() : 0;

		if (only < 0)
			printf("the game's programs: %s\n", game ? "NOT ALL TRANSLATED" : "translated");
		return bad != 0 || game != 0;
	}
}
