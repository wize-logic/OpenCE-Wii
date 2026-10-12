/*
WII_BITMAP_TEXEL.C

One texel of a bitmap as the game's processor reads it (bitmap_2d_get_pixel,
source/bitmaps/bitmaps.c): the lightmap under an object, which lights it
(object_lights.c's sample_lightmap), and the diffuse texture there. The map
converter rewrote the bitmaps' pixels in GX's formats and tiles
(tools/wii_gx_textures.py), so the Xbox's swizzled addressing reads another
texel of them (on a lightmap, often the black between its pieces: the
objects were drawn nearly black). This finds the texel in the GX texture; where a texel sits in
GX's tiles and what the hardware makes of it is WiiLibs' (wl_gxc_texel_raw and
wl_gxc_texel_read, wl/codec/gx_tex.h), and what is left here is the game's: its
bitmap formats, its levels, the Xbox's word:

- wii_bitmap_gx_levels: the levels of a bitmap the converter rewrote, 0 for
  one it left as it was (tools/wii_convert_maps.py's bitmap_pixels and
  wii_gx_textures.py's gx_levels, which d3d8_gx.c's gx_native_levels also
  follows);
- wii_bitmap_gx_texel: a texel of a level, as the Xbox's texel (its word,
  for the game's own bitmap_format_to_a8r8g8b8) or, for DXT, as A8R8G8B8
  decoded as the GPU decodes CMPR (WiiLibs' wl_gxc_cmpr_palette) with the
  alpha of DXT3 and DXT5 from the I4 levels that follow the colour's.

Checked on the host against the Xbox textures it converts
(port/wii/tests/gx_texel_check.c, tools/wii_gx_texel_check.py).
*/

#include "wii_bitmap_texel.h"

#include "wl/codec/gx_tex.h"

#include <stddef.h>
#include <stdint.h>

/* the bitmaps' formats (source/bitmaps/bitmaps.c's order) and flags */
enum
{
	BITMAP_A8 = 0, BITMAP_Y8 = 1, BITMAP_AY8 = 2, BITMAP_A8Y8 = 3, BITMAP_R5G6B5 = 6, BITMAP_A1R5G5B5 = 8,
	BITMAP_A4R4G4B4 = 9, BITMAP_X8R8G8B8 = 10, BITMAP_A8R8G8B8 = 11, BITMAP_DXT1 = 14, BITMAP_DXT3 = 15,
	BITMAP_DXT5 = 16, BITMAP_P8 = 17,
	BITMAP_TYPE_2D = 0,
	BITMAP_POWER_OF_TWO_BIT = 0, BITMAP_COMPRESSED_BIT = 1, BITMAP_LINEAR_BIT = 4, BITMAP_V16U16_BIT = 5,
	GX_MAXIMUM_DIMENSION = 1024,
};

/* a bitmap format's GX format (wii_gx_textures.py's GX_TILES: WiiLibs' wl_xbox_gx_format), and how many
GX levels of that format its bytes are: DXT3 and DXT5 are a CMPR level for the colour and an I4 level of the
same size for the alpha. FALSE for a format not converted. */
static int gx_format(int format, WlGxcFormat *gx, int *parts)
{
	*parts = 1;
	switch (format)
	{
	case BITMAP_A8R8G8B8: case BITMAP_X8R8G8B8: *gx = WL_GXC_RGBA8; return 1;
	case BITMAP_R5G6B5: *gx = WL_GXC_RGB565; return 1;
	case BITMAP_A1R5G5B5: case BITMAP_A4R4G4B4: *gx = WL_GXC_RGB5A3; return 1;
	case BITMAP_A8Y8: *gx = WL_GXC_IA8; return 1;
	case BITMAP_A8: case BITMAP_Y8: case BITMAP_AY8: *gx = WL_GXC_I8; return 1;
	case BITMAP_P8: *gx = WL_GXC_C8; return 1;
	case BITMAP_DXT1: *gx = WL_GXC_CMPR; return 1;
	case BITMAP_DXT3: case BITMAP_DXT5: *gx = WL_GXC_CMPR; *parts = 2; return 1;
	default: return 0;
	}
}

static int is_dxt(int format)
{
	return format == BITMAP_DXT1 || format == BITMAP_DXT3 || format == BITMAP_DXT5;
}

static int level_dimension(int base, int level)
{
	int value = base >> level;

	return value > 1 ? value : 1;
}

/* bytes of an Xbox level (non-linear: swizzled, or DXT's blocks) */
static long xbox_level_bytes(int format, int width, int height, int level)
{
	int w = level_dimension(width, level), h = level_dimension(height, level);

	switch (format)
	{
	case BITMAP_DXT1: return (long)((w + 3) / 4) * ((h + 3) / 4) * 8;
	case BITMAP_DXT3: case BITMAP_DXT5: return (long)((w + 3) / 4) * ((h + 3) / 4) * 16;
	case BITMAP_A8R8G8B8: case BITMAP_X8R8G8B8: return (long)w * h * 4;
	case BITMAP_R5G6B5: case BITMAP_A1R5G5B5: case BITMAP_A4R4G4B4: case BITMAP_A8Y8: return (long)w * h * 2;
	default: return (long)w * h;
	}
}

/* bytes of a GX level: whole tiles (for DXT3/5, the CMPR level and the I4 level that both take as many bytes) */
static long gx_level_bytes(WlGxcFormat gx, int parts, int width, int height, int level)
{
	return (long)wl_gxc_tex_bytes(gx, level_dimension(width, level), level_dimension(height, level)) * parts;
}

/* bytes of a level's colour: for DXT3/5 the CMPR half, its alpha's I4 level coming after all the colour's */
static long gx_colour_level_bytes(WlGxcFormat gx, int width, int height, int level)
{
	return gx_level_bytes(gx, 1, width, height, level);
}

int wii_bitmap_gx_levels(int format, int type, int width, int height, int depth, unsigned flags, int mipmap_count)
{
	WlGxcFormat gx;
	int parts, levels = 1, level, kept = 0;
	long budget = 0, used = 0;

	if (!gx_format(format, &gx, &parts) || type != BITMAP_TYPE_2D || (flags >> BITMAP_V16U16_BIT & 1) || width <= 0 ||
		height <= 0 || (flags >> BITMAP_LINEAR_BIT & 1))
	{
		return 0;
	}
	if (flags >> BITMAP_POWER_OF_TWO_BIT & 1)
	{
		int largest = (flags >> BITMAP_COMPRESSED_BIT & 1) ? width / 4 : width;
		int bits = 0;

		if (flags >> BITMAP_COMPRESSED_BIT & 1)
		{
			if (height / 4 > largest)
				largest = height / 4;
		}
		else if (height > largest)
		{
			largest = height;
		}
		if (depth > largest)
			largest = depth;
		if (largest < 1)
			largest = 1;
		while (largest >> bits)
			bits++;
		levels = (mipmap_count < bits - 1 ? mipmap_count : bits - 1) + 1;
	}
	if (width > GX_MAXIMUM_DIMENSION || height > GX_MAXIMUM_DIMENSION)
		return 0;
	for (level = 0; level < levels; level++)
		budget += xbox_level_bytes(format, width, height, level);
	for (level = 0; level < levels; level++)
	{
		used += gx_level_bytes(gx, parts, width, height, level);
		if (used > budget)
			break;
		kept = level + 1;
	}
	return kept;
}

/* a GX RGB5A3 texel back to the Xbox's word it was made from (WiiLibs'
wl_xbox_to_gx, xbox_tex.h, made it so) */
static unsigned long xbox_word_from_rgb5a3(int format, unsigned value)
{
	if (format == BITMAP_A1R5G5B5)
	{
		if (value & 0x8000)
			return value;
		/* (alpha 0, the top four bits of each colour) */
		return ((value >> 8 & 15) << 11) | ((value >> 4 & 15) << 6) | ((value & 15) << 1);
	}
	if (value & 0x8000)
		return 0xF000 | ((value >> 11 & 15) << 8) | ((value >> 6 & 15) << 4) | (value >> 1 & 15);
	/* (the top three bits of alpha) */
	{
		unsigned alpha = value >> 12 & 7;

		return ((alpha << 1 | alpha >> 2) << 12) | (value & 0x0FFF);
	}
}

int wii_bitmap_gx_texel(int format, int width, int height, int levels, const void *pixels, int level, int x, int y,
	unsigned long *value)
{
	const unsigned char *base = pixels;
	WlGxcFormat gx;
	int parts, w, h, index;
	long offset = 0, level_bytes;
	uint32_t raw;

	if (!pixels || !gx_format(format, &gx, &parts) || level < 0 || level >= levels)
		return WII_BITMAP_TEXEL_NONE;
	w = level_dimension(width, level);
	h = level_dimension(height, level);
	if (x < 0 || y < 0 || x >= w || y >= h)
		return WII_BITMAP_TEXEL_NONE;
	for (index = 0; index < level; index++)
		offset += gx_colour_level_bytes(gx, width, height, index);
	level_bytes = gx_colour_level_bytes(gx, width, height, level);
	if (is_dxt(format))
	{
		/* CMPR: the colour is WiiLibs' (the hardware's blend) */
		unsigned char colour[4];
		unsigned alpha;

		if (!wl_gxc_texel_read(gx, base + offset, (size_t)level_bytes, (uint32_t)w, (uint32_t)h, (uint32_t)x, (uint32_t)y,
			colour))
			return WII_BITMAP_TEXEL_NONE;
		alpha = colour[3];
		if (format != BITMAP_DXT1)
		{
			/* (the alpha's I4 levels follow all the colour's, the same sizes: the level's own comes
			after the colour's levels and the earlier alpha levels) */
			long alpha_offset = offset;

			for (index = 0; index < levels; index++)
				alpha_offset += gx_colour_level_bytes(gx, width, height, index);
			if (!wl_gxc_texel_raw(WL_GXC_I4, base + alpha_offset, (size_t)level_bytes, (uint32_t)w, (uint32_t)h, (uint32_t)x,
				(uint32_t)y, &raw))
				return WII_BITMAP_TEXEL_NONE;
			alpha = raw * 17;
		}
		*value = (unsigned long)alpha << 24 | (unsigned long)colour[0] << 16 | (unsigned long)colour[1] << 8 | colour[2];
		return WII_BITMAP_TEXEL_A8R8G8B8;
	}
	/* the texel as GX stores it: the Xbox's word, unless it is RGB5A3 (re-packed bit for bit by
	wl_xbox_to_gx, which this undoes) */
	if (!wl_gxc_texel_raw(gx, base + offset, (size_t)level_bytes, (uint32_t)w, (uint32_t)h, (uint32_t)x, (uint32_t)y, &raw))
		return WII_BITMAP_TEXEL_NONE;
	*value = gx == WL_GXC_RGB5A3 ? xbox_word_from_rgb5a3(format, raw) : raw;
	return WII_BITMAP_TEXEL_XBOX;
}
