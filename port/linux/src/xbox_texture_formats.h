/*
XBOX_TEXTURE_FORMATS.H

Xbox texture formats, independent of any renderer: what a texture's Format
and Size words describe, where its levels lie, and its texels decoded to
32-bit BGRA (xbox_texture_formats.c). The OpenGL texture cache
(xbox_textures.c) and the Wii's (port/wii) are built on it.
*/

#ifndef __HALO_LINUX_XBOX_TEXTURE_FORMATS_H
#define __HALO_LINUX_XBOX_TEXTURE_FORMATS_H

#include "platform.h"

enum texel_kind
{
	_texel_unknown,
	_texel_a8r8g8b8, _texel_x8r8g8b8, _texel_r5g6b5, _texel_a1r5g5b5, _texel_x1r5g5b5, _texel_a4r4g4b4,
	_texel_l8, _texel_al8, _texel_a8, _texel_a8l8, _texel_p8, _texel_g8b8, _texel_r8b8, _texel_r6g5b5,
	_texel_l16, _texel_v16u16, _texel_a8b8g8r8, _texel_b8g8r8a8, _texel_r8g8b8a8, _texel_r5g5b5a1,
	_texel_r4g4b4a4, _texel_yuy2, _texel_uyvy, _texel_d24s8, _texel_d16,
	_texel_dxt1, _texel_dxt3, _texel_dxt5,
};

struct format_information
{
	unsigned char kind;
	unsigned char bytes; /* per texel; per 4x4 block for DXT */
	unsigned char linear;
};

/* the kind and size of a D3DFMT_* texel (a 4x4 block's for DXT) */
struct format_information xgpu_texture_format_information(DWORD format);

struct xgpu_texture_description
{
	DWORD format;       /* D3DFMT_* */
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;        /* not swizzled; addressed with texel coordinates */
	BOOL compressed;
	unsigned long pitch; /* linear textures */
	BOOL hires;         /* a high-res HUD texture drawn in the texture's place (hud_hires.h) */
	BOOL hires_coverage; /* ... whose green is its coverage (a meter's) */
	BOOL hires_point_threshold; /* ... whose red holds discrete segment data */
};

void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description);
/* bytes of one face, mip levels included (cube faces are padded) */
unsigned long xgpu_texture_face_size(const struct xgpu_texture_description *description);
unsigned long xgpu_texture_level_offset(const struct xgpu_texture_description *description, unsigned long level);
unsigned long xgpu_texture_level_pitch(const struct xgpu_texture_description *description, unsigned long level);

/* a level's width, height or depth */
unsigned long xgpu_texture_level_dimension(unsigned long base, unsigned long level);
/* bytes of a level */
unsigned long xgpu_texture_level_bytes(const struct xgpu_texture_description *description, unsigned long level);
/* whether the size is one D3DDevice_GetDeviceCaps allows */
BOOL xgpu_texture_size_supported(const struct xgpu_texture_description *description);
/* one level (or 3D slice set) of an uncompressed texture into BGRA,
width * height * depth texels; FALSE when out of memory */
BOOL xgpu_texture_decode_level(const struct xgpu_texture_description *description, unsigned long level,
	const unsigned char *source, const D3DCOLOR *palette, unsigned long *destination);
/* one level of a DXT1, DXT3 or DXT5 texture (kind _texel_dxt*) into BGRA */
void xgpu_texture_dxt_decode_level(unsigned char kind, const unsigned char *source, unsigned long width,
	unsigned long height, unsigned long depth, unsigned long *destination);

#endif
