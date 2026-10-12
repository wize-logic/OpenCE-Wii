/*
XBOX_TEXTURES.C

The OpenGL texture cache. Xbox textures are decoded by
xbox_texture_formats.c: everything except DXT is converted to 32-bit BGRA
on upload.

A cached texture stays valid until any page it was read from is written;
memory_watch.c detects that by write-protecting the pages.
*/

#include "xgpu.h"
#include "hud_hires.h"
#include "menu_files.h"
#include "text_hires.h"
#include "port_config.h"
#include "../game/cache_file_formats.h"

#include <stdio.h>
#ifdef HALO_GLES
#define GL_BGRA GL_RGBA
#endif
#include <stdlib.h>
#include <string.h>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83f1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83f2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83f3
#endif

static GLenum compressed_format(unsigned char kind)
{
	switch (kind)
	{
	case _texel_dxt1: return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	case _texel_dxt3: return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
	default: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	}
}

/* ---------- upload */

/* debug.texture_dump_directory writes level 0 of every upload as a TGA, read back from GL */
static void texture_dump(GLenum target, const struct xgpu_texture_description *description)
{
#ifdef HALO_GLES
	/* ES cannot read textures back */
	(void)target;
	(void)description;
}
#else
	static unsigned long dump_index = 0;
	const char *directory = *config_string("debug.texture_dump_directory") ?
		config_string("debug.texture_dump_directory") : NULL;
	unsigned long width = description->width, height = description->height;
	unsigned char header[18];
	unsigned char *pixels;
	char path[512];
	FILE *file;

	if (!directory || target != GL_TEXTURE_2D)
		return;
	pixels = malloc(width * height * 4);
	if (!pixels)
		return;
	glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	snprintf(path, sizeof(path), "%s/tex%05lu_fmt%02x_%lux%lu.tga", directory, dump_index++,
		(unsigned)description->format, width, height);
	file = fopen(path, "wb");
	if (file)
	{
		memset(header, 0, sizeof(header));
		header[2] = 2;
		header[12] = (unsigned char)width; header[13] = (unsigned char)(width >> 8);
		header[14] = (unsigned char)height; header[15] = (unsigned char)(height >> 8);
		header[16] = 32; header[17] = 0x28;
		fwrite(header, 1, sizeof(header), file);
		fwrite(pixels, 4, width * height, file);
		fclose(file);
	}
	free(pixels);
}
#endif

/* ---------- Custom Edition channel orders

Halo PC keeps what some textures hold in other channels than the game reads
it from (enum custom_edition_channel_order): a model shader's multipurpose
masks, and a HUD meter's shape and fill order. The Custom Edition map
loading says which texels hold which order as they arrive
(port/linux/game/custom_edition_bitmaps.c), and textures made of them are
sampled with each channel taken from where Halo PC keeps it, which leaves
them as compressed as they were. Addresses stay listed until other texels
arrive there, which the loading also says, or the map goes; the game and the
renderer share a thread. */

/* for each order, the channel (red, green, blue, alpha) of the texels each
channel is sampled from */
static const unsigned char custom_edition_channel_sources[NUMBER_OF_CUSTOM_EDITION_CHANNEL_ORDERS][4] =
{
	{ 0, 1, 2, 3 },
	/* specular, self-illumination, color change and the auxiliary mask */
	{ 2, 1, 3, 0 },
	/* the fill order in color, the shape in alpha */
	{ 3, 3, 3, 0 },
};

struct custom_edition_texels
{
	unsigned long address;
	unsigned char channel_order;
};

static struct custom_edition_texels *custom_edition_texels;
static unsigned long custom_edition_texel_count;
static unsigned long custom_edition_texel_capacity;

/* the order of the texels at address */
static unsigned char custom_edition_texels_order(unsigned long address)
{
	unsigned long index;

	for (index = 0; index < custom_edition_texel_count; index++)
	{
		if (custom_edition_texels[index].address == address)
			return custom_edition_texels[index].channel_order;
	}
	return _custom_edition_channels_xbox;
}

void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order)
{
	unsigned long address = (unsigned long)texels;
	unsigned long index;

	if (channel_order >= NUMBER_OF_CUSTOM_EDITION_CHANNEL_ORDERS)
		channel_order = _custom_edition_channels_xbox;
	for (index = 0; index < custom_edition_texel_count && custom_edition_texels[index].address != address; index++)
	{
	}
	if (index < custom_edition_texel_count)
	{
		if (channel_order == _custom_edition_channels_xbox)
			custom_edition_texels[index] = custom_edition_texels[--custom_edition_texel_count];
		else
			custom_edition_texels[index].channel_order = channel_order;
	}
	else if (channel_order != _custom_edition_channels_xbox)
	{
		if (custom_edition_texel_count == custom_edition_texel_capacity)
		{
			unsigned long capacity = custom_edition_texel_capacity ? custom_edition_texel_capacity * 2 : 64;
			struct custom_edition_texels *grown = realloc(custom_edition_texels, capacity * sizeof(*grown));

			if (!grown)
			{
				platform_log("no memory to list the texels at %08lx: they are sampled in Halo PC's channel order",
					address);
				return;
			}
			custom_edition_texels = grown;
			custom_edition_texel_capacity = capacity;
		}
		custom_edition_texels[custom_edition_texel_count].address = address;
		custom_edition_texels[custom_edition_texel_count].channel_order = channel_order;
		custom_edition_texel_count++;
	}
}

void halo_custom_edition_texels_forget(void)
{
	free(custom_edition_texels);
	custom_edition_texels = NULL;
	custom_edition_texel_count = 0;
	custom_edition_texel_capacity = 0;
}

static void upload(GLuint texture, GLenum target, const struct xgpu_texture_description *description,
	const unsigned char *base, const D3DCOLOR *palette, unsigned char channel_order)
{
	struct format_information information = xgpu_texture_format_information(description->format);
	unsigned long face_count = description->cube_map ? 6 : 1;
	unsigned long face_size = xgpu_texture_face_size(description);
	unsigned long largest = description->width * description->height * description->depth;
	BOOL decode_compressed = FALSE;
	unsigned long *converted;
	unsigned long face, level;

#ifdef HALO_GLES
	decode_compressed = description->compressed && !xgpu_capabilities.s3tc;
#endif
#ifdef HALO_WEB
	/* WebGL's S3TC takes 2D textures whose sides are multiples of 4 */
	decode_compressed |= description->compressed &&
		(target == GL_TEXTURE_3D || (description->width & 3) || (description->height & 3));
#endif
	converted = description->compressed && !decode_compressed ? NULL : malloc(largest * sizeof(unsigned long));
	if (!converted && !(description->compressed && !decode_compressed))
	{
		platform_log("textures: no memory to convert a %lux%lux%lu texture; it is not drawn",
			description->width, description->height, description->depth);
		return;
	}
	glBindTexture(target, texture);
	xgpu_gl_state_invalidate();
	/* the channel of the texels each channel is sampled from, set on every
	upload: a texture object can be reused for other texels (WebGL 2 has no
	texture swizzle: the converted texels' channels are put in order before
	they are uploaded instead, and Custom Edition maps do not run there) */
#ifndef HALO_WEB
	{
		GLint channels[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };

#ifdef HALO_GLES
		/* converted texels are BGRA in memory (32-bit ARGB words); ES takes
		RGBA */
		if (converted)
		{
			channels[0] = GL_BLUE;
			channels[2] = GL_RED;
		}
#endif
		if (channel_order != _custom_edition_channels_xbox)
		{
			GLint stored[4] = { channels[0], channels[1], channels[2], channels[3] };
			unsigned long channel;

			for (channel = 0; channel < 4; channel++)
				channels[channel] = stored[custom_edition_channel_sources[channel_order][channel]];
		}
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, channels[0]);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, channels[1]);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, channels[2]);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, channels[3]);
	}
#endif
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)description->levels - 1);
	for (face = 0; face < face_count; face++)
	{
		GLenum image_target = description->cube_map ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : target;

		for (level = 0; level < description->levels; level++)
		{
			const unsigned char *source = base + face * face_size + xgpu_texture_level_offset(description, level);
			GLsizei width = (GLsizei)xgpu_texture_level_dimension(description->width, level);
			GLsizei height = (GLsizei)xgpu_texture_level_dimension(description->height, level);
			GLsizei depth = (GLsizei)xgpu_texture_level_dimension(description->depth, level);

			if (description->compressed && !decode_compressed)
			{
				if (target == GL_TEXTURE_3D)
					glCompressedTexImage3D(image_target, (GLint)level, compressed_format(information.kind), width, height, depth, 0,
						(GLsizei)xgpu_texture_level_bytes(description, level), source);
				else
					glCompressedTexImage2D(image_target, (GLint)level, compressed_format(information.kind), width, height, 0,
						(GLsizei)xgpu_texture_level_bytes(description, level), source);
			}
			else
			{
#ifdef HALO_GLES
				if (decode_compressed)
					xgpu_texture_dxt_decode_level(information.kind, source, (unsigned long)width, (unsigned long)height,
						(unsigned long)depth, converted);
				else
#endif
				if (!xgpu_texture_decode_level(description, level, source, palette, converted))
				{
					platform_log("textures: no memory to convert a %lux%lux%lu texture; it is not drawn",
						description->width, description->height, description->depth);
					free(converted);
					return;
				}
#ifdef HALO_WEB
				{
					/* BGRA (32-bit ARGB words) to the RGBA WebGL takes */
					unsigned long texel, count = (unsigned long)width * (unsigned long)height * (unsigned long)depth;

					for (texel = 0; texel < count; texel++)
					{
						unsigned long value = converted[texel];

						converted[texel] = (value & 0xff00ff00UL) | ((value >> 16) & 0xffUL) | ((value & 0xffUL) << 16);
					}
				}
#endif
				if (target == GL_TEXTURE_3D)
					glTexImage3D(image_target, (GLint)level, GL_RGBA8, width, height, depth, 0, GL_BGRA, GL_UNSIGNED_BYTE, converted);
				else
					glTexImage2D(image_target, (GLint)level, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, converted);
			}
		}
	}
	free(converted);
	texture_dump(target, description);
}

/* ---------- cache */

struct texture_entry
{
	struct texture_entry *next;
	DWORD data, format_word, size_word;
	unsigned long palette_hash;
	GLuint texture;
	GLenum target;
	struct xgpu_texture_description description;
	unsigned long address, size;
	unsigned long generation;
	unsigned long last_used_frame;
	/* the high-res HUD texture drawn in its place (hud_hires.h), or -1 */
	long override;
	/* the newest generation of its pages (memory_watch_generation) as of the
	memory watch serial read before it was found: the same while no watched
	page has been written since (0: never found) */
	unsigned long watched_serial, watched_generation;
};

#define TEXTURE_BUCKET_COUNT 4096
#define TEXTURE_IDLE_FRAMES 1800
#define MAXIMUM_PALETTE_VARIANTS 8

static struct texture_entry *texture_buckets[TEXTURE_BUCKET_COUNT];

/* Draws mostly bind the textures the draws before them bound. A lookup of a
texture that is not palettized is remembered with the memory watch serial it
started at: while no watched page has been written since, and no texture
has been dropped, the same lookup finds the same current texture. */
#define RECENT_TEXTURE_COUNT 512

static struct
{
	DWORD data, format_word, size_word;
	struct texture_entry *entry;
	unsigned long watch_serial;
	unsigned long drop_serial;
} recent_textures[RECENT_TEXTURE_COUNT];
static unsigned long texture_drop_serial = 1;
static unsigned long texture_frame = 0;

/* (every bit of the three mixed into the top ones: textures are aligned,
and few sizes and formats are common) */
static unsigned long bucket_index(DWORD data, DWORD format_word, DWORD size_word)
{
	unsigned long hash = (unsigned long)data * 2654435761UL ^ (unsigned long)format_word * 2246822519UL ^
		(unsigned long)size_word * 3266489917UL;

	return ((hash & 0xffffffffUL) >> 20) % TEXTURE_BUCKET_COUNT;
}

/* palettized textures are cached per palette contents: the game rewrites
palettes freely, and often cycles a texture through a few of them */
static unsigned long palette_hash(const D3DCOLOR *palette)
{
	unsigned long hash = 2166136261UL, index;

	if (!palette)
		return 0;
	for (index = 0; index < 256; index++)
		hash = (hash ^ palette[index]) * 16777619UL;
	return hash ? hash : 1;
}

/* an entry's GL texture and description: its replacement's, if it has one,
with the bitmap's own size (which its coordinates are in) */
static GLuint texture_entry_result(struct texture_entry *entry, GLenum *target,
	struct xgpu_texture_description *description)
{
	*target = entry->target;
	*description = entry->description;
	/* (the high-res text's atlas, for its placeholder bitmap: text_hires.h) */
	{
		GLuint atlas = text_hires_atlas_texture(entry->data);

		if (atlas)
		{
			description->levels = 1;
			return atlas;
		}
	}
	/* (a menu's bitmap: menu_files.h) */
	{
		unsigned long levels;
		GLuint art = menu_art_texture(entry->data, &levels);

		if (art)
		{
			description->levels = levels;
			description->hires = TRUE;
			return art;
		}
	}
	/* (a high-res texture drawn for some sprites, for the placeholder the
	game draws them from: hud_hires.h) */
	{
		GLuint texture = hud_hires_placeholder_texture(entry->data, &description->levels);

		if (texture)
		{
			description->hires = TRUE;
			return texture;
		}
	}
	if (entry->override >= 0)
	{
		GLuint texture = hud_hires_override_texture(entry->override, &description->levels);

		if (texture)
		{
			description->hires = TRUE;
			description->hires_coverage = hud_hires_override_coverage(entry->override);
			description->hires_point_threshold = hud_hires_override_point_threshold(entry->override);
			return texture;
		}
	}
	return entry->texture;
}

GLuint xgpu_texture_get(const DWORD *resource, const D3DCOLOR *palette, GLenum *target,
	struct xgpu_texture_description *description)
{
	DWORD data = resource[1], format_word = resource[3], size_word = resource[4];
	struct texture_entry **bucket = &texture_buckets[bucket_index(data, format_word, size_word)];
	struct texture_entry *entry;
	unsigned long generation;
	BOOL palettized = ((format_word & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT) == 0x0b;
	unsigned long hash = palettized ? palette_hash(palette) : 0;

	struct texture_entry *oldest_variant = NULL;
	unsigned long variant_count = 0;
	static int no_cache = -1;
	unsigned long recent = bucket_index(data, format_word, size_word) % RECENT_TEXTURE_COUNT;
	unsigned long watch_serial = memory_watch_serial();

	/* (a texture that is not palettized has the one entry, until one is
	dropped: remembered, a page written since only means checking it) */
	entry = NULL;
	if (!palettized && recent_textures[recent].entry && recent_textures[recent].data == data &&
		recent_textures[recent].format_word == format_word && recent_textures[recent].size_word == size_word &&
		recent_textures[recent].drop_serial == texture_drop_serial)
	{
		entry = recent_textures[recent].entry;
		if (recent_textures[recent].watch_serial == watch_serial)
		{
			entry->last_used_frame = texture_frame;
			return texture_entry_result(entry, target, description);
		}
	}

	if (!entry)
	{
		for (entry = *bucket; entry; entry = entry->next)
		{
			if (entry->data == data && entry->format_word == format_word && entry->size_word == size_word)
			{
				if (entry->palette_hash == hash)
					break;
				variant_count++;
				if (!oldest_variant || entry->last_used_frame < oldest_variant->last_used_frame)
					oldest_variant = entry;
			}
		}
	}
	if (!entry && variant_count >= MAXIMUM_PALETTE_VARIANTS)
	{
		/* a palette that keeps changing reuses the stalest copy */
		entry = oldest_variant;
		entry->palette_hash = hash;
		entry->generation = 0;
	}
	if (!entry)
	{
		entry = calloc(1, sizeof(*entry));
		if (!entry)
		{
			xgpu_texture_describe(format_word, size_word, description);
			*target = GL_TEXTURE_2D;
			return 0;
		}
		entry->data = data;
		entry->format_word = format_word;
		entry->size_word = size_word;
		entry->palette_hash = hash;
		xgpu_texture_describe(format_word, size_word, &entry->description);
		entry->target = entry->description.cube_map ? GL_TEXTURE_CUBE_MAP :
			entry->description.depth > 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
		entry->address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(data);
		/* (a size beyond D3DDevice_GetDeviceCaps' is never uploaded: its
		byte counts would not fit in 32 bits) */
		entry->size = xgpu_texture_size_supported(&entry->description) ?
			xgpu_texture_face_size(&entry->description) * (entry->description.cube_map ? 6 : 1) : 0;
		if (!entry->size)
		{
			platform_log("textures: a %lux%lux%lu texture is larger than the device takes; it is not drawn",
				entry->description.width, entry->description.height, entry->description.depth);
		}
		entry->generation = 0;
		entry->override = -1;
		glGenTextures(1, &entry->texture);
		entry->next = *bucket;
		*bucket = entry;
	}

	if (no_cache < 0)
		no_cache = config_boolean("debug.texture_no_cache");
	/* (its pages' newest generation, found again only once a watched page
	has been written: a large texture's pages, scanned for every draw that
	bound it, were much of a frame with many objects) */
	if (entry->watched_serial && entry->watched_serial == watch_serial)
		generation = entry->watched_generation;
	else
	{
		generation = memory_watch_generation(entry->address, entry->size);
		entry->watched_serial = watch_serial;
		entry->watched_generation = generation;
	}
	if (!entry->generation || generation > entry->generation || no_cache)
	{
		/* protect first, so a write racing with the upload is noticed */
		memory_watch_protect(entry->address, entry->size);
		entry->generation = memory_watch_generation(entry->address, entry->size);
		if (!entry->generation)
			entry->generation = 1;
		/* (which bitmap is here may have changed with the pixels) */
		entry->override = -1;
		if (entry->size && !palettized && !entry->description.cube_map && entry->description.depth == 1)
		{
			unsigned long levels;

			entry->override = hud_hires_override_find(entry->address, entry->description.width,
				entry->description.height, entry->description.levels > 1 ?
				xgpu_texture_level_offset(&entry->description, 1) : xgpu_texture_face_size(&entry->description));
			if (entry->override >= 0 && !hud_hires_override_texture(entry->override, &levels))
				entry->override = -1;
		}
		if (entry->override < 0 && entry->size && platform_is_contiguous((void *)entry->address) &&
			platform_is_contiguous((void *)(entry->address + entry->size - 1)))
		{
			if (config_boolean("debug.texture_log"))
			{
				const unsigned char *bytes = (const unsigned char *)entry->address;
				unsigned long index, ones = 0, zeros = 0;

				for (index = 0; index < entry->size; index++)
				{
					ones += bytes[index] == 0xff;
					zeros += bytes[index] == 0;
				}
				platform_log("texture upload %08lx fmt %02lx %lux%lu size %lu gen %lu ff %lu%% 00 %lu%%",
					(unsigned long)data, (unsigned long)entry->description.format, entry->description.width,
					entry->description.height, entry->size, entry->generation,
					ones * 100 / entry->size, zeros * 100 / entry->size);
			}
			upload(entry->texture, entry->target, &entry->description, (const unsigned char *)entry->address, palette,
				custom_edition_texels_order(entry->address));
		}
	}
	entry->last_used_frame = texture_frame;
	if (!palettized && !no_cache)
	{
		recent_textures[recent].data = data;
		recent_textures[recent].format_word = format_word;
		recent_textures[recent].size_word = size_word;
		recent_textures[recent].entry = entry;
		recent_textures[recent].watch_serial = watch_serial;
		recent_textures[recent].drop_serial = texture_drop_serial;
	}
	return texture_entry_result(entry, target, description);
}

void xgpu_texture_cache_begin_frame(void)
{
	unsigned long index;

	texture_frame++;
	if (texture_frame % 600)
		return;
	/* drop textures that have not been used for a while */
	for (index = 0; index < TEXTURE_BUCKET_COUNT; index++)
	{
		struct texture_entry **link = &texture_buckets[index];

		while (*link)
		{
			struct texture_entry *entry = *link;

			if (texture_frame - entry->last_used_frame > TEXTURE_IDLE_FRAMES)
			{
				*link = entry->next;
				glDeleteTextures(1, &entry->texture);
				xgpu_gl_state_invalidate();
				texture_drop_serial++;
				free(entry);
			}
			else
			{
				link = &entry->next;
			}
		}
	}
}
