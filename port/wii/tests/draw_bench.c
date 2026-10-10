/*
DRAW_BENCH.C

The Direct3D device's draws on the host (tools/wii_draw_bench.py builds it
and runs it under qemu-ppc): port/wii/src/d3d8_gx.c itself, as the Wii build
compiles it, over a stand-in of GX (tests/host/gx_stand_in.c), drawing
synthetic scenes the game's way - its vertex programs and declarations, a
camera and lights, models' nodes - for some frames:
- environment: a structure's lightmapped sections, as triangle lists of the
  frame's visible triangles (rewritten each frame, as the game's dynamic
  triangles are) over static vertices;
- rigid: a model of one node, its parts drawn in two passes;
- skinned: a character's parts of many nodes, some vertices blending two,
  lit on GX's lights (display.gx_lighting), with a reflection.
Each draw is bracketed by bench_region_begin and bench_region_end, whose
calls the script finds in qemu's trace to count the instructions between
them; with the stand-in's recording, each draw's digest of what GX draws is
printed instead, to compare two builds of the device.

usage: draw_bench <scene> <frames> (scene: environment, rigid, skinned, all)
*/

#include "d3d8_gx.c"

#include "host/gx_stand_in.h"

/* ---------- the host (tests/host/bench_host.c) */

void *bench_window(unsigned long address, unsigned long size);
void bench_print(const char *text);

static void print(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void print(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	bench_print(line);
}

/* ---------- what the device calls besides GX */

static int setting_gx_lighting = 1;

int config_boolean(const char *name)
{
	if (!strcmp(name, "debug.gpu_transform") || !strcmp(name, "debug.gpu_cull") ||
		!strcmp(name, "display.bumped_highlights"))
		return 1;
	if (!strcmp(name, "display.gx_lighting"))
		return setting_gx_lighting;
	return 0;
}

struct platform_file_reads platform_file_reads;
void dsound_wii_audio_report(void) {}
void wii_heap_usage(unsigned long *used, unsigned long *free_bytes) { *used = *free_bytes = 0; }
unsigned long wii_game_stack_used(unsigned long *size) { *size = 0; return 0; }
void wii_memory_window_usage(unsigned long *used, unsigned long *free_bytes) { *used = *free_bytes = 0; }
void wl_mem_log_usage(void) {}
void wl_gx_begin_frame(uint32_t clear_rgba) { (void)clear_rgba; }
uint32_t wl_gx_end_frame(void) { return 0; }
WlResult wl_gx_init(void) { return WL_OK; }
void wl_gx_invalidate_shadow(void) {}
void wl_gx_set_efb_alpha(bool enabled) { (void)enabled; }
WlResult wl_platform_video_init(void) { return WL_OK; }
uint64_t wl_platform_time_us(void) { static uint64_t now; return now += 10; }
size_t wl_gxc_tex_bytes(WlGxcFormat format, uint32_t width, uint32_t height)
{
	(void)format;
	return (size_t)width * height * 4;
}
size_t wl_gxc_encode(WlGxcFormat format, const uint8_t *rgba, uint32_t width, uint32_t height, void *out, size_t out_size)
{
	(void)format;
	(void)rgba;
	(void)width;
	(void)height;
	memset(out, 0, out_size);
	return out_size;
}

/* the window: the Wii's, the texture cache's part of it, and the rest
handed out in order */
#define WINDOW_BYTES (32UL << 20)
#define TEXTURE_CACHE_OFFSET (16UL << 20)
static unsigned char *window;
static unsigned long window_used;

void *physical_memory_get_texture_cache_base_address(void)
{
	return window + TEXTURE_CACHE_OFFSET;
}

static void *window_allocate(unsigned long size)
{
	void *block;

	window_used = (window_used + 63) & ~63UL;
	if (window_used + size > TEXTURE_CACHE_OFFSET)
	{
		print("bench: the window is full\n");
		exit(1);
	}
	block = window + window_used;
	window_used += size;
	memset(block, 0, size);
	return block;
}

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment, unsigned long physical_address, DWORD protect)
{
	(void)alignment;
	(void)physical_address;
	(void)protect;
	return window_allocate(size);
}

void platform_contiguous_free(void *address)
{
	(void)address;
}

/* ---------- the regions counted (their names printed as they begin) */

__attribute__((noinline)) void bench_region_begin(void)
{
	__asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) void bench_region_end(void)
{
	__asm__ volatile("" ::: "memory");
}

/* ---------- the game's vertex programs and declarations */

static const unsigned long vertex_shader_code[] =
{
#include "rasterizer/xbox/rasterizer_xbox_vertex_shaders_data.inc"
};

/* (rasterizer_xbox_vertex_shaders.c's table: each program's offset) */
static const unsigned short vertex_shader_offsets[67] =
{
	0x0000, 0x0078, 0x0140, 0x0308, 0x0470, 0x04F8, 0x0670, 0x0708, 0x0890, 0x0918, 0x0BF0, 0x1018, 0x1210, 0x13E8,
	0x1540, 0x1808, 0x1B70, 0x1D08, 0x2200, 0x2478, 0x2710, 0x27D8, 0x2910, 0x2A98, 0x2BF0, 0x2E48, 0x31C0, 0x3308,
	0x3490, 0x36B8, 0x3770, 0x39C8, 0x3F30, 0x4198, 0x4380, 0x4658, 0x4900, 0x4CB8, 0x4DA0, 0x4E58, 0x4FB0, 0x5088,
	0x51E0, 0x52E8, 0x54E0, 0x5648, 0x5A00, 0x5B88, 0x5F00, 0x6158, 0x6280, 0x6708, 0x6830, 0x6938, 0x6A40, 0x6C78,
	0x6DD0, 0x6E68, 0x7260, 0x72D8, 0x7410, 0x7788, 0x7970, 0x7D38, 0x80D0, 0x8368, 0x8510,
};

/* (rasterizer_xbox_vertex_shaders_initialize.c's declarations, and which
each program takes, by its offset in them) */
static const DWORD vertex_shader_declarations[] =
{
	0x20000000, 0x40320000, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40210004, 0xFFFFFFFF,
	0x20000000, 0x40340000, 0x40340009, 0x40150008, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40220004, 0x40400009, 0x20000001, 0x4024000B, 0xFFFFFFFF,
	0x20000000, 0x40220000, 0x40220004, 0x40400009, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40210004, 0x40240005, 0x40110006, 0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0x20000001, 0x40160007, 0x40210008,
	0xFFFFFFFF,
	0x20000000, 0x40320000, 0x40160001, 0x40160002, 0x40160003, 0x40220004, 0xFFFFFFFF,
};

enum
{
	DECLARATION_MODEL = 0x78 / 4,
	DECLARATION_LIGHTMAPPED = 0x9C / 4,
	DECLARATION_ENVIRONMENT = 0xC4 / 4,
};

static const unsigned char vertex_shader_declaration_of[67] =
{
	[4] = 0x64 / 4, [3] = 0x64 / 4, [38] = 0x64 / 4, [33] = 0x20 / 4, [65] = 0x34 / 4, [2] = 0x34 / 4, [12] = 0x34 / 4,
	[56] = 0x34 / 4, [0] = 0x00 / 4, [1] = 0x10 / 4, [66] = 0x48 / 4, [16] = 0x9C / 4, [49] = 0xC4 / 4, [29] = 0xC4 / 4,
	[40] = 0xC4 / 4, [21] = 0xC4 / 4, [41] = 0xC4 / 4, [59] = 0x9C / 4, [58] = 0x9C / 4, [26] = 0xC4 / 4, [42] = 0xC4 / 4,
	[44] = 0x9C / 4, [51] = 0xC4 / 4, [6] = 0xC4 / 4, [8] = 0xC4 / 4, [10] = 0x78 / 4, [11] = 0x20 / 4, [9] = 0x78 / 4,
	[27] = 0x78 / 4, [17] = 0x78 / 4, [64] = 0x78 / 4, [39] = 0x78 / 4, [13] = 0x78 / 4, [5] = 0x78 / 4, [24] = 0xC4 / 4,
	[48] = 0xC4 / 4, [34] = 0xC4 / 4, [19] = 0xC4 / 4, [35] = 0xC4 / 4, [47] = 0x78 / 4, [31] = 0x78 / 4, [60] = 0x78 / 4,
	[57] = 0x78 / 4, [45] = 0x78 / 4, [62] = 0x78 / 4, [46] = 0xC4 / 4, [28] = 0xC4 / 4, [43] = 0xC4 / 4, [61] = 0xC4 / 4,
	[25] = 0x9C / 4, [30] = 0x78 / 4, [63] = 0x78 / 4, [36] = 0x78 / 4, [50] = 0x78 / 4, [20] = 0xC4 / 4, [23] = 0xC4 / 4,
	[18] = 0x78 / 4, [14] = 0x78 / 4, [22] = 0xC4 / 4, [32] = 0x78 / 4, [15] = 0x78 / 4, [37] = 0xC4 / 4, [7] = 0xC4 / 4,
	[54] = 0xC4 / 4, [55] = 0xC4 / 4, [53] = 0xC4 / 4, [52] = 0xC4 / 4,
};

static DWORD vertex_shader_handles[67];

static void vertex_shaders_create(void)
{
	int index;

	for (index = 0; index < 67; index++)
		D3DDevice_CreateVertexShader(&vertex_shader_declarations[vertex_shader_declaration_of[index]],
			(const DWORD *)((const unsigned char *)vertex_shader_code + vertex_shader_offsets[index]),
			&vertex_shader_handles[index], 0);
}

/* the program's hash (its compiled program's: nv2a_cpu_hash) */
static unsigned long vertex_shader_hash(int index)
{
	const DWORD *code = (const DWORD *)((const unsigned char *)vertex_shader_code + vertex_shader_offsets[index]);

	return nv2a_cpu_hash(code + 1, code[0] >> 16);
}

static int vertex_shader_find(unsigned long hash)
{
	int index;

	for (index = 0; index < 67; index++)
		if (vertex_shader_hash(index) == hash)
			return index;
	print("bench: no program %08lx\n", hash);
	exit(1);
}

/* each program: its declaration and what it depends on */
static void programs_list(void)
{
	int index;

	for (index = 0; index < 67; index++)
	{
		struct vertex_shader_object *shader = vertex_shader_from_handle(vertex_shader_handles[index]);
		const struct nv2a_cpu_dependencies *d;
		unsigned long clip;
		int stage;

		device.vertex_shader = shader;
		program_current();
		d = &shader->dependencies;
		clip = d->clip[0] | d->clip[1] | d->clip[3];
		print("%2d %08lx decl %02x clip %05lx%s relative %d t:", index, vertex_shader_hash(index),
			vertex_shader_declaration_of[index] * 4, clip, (clip & NV2A_CPU_NOT_AFFINE) ? " (not affine)" : "",
			d->relative);
		for (stage = 0; stage < 4; stage++)
			print(" %05lx", d->t[stage][0] | d->t[stage][1] | d->t[stage][2]);
		print(" d0 %05lx d1 %05lx fog %05lx%s\n", d->d0[0] | d->d0[1] | d->d0[2] | d->d0[3],
			d->d1[0] | d->d1[1] | d->d1[2] | d->d1[3], d->fog, gx_lighting_recipe_find(shader->program->hash) ? " lit" : "");
	}
}

/* ---------- the scene's data */

/* a deterministic generator (the same scenes in every build) */
static unsigned long random_state = 12345;

static float uniform(float low, float high)
{
	random_state = random_state * 1103515245UL + 12345UL;
	return low + (high - low) * (float)((random_state >> 8) & 0xFFFF) / 65535.0f;
}

static unsigned long physical(const void *address)
{
	return PLATFORM_VIRTUAL_TO_PHYSICAL(address);
}

/* a normal as the maps' 11:11:10 word */
static u32 packed_normal(float x, float y, float z)
{
	long ix = (long)(x * 1023.0f), iy = (long)(y * 1023.0f), iz = (long)(z * 511.0f);

	return (u32)(ix & 0x7FF) | (u32)(iy & 0x7FF) << 11 | (u32)(iz & 0x3FF) << 22;
}

static void float_store(unsigned char *out, float value)
{
	memcpy(out, &value, 4);
}

static void short_store(unsigned char *out, short value)
{
	memcpy(out, &value, 2);
}

/* a texture of the map's, converted (in the texture cache: bound where it
is): DXT1, size by size, or a cube map left as its sphere map */
static D3DTexture *texture_make(unsigned size, BOOL cube)
{
	D3DTexture *texture = calloc(1, sizeof(*texture));
	unsigned shift = 0, bytes;
	static unsigned long cache_used;
	unsigned char *texels;

	while ((1U << shift) < size)
		shift++;
	bytes = cube ? 6 * size * size * 4 : size * size / 2;
	texels = window + TEXTURE_CACHE_OFFSET + cache_used;
	cache_used += (bytes + 127) & ~127UL;
	memset(texels, 0x5A, bytes);
	texture->Common = D3DCOMMON_TYPE_TEXTURE | 1;
	texture->Data = physical(texels);
	texture->Format = (2UL << D3DFORMAT_DIMENSION_SHIFT) | ((DWORD)shift << D3DFORMAT_USIZE_SHIFT) |
		((DWORD)shift << D3DFORMAT_VSIZE_SHIFT) | (1UL << D3DFORMAT_MIPMAP_SHIFT);
	if (cube)
	{
		unsigned char *trailer = texels + bytes - 32;

		texture->Format |= ((DWORD)0x06 << D3DFORMAT_FORMAT_SHIFT) | D3DFORMAT_CUBEMAP;
		memcpy(trailer, "GXSPHERE", 8);
		trailer[8] = (unsigned char)(64 >> 8);
		trailer[9] = 64;
		trailer[10] = GX_TF_RGB565;
	}
	else
		texture->Format |= (DWORD)0x0C << D3DFORMAT_FORMAT_SHIFT;
	return texture;
}

static D3DVertexBuffer *vertex_buffer_make(unsigned long bytes, unsigned char **data)
{
	D3DVertexBuffer *buffer = calloc(1, sizeof(*buffer));

	*data = window_allocate(bytes);
	/* (a map's: registered, not created by the device) */
	buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
	buffer->Data = physical(*data);
	return buffer;
}

/* a grid's triangle strip (rows joined by degenerate triangles), or its
triangle list */
static unsigned grid_indices(unsigned columns, unsigned rows, BOOL strip, WORD *out)
{
	unsigned count = 0, row, column;

	for (row = 0; row < rows; row++)
	{
		if (strip)
		{
			if (row)
			{
				out[count++] = (WORD)(row * (columns + 1) + columns);
				out[count++] = (WORD)(row * (columns + 1));
			}
			for (column = 0; column <= columns; column++)
			{
				out[count++] = (WORD)(row * (columns + 1) + column);
				out[count++] = (WORD)((row + 1) * (columns + 1) + column);
			}
		}
		else
			for (column = 0; column < columns; column++)
			{
				WORD a = (WORD)(row * (columns + 1) + column), b = (WORD)(a + 1), c = (WORD)(a + columns + 1),
					d = (WORD)(c + 1);

				out[count++] = a;
				out[count++] = c;
				out[count++] = b;
				out[count++] = b;
				out[count++] = c;
				out[count++] = d;
			}
	}
	return count;
}

/* ---------- the camera and the lights (as tests/gx_lighting_check.c sets them) */

static float constants[NV2A_CPU_CONSTANT_COUNT][4];
#define C(index) constants[(index) + NV2A_CPU_CONSTANT_BIAS]

static void camera_set(int frame)
{
	float angle = 0.6f + 0.01f * (float)frame, eye[3] = { 10.0f * cosf(angle), 10.0f * sinf(angle), 3.0f };
	float view[3][3], target[3] = { 0.0f, 0.0f, 1.0f }, up[3] = { 0.0f, 0.0f, 1.0f }, length;
	float fovy = 1.1f, aspect = 4.0f / 3.0f, near_plane = 0.06f, far_plane = 1000.0f, cot = 1.0f / tanf(fovy / 2);
	int row, column;

	/* (view[2] points back from the target, view[0] right, view[1] up) */
	for (row = 0; row < 3; row++)
		view[2][row] = eye[row] - target[row];
	length = sqrtf(view[2][0] * view[2][0] + view[2][1] * view[2][1] + view[2][2] * view[2][2]);
	for (row = 0; row < 3; row++)
		view[2][row] /= length;
	view[0][0] = up[1] * view[2][2] - up[2] * view[2][1];
	view[0][1] = up[2] * view[2][0] - up[0] * view[2][2];
	view[0][2] = up[0] * view[2][1] - up[1] * view[2][0];
	length = sqrtf(view[0][0] * view[0][0] + view[0][1] * view[0][1] + view[0][2] * view[0][2]);
	for (row = 0; row < 3; row++)
		view[0][row] /= length;
	view[1][0] = view[2][1] * view[0][2] - view[2][2] * view[0][1];
	view[1][1] = view[2][2] * view[0][0] - view[2][0] * view[0][2];
	view[1][2] = view[2][0] * view[0][1] - view[2][1] * view[0][0];
	{
		float translation[3];

		for (row = 0; row < 3; row++)
			translation[row] = -(view[row][0] * eye[0] + view[row][1] * eye[1] + view[row][2] * eye[2]);
		for (column = 0; column < 3; column++)
		{
			C(-96)[column] = cot / aspect * view[0][column];
			C(-95)[column] = cot * view[1][column];
			C(-94)[column] = -far_plane / (far_plane - near_plane) * view[2][column];
			C(-93)[column] = -view[2][column];
		}
		C(-96)[3] = cot / aspect * translation[0];
		C(-95)[3] = cot * translation[1];
		C(-94)[3] = -far_plane / (far_plane - near_plane) * translation[2] - far_plane * near_plane / (far_plane - near_plane);
		C(-93)[3] = -translation[2];
	}
	memcpy(C(-92), eye, sizeof(eye));
	C(-92)[3] = 2.0f;
	C(-89)[3] = 765.0f;
	C(-84)[0] = C(-84)[1] = 4.0f;
	C(-84)[2] = C(-84)[3] = 1.0f;
	C(-83)[0] = 1.0f;
	C(-82)[1] = 1.0f;
	C(-81)[0] = C(-81)[1] = C(-81)[2] = 0.3f;
	C(-80)[0] = C(-80)[1] = C(-80)[2] = 0.2f;
	/* two distant lights and the ambient */
	C(-73)[0] = 0.577f;
	C(-73)[1] = 0.577f;
	C(-73)[2] = -0.577f;
	C(-72)[0] = C(-72)[1] = C(-72)[2] = 0.8f;
	C(-71)[1] = -1.0f;
	C(-70)[0] = C(-70)[1] = C(-70)[2] = 0.3f;
	C(-69)[0] = C(-69)[1] = C(-69)[2] = 0.15f;
	/* (c-96 to c-69: the viewport's, c-38 and c-37, are the device's) */
	D3DDevice_SetVertexShaderConstant(-96, C(-96), 28);
}

/* a pixel shader of one combiner: r0 = A B + C D (rgb), A B (alpha), then
the final combiner's r0 */
static void pixel_shader_set(DWORD texture_modes, DWORD rgb_inputs, DWORD rgb_outputs, DWORD alpha_inputs)
{
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = texture_modes;
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = 0x00011001;
	D3D__RenderState[D3DRS_PSRGBINPUTS0] = rgb_inputs;
	D3D__RenderState[D3DRS_PSRGBOUTPUTS0] = rgb_outputs;
	D3D__RenderState[D3DRS_PSALPHAINPUTS0] = alpha_inputs;
	D3D__RenderState[D3DRS_PSALPHAOUTPUTS0] = 0x000000C0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = 0x0000000C;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = 0x00001C00;
}

/* ---------- drawing, counted or recorded */

static BOOL recording;
static unsigned long draws_in_frame;

static void draw(const char *scene, int frame, D3DPRIMITIVETYPE primitive, const WORD *indices, UINT count)
{
	print("region %s frame=%d draw=%lu indices=%u\n", scene, frame, draws_in_frame, count);
	bench_region_begin();
	D3DDevice_DrawIndexedVertices(primitive, count, indices);
	bench_region_end();
	if (recording)
	{
		static struct gx_stand_in_draw taken[256];
		unsigned long index, count_taken = gx_stand_in_take(taken, 256);

		for (index = 0; index < count_taken && index < 256; index++)
			print("draw %s f%d d%lu.%lu: %016llx %u\n", scene, frame, draws_in_frame, index,
				(unsigned long long)taken[index].digest, (unsigned)taken[index].vertices);
	}
	draws_in_frame++;
}

/* ---------- the environment: lightmapped sections */

enum { SECTIONS = 12, SECTION_COLUMNS = 20, SECTION_ROWS = 15 };

static struct
{
	D3DVertexBuffer *vertices, *lightmap;
	WORD *triangles;
	unsigned triangle_count;
} sections[SECTIONS];
static D3DIndexBuffer environment_index_buffer;
static WORD *environment_indices;
static D3DTexture *environment_textures[3];

static void environment_make(void)
{
	unsigned section;

	environment_indices = window_allocate(SECTIONS * SECTION_COLUMNS * SECTION_ROWS * 6 * sizeof(WORD));
	environment_index_buffer.Common = D3DCOMMON_TYPE_INDEXBUFFER | D3DCOMMON_D3DCREATED | 1;
	environment_index_buffer.Data = (DWORD)environment_indices;
	for (section = 0; section < 3; section++)
		environment_textures[section] = texture_make(section == 2 ? 128 : 256, FALSE);
	for (section = 0; section < SECTIONS; section++)
	{
		unsigned vertex_count = (SECTION_COLUMNS + 1) * (SECTION_ROWS + 1), row, column;
		unsigned char *data, *lightmap;

		sections[section].vertices = vertex_buffer_make(vertex_count * 32, &data);
		sections[section].lightmap = vertex_buffer_make(vertex_count * 8, &lightmap);
		for (row = 0; row <= SECTION_ROWS; row++)
			for (column = 0; column <= SECTION_COLUMNS; column++)
			{
				unsigned vertex = row * (SECTION_COLUMNS + 1) + column;
				unsigned char *v = data + vertex * 32, *l = lightmap + vertex * 8;
				u32 normal = packed_normal(0.0f, 0.0f, 1.0f);

				float_store(v, (float)(section % 4) * 8.0f + (float)column * 0.4f - 16.0f);
				float_store(v + 4, (float)(section / 4) * 8.0f + (float)row * 0.5f - 12.0f);
				float_store(v + 8, uniform(-0.2f, 0.2f));
				memcpy(v + 12, &normal, 4);
				normal = packed_normal(1.0f, 0.0f, 0.0f);
				memcpy(v + 16, &normal, 4);
				normal = packed_normal(0.0f, 1.0f, 0.0f);
				memcpy(v + 20, &normal, 4);
				float_store(v + 24, (float)column * 0.25f);
				float_store(v + 28, (float)row * 0.25f);
				normal = packed_normal(0.0f, 0.0f, 1.0f);
				memcpy(l, &normal, 4);
				short_store(l + 4, (short)(column * 1000));
				short_store(l + 6, (short)(row * 1000));
			}
		sections[section].triangles = malloc(SECTION_COLUMNS * SECTION_ROWS * 6 * sizeof(WORD));
		sections[section].triangle_count = grid_indices(SECTION_COLUMNS, SECTION_ROWS, FALSE, sections[section].triangles) / 3;
	}
}

/* the frame's visible triangles, written into the dynamic index buffer as
the game writes them, then each section drawn: its diffuse and detail
pass, then its lightmap */
static void environment_draw(int frame)
{
	WORD *next = environment_indices;
	const WORD *starts[SECTIONS];
	unsigned counts[SECTIONS], section, pass;

	for (section = 0; section < SECTIONS; section++)
	{
		unsigned triangles = sections[section].triangle_count;

		/* (on the third frame some sections see fewer triangles) */
		if (frame == 2 && section % 3 == 0)
			triangles -= triangles / 4;
		starts[section] = next;
		counts[section] = triangles * 3;
		memcpy(next, sections[section].triangles, triangles * 3 * sizeof(WORD));
		next += triangles * 3;
	}
	D3DDevice_SetIndices(&environment_index_buffer, 0);
	for (pass = 0; pass < 2; pass++)
	{
		D3DDevice_SetVertexShader(vertex_shader_handles[vertex_shader_find(pass ? 0xa60ed22cUL : 0x0dfdf660UL)]);
		if (pass)
		{
			pixel_shader_set(1, 0x08200000, 0x000000C0, 0x18200000);
			D3DDevice_SetTexture(0, (D3DBaseTexture *)environment_textures[2]);
			D3DDevice_SetTexture(1, NULL);
		}
		else
		{
			pixel_shader_set(1 | 1 << 5, 0x08090000, 0x000000C0, 0x18200000);
			D3DDevice_SetTexture(0, (D3DBaseTexture *)environment_textures[0]);
			D3DDevice_SetTexture(1, (D3DBaseTexture *)environment_textures[1]);
		}
		/* the texture coordinates' transforms (c-80 on, as the game's) */
		{
			float transforms[4][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f },
				{ 4.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 4.0f, 0.0f, 0.0f } };

			D3DDevice_SetVertexShaderConstant(-80, transforms, 4);
		}
		for (section = 0; section < SECTIONS; section++)
		{
			D3DDevice_SetStreamSource(0, sections[section].vertices, 32);
			D3DDevice_SetStreamSource(1, sections[section].lightmap, 8);
			draw("environment", frame, D3DPT_TRIANGLELIST, starts[section], counts[section]);
		}
	}
}

/* ---------- models: rigid (one node) and skinned (several, some vertices
blending two) */

enum { MODEL_COLUMNS = 16, MODEL_ROWS = 12, MODEL_VERTICES = (MODEL_COLUMNS + 1) * (MODEL_ROWS + 1) };

struct model_part
{
	D3DVertexBuffer *vertices;
	WORD *strip;
	unsigned strip_count;
};

/* a part over nodes first to first + node_count - 1, in bands along its
columns, the vertices at a band's edge blending its two nodes */
static void model_part_make(struct model_part *part, unsigned first, unsigned node_count, BOOL blended)
{
	unsigned char *data;
	unsigned row, column;

	part->vertices = vertex_buffer_make(MODEL_VERTICES * 32, &data);
	for (row = 0; row <= MODEL_ROWS; row++)
		for (column = 0; column <= MODEL_COLUMNS; column++)
		{
			unsigned char *v = data + (row * (MODEL_COLUMNS + 1) + column) * 32;
			float angle = (float)column * (6.2831853f / MODEL_COLUMNS);
			unsigned band = column * node_count / (MODEL_COLUMNS + 1), node = first + band;
			unsigned other = band + 1 < node_count ? node + 1 : node;
			float weight = 1.0f;
			u32 normal = packed_normal(cosf(angle), sinf(angle), 0.0f);

			float_store(v, 0.3f * cosf(angle));
			float_store(v + 4, 0.3f * sinf(angle));
			float_store(v + 8, (float)row * 0.15f);
			memcpy(v + 12, &normal, 4);
			normal = packed_normal(0.0f, 0.0f, 1.0f);
			memcpy(v + 16, &normal, 4);
			normal = packed_normal(-sinf(angle), cosf(angle), 0.0f);
			memcpy(v + 20, &normal, 4);
			short_store(v + 24, (short)(column * 2000));
			short_store(v + 26, (short)(row * 2500));
			/* (the last column of a band blends into the next node's) */
			if (blended && other != node && (column + 1) * node_count / (MODEL_COLUMNS + 1) != band)
				weight = 0.25f + 0.5f * (float)(row % 3) / 2.0f;
			v[28] = (unsigned char)node;
			v[29] = (unsigned char)other;
			short_store(v + 30, (short)(weight * 32767.0f + 0.5f));
		}
	part->strip = window_allocate(MODEL_COLUMNS * MODEL_ROWS * 4 * sizeof(WORD));
	part->strip_count = grid_indices(MODEL_COLUMNS, MODEL_ROWS, TRUE, part->strip);
}

enum { RIGID_MODELS = 6, RIGID_PARTS = 2, SKINNED_MODELS = 3, SKINNED_PARTS = 5, NODES = 20 };

static struct model_part rigid_parts[RIGID_PARTS], skinned_parts[SKINNED_PARTS];
static D3DTexture *model_texture, *model_cube;

static void models_make(void)
{
	unsigned part;

	model_texture = texture_make(256, FALSE);
	model_cube = texture_make(64, TRUE);
	for (part = 0; part < RIGID_PARTS; part++)
		model_part_make(&rigid_parts[part], 0, 1, FALSE);
	/* (a part of 2, 3 and 4 nodes, and one of 12: past GX's nine matrices) */
	model_part_make(&skinned_parts[0], 0, 2, TRUE);
	model_part_make(&skinned_parts[1], 2, 3, TRUE);
	model_part_make(&skinned_parts[2], 5, 4, TRUE);
	model_part_make(&skinned_parts[3], 9, 2, FALSE);
	model_part_make(&skinned_parts[4], 6, 12, TRUE);
}

/* a model's nodes about where it stands; animated ones turn a little each frame */
static void nodes_set(unsigned model, int frame, BOOL animated, unsigned node_count)
{
	float place[3] = { -3.0f + 2.0f * (float)(model % 4), -2.0f + 2.0f * (float)(model / 4), 0.0f };
	unsigned node, row;

	for (node = 0; node < node_count; node++)
	{
		float angle = 0.3f * (float)node + (animated ? 0.05f * (float)frame : 0.0f), c = cosf(angle), s = sinf(angle);
		float rows[3][4] = { { c, -s, 0.0f, place[0] }, { s, c, 0.0f, place[1] }, { 0.0f, 0.0f, 1.0f, place[2] + 0.1f * node } };

		for (row = 0; row < 3; row++)
			memcpy(C(-36 + 3 * (int)node + (int)row), rows[row], sizeof(rows[row]));
	}
	D3DDevice_SetVertexShaderConstant(-36, C(-36), 3 * node_count);
}

static void model_pass(BOOL reflection)
{
	if (reflection)
	{
		pixel_shader_set(1 | 3 << 15, 0x08040B05, 0x00000C00, 0x18200000);
		D3DDevice_SetTexture(3, (D3DBaseTexture *)model_cube);
	}
	else
	{
		pixel_shader_set(1, 0x08040000, 0x000000C0, 0x18200000);
		D3DDevice_SetTexture(3, NULL);
	}
	D3DDevice_SetTexture(0, (D3DBaseTexture *)model_texture);
	D3DDevice_SetTexture(1, NULL);
	D3DDevice_SetTexture(2, NULL);
}

static void rigid_draw(int frame)
{
	unsigned model, part;

	for (model = 0; model < RIGID_MODELS; model++)
	{
		nodes_set(model, frame, FALSE, 1);
		/* the lit pass with its reflection, then a second pass of the texture */
		D3DDevice_SetVertexShader(vertex_shader_handles[vertex_shader_find(0xcfc6f3a4UL)]);
		model_pass(TRUE);
		for (part = 0; part < RIGID_PARTS; part++)
		{
			D3DDevice_SetStreamSource(0, rigid_parts[part].vertices, 32);
			draw("rigid", frame, D3DPT_TRIANGLESTRIP, rigid_parts[part].strip, rigid_parts[part].strip_count);
		}
		D3DDevice_SetVertexShader(vertex_shader_handles[vertex_shader_find(0x9539ee1eUL)]);
		model_pass(FALSE);
		for (part = 0; part < RIGID_PARTS; part++)
		{
			D3DDevice_SetStreamSource(0, rigid_parts[part].vertices, 32);
			draw("rigid", frame, D3DPT_TRIANGLESTRIP, rigid_parts[part].strip, rigid_parts[part].strip_count);
		}
	}
}

static void skinned_draw(int frame)
{
	unsigned model, part;

	for (model = 0; model < SKINNED_MODELS; model++)
	{
		nodes_set(model, frame, model != 0, NODES);
		/* each part lit with its reflection, then lit by point lights (no reflection) */
		for (part = 0; part < SKINNED_PARTS; part++)
		{
			D3DDevice_SetStreamSource(0, skinned_parts[part].vertices, 32);
			D3DDevice_SetVertexShader(vertex_shader_handles[vertex_shader_find(0xcfc6f3a4UL)]);
			model_pass(TRUE);
			draw("skinned", frame, D3DPT_TRIANGLESTRIP, skinned_parts[part].strip, skinned_parts[part].strip_count);
			D3DDevice_SetVertexShader(vertex_shader_handles[vertex_shader_find(0x9d2757d2UL)]);
			model_pass(FALSE);
			draw("skinned", frame, D3DPT_TRIANGLESTRIP, skinned_parts[part].strip, skinned_parts[part].strip_count);
		}
	}
}

int main(int argc, char **argv)
{
	IDirect3DDevice8 *d3d_device;
	const char *scene = argc > 1 ? argv[1] : "all";
	int frames = argc > 2 ? atoi(argv[2]) : 4, frame;
	BOOL all = !strcmp(scene, "all");

	window = bench_window(PLATFORM_CONTIGUOUS_WINDOW, WINDOW_BYTES);
	Direct3D_CreateDevice(0, D3DDEVTYPE_HAL, NULL, 0, NULL, (D3DDevice **)&d3d_device);
	vertex_shaders_create();
	if (!strcmp(scene, "programs"))
	{
		programs_list();
		return 0;
	}
	if (!all && strcmp(scene, "environment") && strcmp(scene, "rigid") && strcmp(scene, "skinned"))
	{
		print("usage: draw_bench programs|environment|rigid|skinned|all [frames] [record]\n");
		return 1;
	}
	recording = argc > 3 && !strcmp(argv[3], "record");
	environment_make();
	models_make();
	for (frame = 0; frame < frames; frame++)
	{
		draws_in_frame = 0;
		camera_set(frame);
		if (all || !strcmp(scene, "environment"))
			environment_draw(frame);
		if (all || !strcmp(scene, "rigid"))
			rigid_draw(frame);
		if (all || !strcmp(scene, "skinned"))
			skinned_draw(frame);
		D3DDevice_Present(NULL, NULL, NULL, NULL);
		if (recording)
		{
			static struct gx_stand_in_draw taken[256];

			gx_stand_in_take(taken, 256);
		}
	}
	if (recording)
		print("errors: %lu vertex writes outside a primitive or primitives left short\n", gx_stand_in_errors());
	print("node keys: %lu kept, %lu made, %lu bytes\n", key_records_kept, key_records_made, key_record_bytes);
	print("hardware: %lu draws, %lu matrices kept, %lu keyed, %lu blended, %lu lit, %lu refused\n", hardware_draws,
		hardware_cache_hits, hardware_keyed_draws, hardware_blended_draws, lit_draws, lit_refused);
	return 0;
}
