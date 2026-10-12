/*
GX_TEXEL_CHECK.C

The host check of port/wii/src/wii_bitmap_texel.c (tools/wii_gx_texel_check.py):
textures of every format the map converter rewrites, of random texels, are
converted as tools/wii_gx_textures.py converts them (WiiLibs' wl_xbox_to_gx,
wl_xbox_dxt_to_cmpr and wl_xbox_dxt_alpha_i4, level after level, the alpha's
I4 levels after all the colour's), and every texel of every level kept is
read back from the GX texture and compared with the Xbox's texel at the
same place: the same value (what the conversion keeps of it), or for DXT
the block decoded (within GX's 5/8 blend of S3TC's 2/3).
*/

#include "wii_bitmap_texel.h"

#include "wl/codec/gx_tex.h"
#include "wl/codec/xbox_tex.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct format
{
	const char *name;
	int bitmap_format;
	WlXboxFormat xbox;
	int bytes; /* per texel, 0 for DXT */
};

static const struct format formats[] =
{
	{ "a8r8g8b8", 11, WL_XBOX_A8R8G8B8, 4 }, { "x8r8g8b8", 10, WL_XBOX_X8R8G8B8, 4 },
	{ "r5g6b5", 6, WL_XBOX_R5G6B5, 2 }, { "a1r5g5b5", 8, WL_XBOX_A1R5G5B5, 2 },
	{ "a4r4g4b4", 9, WL_XBOX_A4R4G4B4, 2 }, { "l8", 1, WL_XBOX_L8, 1 }, { "a8", 0, WL_XBOX_A8, 1 },
	{ "al8", 2, WL_XBOX_AL8, 1 }, { "a8l8", 3, WL_XBOX_A8L8, 2 }, { "p8", 17, WL_XBOX_P8, 1 },
	{ "dxt1", 14, WL_XBOX_DXT1, 0 }, { "dxt3", 15, WL_XBOX_DXT3, 0 }, { "dxt5", 16, WL_XBOX_DXT5, 0 },
};

static uint32_t random_state = 12345;
static long checked_texels;

static uint32_t random_next(void)
{
	random_state = random_state * 1664525u + 1013904223u;
	return random_state >> 8;
}

static uint32_t spread(uint32_t value, uint32_t mask)
{
	uint32_t result = 0, bit;

	for (bit = 1; mask; bit <<= 1)
	{
		uint32_t low = mask & -mask;

		if (value & bit)
			result |= low;
		mask &= mask - 1;
	}
	return result;
}

static int dimension(int base, int level)
{
	return base >> level > 1 ? base >> level : 1;
}

/* what the conversion keeps of an Xbox texel */
static unsigned long kept(const struct format *format, unsigned long value)
{
	switch (format->bitmap_format)
	{
	case 10:
		return value | 0xFF000000ul;
	case 8:
		/* (alpha 0: the top four bits of each colour) */
		return value & 0x8000 ? value : value & 0x7BDE;
	case 9:
	{
		unsigned alpha = value >> 12 & 15;

		if (alpha == 15)
			return value;
		alpha >>= 1;
		return ((alpha << 1 | alpha >> 2) << 12) | (value & 0x0FFF);
	}
	default:
		return value;
	}
}

static int check(const struct format *format, int width, int height, int mipmap_count)
{
	unsigned flags = 1u << 0 | (format->bytes ? 0 : 1u << 1) | 1u << 3;
	int levels = mipmap_count + 1, level, x, y, failures = 0, kept_levels;
	size_t size = 0, offset, written = 0;
	unsigned char *xbox, *gx;

	for (level = 0; level < levels; level++)
		size += wl_xbox_level_bytes(format->xbox, dimension(width, level), dimension(height, level), 0);
	xbox = malloc(size);
	gx = calloc(1, size);
	for (offset = 0; offset < size; offset++)
		xbox[offset] = (unsigned char)random_next();
	kept_levels = wii_bitmap_gx_levels(format->bitmap_format, 0, width, height, 1, flags, mipmap_count);
	if (!kept_levels)
	{
		free(xbox);
		free(gx);
		return 0;
	}
	/* (converted as tools/wii_gx_textures.py does) */
	for (level = 0, offset = 0; level < kept_levels; level++)
	{
		int w = dimension(width, level), h = dimension(height, level);
		size_t bytes = wl_xbox_level_bytes(format->xbox, w, h, 0);

		written += format->bytes ? wl_xbox_to_gx(format->xbox, 1, 0, xbox + offset, bytes, w, h, gx + written, size - written) :
			wl_xbox_dxt_to_cmpr(format->xbox, xbox + offset, bytes, w, h, gx + written, size - written);
		offset += bytes;
	}
	if (format->xbox == WL_XBOX_DXT3 || format->xbox == WL_XBOX_DXT5)
	{
		for (level = 0, offset = 0; level < kept_levels; level++)
		{
			int w = dimension(width, level), h = dimension(height, level);
			size_t bytes = wl_xbox_level_bytes(format->xbox, w, h, 0);

			written += wl_xbox_dxt_alpha_i4(format->xbox, xbox + offset, bytes, w, h, gx + written, size - written);
			offset += bytes;
		}
	}
	for (level = 0, offset = 0; level < kept_levels; level++)
	{
		int w = dimension(width, level), h = dimension(height, level);
		uint32_t mask_x = 0, mask_y = 0;

		if (format->bytes)
			wl_xbox_swizzle_masks((uint32_t)w, (uint32_t)h, &mask_x, &mask_y);
		for (y = 0; y < h; y++)
		{
			for (x = 0; x < w; x++)
			{
				unsigned long value = 0, expected = 0;
				int result = wii_bitmap_gx_texel(format->bitmap_format, width, height, kept_levels, gx, level, x, y, &value);
				int good;

				checked_texels++;

				if (format->bytes)
				{
					const unsigned char *texel = xbox + offset +
						(spread((uint32_t)x, mask_x) | spread((uint32_t)y, mask_y)) * format->bytes;
					int byte;

					for (byte = format->bytes - 1; byte >= 0; byte--)
						expected = expected << 8 | texel[byte];
					expected = kept(format, expected);
					good = result == WII_BITMAP_TEXEL_XBOX && value == expected;
				}
				else
				{
					int block_bytes = format->xbox == WL_XBOX_DXT1 ? 8 : 16;
					const unsigned char *block = xbox + offset + ((size_t)(y / 4) * ((w + 3) / 4) + x / 4) * block_bytes;
					uint8_t pixels[16][4], alphas[16];
					const uint8_t *pixel = pixels[(y & 3) * 4 + (x & 3)];
					int channel;

					wl_xbox_dxt_decode_block(format->xbox, block, pixels);
					good = result == WII_BITMAP_TEXEL_A8R8G8B8;
					/* (DXT3/5 colour: four-colour mode, as GX shows it) */
					if (format->xbox == WL_XBOX_DXT1)
					{
						good &= (value >> 24) == pixel[3];
					}
					else
					{
						wl_xbox_dxt_alpha_block(format->xbox, block, alphas);
						good &= (value >> 24) == (unsigned long)(alphas[(y & 3) * 4 + (x & 3)] >> 4) * 17;
					}
					if (format->xbox == WL_XBOX_DXT1 && pixel[3] == 0)
						channel = 3;
					else
						channel = 0;
					for (; channel < 3; channel++)
					{
						int difference = (int)(value >> (16 - 8 * channel) & 255) - pixel[channel];

						good &= difference >= -12 && difference <= 12;
					}
					expected = (unsigned long)pixel[3] << 24 | (unsigned long)pixel[0] << 16 | (unsigned long)pixel[1] << 8 | pixel[2];
				}
				if (!good && failures++ < 4)
				{
					printf("%s %dx%d level %d (%d, %d): %d %08lx, expected %08lx\n", format->name, width, height, level, x, y,
						result, value, expected);
				}
			}
		}
		offset += wl_xbox_level_bytes(format->xbox, w, h, 0);
	}
	free(xbox);
	free(gx);
	return failures;
}

int main(void)
{
	static const int sizes[][3] = { { 256, 256, 0 }, { 64, 32, 6 }, { 32, 64, 3 }, { 8, 8, 3 }, { 16, 4, 2 }, { 4, 4, 0 } };
	size_t format_index, size_index;
	int failures = 0;

	for (format_index = 0; format_index < sizeof(formats) / sizeof(formats[0]); format_index++)
	{
		for (size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]); size_index++)
			failures += check(&formats[format_index], sizes[size_index][0], sizes[size_index][1], sizes[size_index][2]);
	}
	printf("gx_texel_check: %s (%ld texels read, %d differ)\n", failures ? "FAILED" : "passed", checked_texels, failures);
	return failures != 0;
}
