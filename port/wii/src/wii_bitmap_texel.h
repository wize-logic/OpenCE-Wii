/*
WII_BITMAP_TEXEL.H

A texel of a bitmap the map converter rewrote in GX's formats, for the
game's processor (wii_bitmap_texel.c).
*/

#ifndef WII_BITMAP_TEXEL_H
#define WII_BITMAP_TEXEL_H

enum
{
	WII_BITMAP_TEXEL_NONE = 0,
	/* the Xbox's texel: its byte, word or dword */
	WII_BITMAP_TEXEL_XBOX,
	/* decoded, as A8R8G8B8 (DXT) */
	WII_BITMAP_TEXEL_A8R8G8B8,
};

/* the levels the converter rewrote in GX's formats, 0 when it left the
bitmap as it was */
int wii_bitmap_gx_levels(int format, int type, int width, int height, int depth, unsigned flags, int mipmap_count);
/* a texel of a rewritten bitmap's level (the pixels are its first level's) */
int wii_bitmap_gx_texel(int format, int width, int height, int levels, const void *pixels, int level, int x, int y,
	unsigned long *value);

#endif
