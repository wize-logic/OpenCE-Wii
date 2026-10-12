/*
WII_BITMAP_TEXEL.C

One texel of a bitmap as the game's processor reads it (bitmap_2d_get_pixel,
source/bitmaps/bitmaps.c): the lightmap under an object, which lights it
(object_lights.c's sample_lightmap), and the diffuse texture there. The map
converter rewrote the bitmaps' pixels in GX's formats and tiles
(tools/wii_gx_textures.py), so the Xbox's swizzled addressing reads another
texel of them (on a lightmap, often the black between its pieces: the
objects were drawn nearly black). This finds the texel in the GX texture:

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

struct gx_tile
{
	int width, height, bytes;
};

/* a format's GX tile (wii_gx_textures.py's GX_TILES), FALSE for one not converted */
static int gx_tile(int format, struct gx_tile *tile)
{
	switch (format)
	{
	case BITMAP_A8R8G8B8: case BITMAP_X8R8G8B8:
		tile->width = 4; tile->height = 4; tile->bytes = 64; return 1;
	case BITMAP_R5G6B5: case BITMAP_A1R5G5B5: case BITMAP_A4R4G4B4: case BITMAP_A8Y8:
		tile->width = 4; tile->height = 4; tile->bytes = 32; return 1;
	case BITMAP_A8: case BITMAP_Y8: case BITMAP_AY8: case BITMAP_P8:
		tile->width = 8; tile->height = 4; tile->bytes = 32; return 1;
	case BITMAP_DXT1:
		tile->width = 8; tile->height = 8; tile->bytes = 32; return 1;
	case BITMAP_DXT3: case BITMAP_DXT5:
		tile->width = 8; tile->height = 8; tile->bytes = 64; return 1;
	default:
		return 0;
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

/* bytes of a GX level: whole tiles (for DXT3/5, the CMPR level and the I4
level that both take as many bytes) */
static long gx_level_bytes(const struct gx_tile *tile, int width, int height, int level)
{
	int w = level_dimension(width, level), h = level_dimension(height, level);

	return (long)((w + tile->width - 1) / tile->width) * ((h + tile->height - 1) / tile->height) * tile->bytes;
}

/* bytes of a level's colour: for DXT3/5 the CMPR half, its alpha's I4
level coming after all the colour's */
static long gx_colour_level_bytes(int format, const struct gx_tile *tile, int width, int height, int level)
{
	long bytes = gx_level_bytes(tile, width, height, level);

	return format == BITMAP_DXT3 || format == BITMAP_DXT5 ? bytes / 2 : bytes;
}

int wii_bitmap_gx_levels(int format, int type, int width, int height, int depth, unsigned flags, int mipmap_count)
{
	struct gx_tile tile;
	int levels = 1, level, kept = 0;
	long budget = 0, used = 0;

	if (!gx_tile(format, &tile) || type != BITMAP_TYPE_2D || (flags >> BITMAP_V16U16_BIT & 1) || width <= 0 ||
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
		used += gx_level_bytes(&tile, width, height, level);
		if (used > budget)
			break;
		kept = level + 1;
	}
	return kept;
}

static unsigned big16(const unsigned char *bytes)
{
	return (unsigned)bytes[0] << 8 | bytes[1];
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
	struct gx_tile tile;
	int w, h, tiles_per_row, index;
	long offset = 0, tile_offset;

	if (!pixels || !gx_tile(format, &tile) || level < 0 || level >= levels)
		return WII_BITMAP_TEXEL_NONE;
	w = level_dimension(width, level);
	h = level_dimension(height, level);
	if (x < 0 || y < 0 || x >= w || y >= h)
		return WII_BITMAP_TEXEL_NONE;
	for (index = 0; index < level; index++)
		offset += gx_colour_level_bytes(format, &tile, width, height, index);
	tiles_per_row = (w + tile.width - 1) / tile.width;
	tile_offset = offset + ((long)(y / tile.height) * tiles_per_row + x / tile.width) * (is_dxt(format) ? 32 : tile.bytes);
	switch (format)
	{
	case BITMAP_A8R8G8B8:
	case BITMAP_X8R8G8B8:
	{
		/* (the tile's 16 A,R pairs, then its 16 G,B pairs) */
		const unsigned char *texel = base + tile_offset + ((y & 3) * 4 + (x & 3)) * 2;

		*value = (unsigned long)texel[0] << 24 | (unsigned long)texel[1] << 16 | (unsigned long)texel[32] << 8 | texel[33];
		return WII_BITMAP_TEXEL_XBOX;
	}
	case BITMAP_R5G6B5:
	case BITMAP_A8Y8:
		/* (the Xbox's word, big-endian; IA8 is the alpha, then the intensity) */
		*value = big16(base + tile_offset + ((y & 3) * 4 + (x & 3)) * 2);
		return WII_BITMAP_TEXEL_XBOX;
	case BITMAP_A1R5G5B5:
	case BITMAP_A4R4G4B4:
		*value = xbox_word_from_rgb5a3(format, big16(base + tile_offset + ((y & 3) * 4 + (x & 3)) * 2));
		return WII_BITMAP_TEXEL_XBOX;
	case BITMAP_A8:
	case BITMAP_Y8:
	case BITMAP_AY8:
	case BITMAP_P8:
		*value = base[tile_offset + (y & 3) * 8 + (x & 7)];
		return WII_BITMAP_TEXEL_XBOX;
	default:
	{
		/* CMPR: an 8x8 tile is four 4x4 blocks, row by row; a block's
		endpoints are big-endian and its rows bytes, texel 0 in the top two
		bits */
		const unsigned char *block = base + tile_offset + ((y & 7) / 4 * 2 + (x & 7) / 4) * 8;
		unsigned char palette[4][4];
		unsigned colour_index = block[4 + (y & 3)] >> (6 - 2 * (x & 3)) & 3;
		unsigned alpha;

		wl_gxc_cmpr_palette(big16(block), big16(block + 2), palette);
		alpha = palette[colour_index][3];
		if (format != BITMAP_DXT1)
		{
			/* (the alpha's I4 levels follow all the colour's: 8x8 tiles of 32
			bytes, the even texel in the high nibble) */
			long alpha_offset = 0;
			unsigned nibble;

			for (index = 0; index < levels; index++)
				alpha_offset += gx_level_bytes(&tile, width, height, index) / 2;
			for (index = 0; index < level; index++)
				alpha_offset += gx_level_bytes(&tile, width, height, index) / 2;
			nibble = base[alpha_offset + ((long)(y / 8) * tiles_per_row + x / 8) * 32 + (y & 7) * 4 + (x & 7) / 2];
			nibble = (x & 1) ? nibble & 15 : nibble >> 4;
			alpha = nibble * 17;
		}
		*value = (unsigned long)alpha << 24 | (unsigned long)palette[colour_index][0] << 16 |
			(unsigned long)palette[colour_index][1] << 8 | palette[colour_index][2];
		return WII_BITMAP_TEXEL_A8R8G8B8;
	}
	}
}
