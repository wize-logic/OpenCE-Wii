/*
D3D8_GX.C

The Xbox Direct3D 8 device for the Wii, over GX, in place of
port/linux/src/d3d8_gl.c.

The game drives the device through the XDK's inline functions, which keep
the "simple" render states in D3D__RenderState (port/linux/src/d3d8_state.c,
shared with the OpenGL device) and call into this file for everything else.
Resources - textures, surfaces, vertex and index buffers - are the shared
d3d8_resources.c's, in the contiguous window, identified by the physical
address in their Data field.

GX has no programmable vertex stage: each draw's vertices are fetched from
their streams by the vertex shader's declaration and run through its NV2A
program on the processor (nv2a_cpu.c), and GX draws what the program leaves
in screen space, through an orthographic projection: x and y in pixels,
depth from the viewport's z scale. The pixel stage is a TEV approximation of
the register combiners (tev_setup), the textures decoded into GX's RGBA8
(texture_get) and kept while their pages are unchanged. The render states
the game sets become GX's blending, depth, culling and alpha test.

Draws to targets other than the screen (render-to-texture) are not shown
yet; visibility tests report nothing visible, as the OpenGL device's null
renderer does.
*/

#include "platform.h"

#include <math.h>
#include <tuxedo/ppc/intrinsics.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

/* (libogc's BOOL is an unsigned int; the XDK's, which the device uses, an
int: libogc's is renamed while its headers are read) */
#define BOOL GCBOOL
#include <ogc/cache.h>
#include <ogc/gx.h>
#include <ogc/video.h>
#undef BOOL

#include "gx_psh.h"
#include "gx_psh_cache.h"
#include "gx_lighting.h"
#include "port_config.h"
#include "nv2a_cpu_native.h"
#include "wii_memory.h"
#include "wii_video.h"
#include "xbox_texture_formats.h"
#include "wl/codec/gx_tex.h"

#include "wl/platform/gx_target.h"
#include "wl/platform/heap_overflow.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"
#include "wl/platform/timebase.h"

void wii_heap_usage(unsigned long *used, unsigned long *free_bytes);

void d3d8_state_defaults(void);

/* where render targets park the back buffer (the EFB's owner, below) */
static void *efb_park_alloc(size_t bytes);
static void efb_park_free(void *memory);
/* (debug.gpu_xf_flush_safe, read at the device's creation: gx_begin) */
static BOOL xf_flush_on;

/* ---------- constants */

#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480
#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define VERTEX_PROGRAM_SLOTS 136
#define VERTEX_CONSTANT_COUNT 192
/* register c[-96] is the first of the 192 */
#define VERTEX_CONSTANT_BIAS 96
#define VERTEX_REGISTER_COUNT 16
#define MAXIMUM_STREAMS 16
#define VISIBILITY_TEST_SLOTS 4096
/* the most immediate-mode vertices between Begin and End */
#define MAXIMUM_IMMEDIATE_VERTICES 4096
/* the most vertices a GX draw takes at once (its count is 16-bit) */
#define MAXIMUM_GX_VERTICES 65535
/* the vertex format the device draws with */
#define DEVICE_VERTEX_FORMAT GX_VTXFMT7
/* (the game's own vertices, indexed: draw_hardware) */
#define HARDWARE_VERTEX_FORMAT GX_VTXFMT6
/* (the view's sphere maps' grids: sphere_bake) */
#define SPHERE_BAKE_VERTEX_FORMAT GX_VTXFMT5
/* bytes of decoded textures kept (in the game's heap) */
#define TEXTURE_CACHE_BYTES (1536UL * 1024UL)
/* the textures known (a converted bitmap's entry holds no memory: b30 binds
more than 128 a frame, which an LRU of 128 lost and found again each frame) */
#define TEXTURE_CACHE_ENTRIES 1024
#define TEXTURE_BUCKETS 512
/* frames between the draw counts in the log */
#define STATISTICS_FRAMES 60

/* ---------- structures */

struct vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long id;
	DWORD *instructions;
	unsigned long instruction_count;
	struct vertex_element elements[NV2A_CPU_INPUT_COUNT];
	unsigned long element_count;
	/* compiled for the processor the first time it draws */
	struct nv2a_cpu_program *program;
	/* what its outputs depend on (nv2a_cpu_analyze), with the program */
	struct nv2a_cpu_dependencies dependencies;
	/* ... and which components read a constant (nv2a_cpu_analyze_constants:
	the others a static vertex buffer's outputs can keep) */
	struct nv2a_cpu_dependencies constant_reads;
	/* ... and with the inputs a draw held the same taken as constants
	(draw_dependencies) */
	struct
	{
		unsigned long uniform;
		struct nv2a_cpu_dependencies dependencies;
	} specialized[2];
	unsigned specialized_count;
};

struct stream_source
{
	D3DVertexBuffer *buffer;
	UINT stride;
};

/* where a frame's time goes, in time base ticks (60.75 a microsecond: timing_now,
converted only when the statistics are logged: timing_frame) */
struct frame_timing
{
	uint64_t window_start;
	uint64_t draws, textures, pixel_shaders, present;
	/* the draws' time by path: GX's transform (its matrices, then its
	vertices), the processor's, immediate mode */
	uint64_t hardware_setup, hardware_vertices, processor, immediate;
	/* GX's transform's setup by part: what the draw depends on and its
	indices' range (with a model's node keys), the matrices (and each node's),
	a lit draw's lights and view sphere map, a static buffer's kept outputs,
	and GX's state (with the streams' flushes) */
	uint64_t setup_keys, setup_matrices, setup_lit, setup_kept, setup_state, setup_draws;
	/* the processor's vertices' steps */
	unsigned long long fetch_ticks, program_ticks, store_ticks, emit_ticks;
	/* the part of present spent waiting for GX to finish the frame's draws
	(the rest: the copy to the display and its retrace) */
	uint64_t present_gx;
};

/* the time base now (a draw reads it a few times: no conversion) */
static inline uint64_t timing_now(void)
{
	return wl_timebase_read();
}

/* ticks in units of unit ticks (60750 a millisecond, 6075 a tenth), a
frame's of the statistics' window: without 64-bit division */
static unsigned long timing_frame(uint64_t ticks, uint32_t unit)
{
	return (unsigned long)wl_udiv64_16(wl_udiv64_16(ticks, unit, NULL), STATISTICS_FRAMES, NULL);
}

/* ticks in milliseconds */
static unsigned long timing_ms(uint64_t ticks)
{
	return (unsigned long)wl_timebase_ms(ticks);
}

struct draw_statistics
{
	unsigned long draws;
	unsigned long vertices;
	/* (run through their programs: fewer, an indexed draw's kept) */
	unsigned long vertices_run;
	unsigned long clears;
	unsigned long immediate_draws;
	unsigned long textures_bound, textures_failed, stages_without_mode;
};

static struct
{
	BOOL created;
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DVIEWPORT8 viewport;
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	struct stream_source streams[MAXIMUM_STREAMS];
	UINT base_vertex_index;
	D3DSHADERCONSTANTMODE shader_constant_mode;
	unsigned long next_vertex_shader_id;
	struct vertex_shader_object *vertex_shader;
	struct vertex_shader_object *program_slots[VERTEX_PROGRAM_SLOTS];
	DWORD program_address;
	float vertex_constants[VERTEX_CONSTANT_COUNT][4];
	/* Begin/End: the primitive, and the vertex registers the next
	SetVertexData* to register 0 (the position) completes a vertex of */
	BOOL immediate;
	D3DPRIMITIVETYPE immediate_primitive;
	float vertex_registers[VERTEX_REGISTER_COUNT][4];
	unsigned long immediate_vertices;
	/* the immediate vertices so far: each the registers when its position
	was set */
	float (*immediate_buffer)[VERTEX_REGISTER_COUNT][4];
	/* the viewport's scale and offset (c[-38] and c[-37]) */
	float viewport_scale[4], viewport_offset[4];
	/* a frame begun on GX (by the first clear or draw after a Present) */
	BOOL frame_open;
	unsigned long draws_elsewhere;
	/* the frame's colour: the last Clear of the back buffer's */
	D3DCOLOR clear_color;
	BOOL visibility_test_active;
	BOOL gx_ready;
	struct draw_statistics statistics;
	unsigned long frames;
} device;
/* (changed with any of device.vertex_constants: constants_hash) */
static unsigned long constants_generation = 1;

/* each constant row's hash, two 32-bit lanes of its index and words, kept
as the rows are set (constant_rows_changed), and the exclusive or of every
row's: a set of rows is hashed by the exclusive or of theirs, a row at a
time (constants_hash), not a word at a time each draw */
static u32 constant_row_hashes[VERTEX_CONSTANT_COUNT][2];
static u32 constant_rows_all[2];
static BOOL constant_rows_ready;

static inline u32 hash_mix(u32 value)
{
	value ^= value >> 16;
	value *= 0x7FEB352DU;
	value ^= value >> 15;
	value *= 0x846CA68BU;
	value ^= value >> 16;
	return value;
}

static void constant_rows_changed(unsigned long first, unsigned long count)
{
	unsigned long row, end = first + count, word;

	if (!constant_rows_ready)
	{
		/* (every row the first time, the old hashes zero) */
		constant_rows_ready = TRUE;
		first = 0;
		end = VERTEX_CONSTANT_COUNT;
	}
	for (row = first; row < end && row < VERTEX_CONSTANT_COUNT; row++)
	{
		const u32 *words = (const u32 *)device.vertex_constants[row];
		u32 a = 0x9E3779B9U * (u32)(row + 1), b = 0x85EBCA6BU ^ (u32)row;

		for (word = 0; word < 4; word++)
		{
			a = (a ^ words[word]) * 0x01000193U;
			b = (b + words[word]) * 0xC2B2AE35U;
			b ^= b >> 13;
		}
		a = hash_mix(a);
		b = hash_mix(b ^ a);
		constant_rows_all[0] ^= constant_row_hashes[row][0] ^ a;
		constant_rows_all[1] ^= constant_row_hashes[row][1] ^ b;
		constant_row_hashes[row][0] = a;
		constant_row_hashes[row][1] = b;
	}
}

/* ---------- the vertical blank

A thread waits for each of the video interface's retraces, counts it,
wakes D3DDevice_BlockUntilVerticalBlank and calls the game's callback, as
the Xbox's interrupt does (the callback runs on that thread, not in the
interrupt, so it may do what the game's does). */

static pthread_mutex_t vertical_blank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vertical_blank_condition = PTHREAD_COND_INITIALIZER;
static unsigned long vertical_blank_count;
static D3DCALLBACK vertical_blank_callback;
static BOOL vertical_blank_running;
static volatile unsigned int flip_count;

static void *vertical_blank_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		D3DCALLBACK callback;

		VIDEO_WaitVSync();
		pthread_mutex_lock(&vertical_blank_lock);
		vertical_blank_count++;
		callback = vertical_blank_callback;
		pthread_cond_broadcast(&vertical_blank_condition);
		pthread_mutex_unlock(&vertical_blank_lock);
		if (callback)
			callback(0);
	}
	return NULL;
}

static void vertical_blank_start(void)
{
	pthread_mutex_lock(&vertical_blank_lock);
	if (!vertical_blank_running)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, vertical_blank_thread, NULL) == 0)
		{
			pthread_detach(thread);
			vertical_blank_running = TRUE;
		}
		else
		{
			platform_log("cannot start the vertical blank thread");
		}
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* replaces main/d3d_intimacy.cpp, which reads the counter out of the Xbox
Direct3D runtime's private device structure */
volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	pthread_mutex_lock(&vertical_blank_lock);
	vertical_blank_callback = callback;
	pthread_mutex_unlock(&vertical_blank_lock);
	vertical_blank_start();
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	unsigned long count;

	vertical_blank_start();
	pthread_mutex_lock(&vertical_blank_lock);
	count = vertical_blank_count;
	while (vertical_blank_count == count)
		pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* ---------- the viewport constants

Direct3D keeps c[-38] and c[-37], which map clip space to the screen, from
the viewport (unless the shader constant mode reserves none): the programs
end by converting their position with them */

static void viewport_update_constants(void)
{
	/* the depth buffer's range: the EFB's 24 bits */
	const float zscale = 16777215.0f;

	device.viewport_scale[0] = device.viewport.Width * 0.5f;
	device.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	device.viewport_scale[2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	device.viewport_scale[3] = 0.0f;
	device.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f;
	device.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f;
	device.viewport_offset[2] = zscale * device.viewport.MinZ;
	device.viewport_offset[3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		memcpy(device.vertex_constants[VERTEX_CONSTANT_BIAS - 38], device.viewport_scale, sizeof(device.viewport_scale));
		memcpy(device.vertex_constants[VERTEX_CONSTANT_BIAS - 37], device.viewport_offset, sizeof(device.viewport_offset));
		constant_rows_changed(VERTEX_CONSTANT_BIAS - 38, 2);
		constants_generation++;
	}
}

/* ---------- the device */

/* a surface's header as d3d8_resources.c's d3d8_surface_initialize writes
one, but with no memory (Data 0) */
static void surface_describe(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height,
	unsigned long bytes_per_texel)
{
	unsigned long pitch = (width * bytes_per_texel + D3DTEXTURE_PITCH_ALIGNMENT - 1) &
		~(unsigned long)(D3DTEXTURE_PITCH_ALIGNMENT - 1);

	memset(surface, 0, sizeof(*surface));
	/* owned by the device: without D3DCOMMON_D3DCREATED, Release never frees it */
	surface->Common = D3DCOMMON_TYPE_SURFACE | 1;
	surface->Data = 0;
	surface->Format = ((DWORD)format << D3DFORMAT_FORMAT_SHIFT) | (2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_DMACHANNEL_A;
	surface->Size = ((pitch / D3DTEXTURE_PITCH_ALIGNMENT - 1) << D3DSIZE_PITCH_SHIFT) |
		((height - 1) << D3DSIZE_HEIGHT_SHIFT) | (width - 1);
}

static D3DDevice *device_pointer(void)
{
	return (D3DDevice *)&device;
}

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

static void kept_start(void);

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		memset(&device, 0, sizeof(device));
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		/* the Xbox's 640 by 480, which the Wii's EFB is too: the device's
		own targets are the EFB's colour and depth, and have no memory in the
		contiguous window (which has 2.4 MB fewer to spare than the Xbox's) */
		surface_describe(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, SCREEN_WIDTH, SCREEN_HEIGHT, 4);
		surface_describe(&device.depth_buffer, D3DFMT_LIN_D24S8, SCREEN_WIDTH, SCREEN_HEIGHT, 4);
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.viewport.Width = SCREEN_WIDTH;
		device.viewport.Height = SCREEN_HEIGHT;
		device.viewport.MaxZ = 1.0f;
		device.next_vertex_shader_id = 1;
		d3d8_state_defaults();
		viewport_update_constants();
		device.immediate_buffer = malloc(MAXIMUM_IMMEDIATE_VERTICES * sizeof(*device.immediate_buffer));

		/* the video mode and GX (the text console of the start-up goes), the
		frame buffer with an alpha: the game blends by what it leaves there */
		wl_gx_set_efb_alpha(TRUE);
		device.gx_ready = wii_video_init() == WL_OK && wl_gx_init() == WL_OK;
		if (!device.gx_ready)
			platform_log("Direct3D: GX did not start (nothing is displayed)");
		else
		{
			/* the back buffer is the Xbox's 640 by 480 of the EFB, which is
			what render targets park and put back; parked where there is room */
			wl_gx_target_set_back_buffer_size(SCREEN_WIDTH, SCREEN_HEIGHT);
			wl_gx_target_set_allocator(efb_park_alloc, efb_park_free);
			/* (the kept buffers give their memory back to the game's blocks) */
			kept_start();
			/* (the transform unit's flush: gx_begin) */
			xf_flush_on = config_boolean("debug.gpu_xf_flush_safe") ? TRUE : FALSE;
			GX_SetMisc(GX_MT_XF_FLUSH, xf_flush_on ? GX_XF_FLUSH_SAFE : GX_XF_FLUSH_NONE);
			platform_log("Direct3D: the transform unit flushed after copies and tokens: %s",
				xf_flush_on ? "yes" : "no (debug.gpu_xf_flush_safe)");
		}
		device.created = TRUE;
		platform_log("Direct3D: device created, %dx%d", SCREEN_WIDTH, SCREEN_HEIGHT);
	}
	*returned_device = device_pointer();
	return S_OK;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	/* the Xbox's: what the game was written against */
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	/* like Direct3D, the caller gets a reference it must release */
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

/* a surface's size: its Size word's (linear), or its format word's
(swizzled, as a texture's level 0) */
static void surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	*width = description.width;
	*height = description.height;
}

static void draw_log_target(void);

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
	draw_log_target();
	/* like Direct3D, reset the viewport to the whole new target */
	if (device.render_target)
	{
		unsigned long width, height;

		surface_dimensions(device.render_target, &width, &height);
		device.viewport.X = 0;
		device.viewport.Y = 0;
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
	viewport_update_constants();
}

/* ---------- GPU synchronisation: the device finishes each draw before it returns */

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	/* the "GPU" reaches the callback immediately */
	if (callback)
		callback(context);
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}

/* ---------- visibility (occlusion) tests: nothing is drawn yet, so
nothing is visible, as with the OpenGL device's null renderer */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	device.visibility_test_active = TRUE;
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	(void)index;
	device.visibility_test_active = FALSE;
	return S_OK;
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	(void)index;
	if (result)
		*result = 0;
	if (time_stamp)
		*time_stamp = 0;
	return S_OK;
}

/* ---------- textures */

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.palettes[stage] = palette;
}

/* ---------- vertex shaders */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

/* the registers a declaration fills from which stream, at which offset
(as port/linux/src/d3d8_gl.c parses them) */
static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				/* skip: the count is in dwords, or in bytes with bit 27 */
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream & 15] += (token & 0x08000000) ? count : count * 4;
			}
			else if (object->element_count < NV2A_CPU_INPUT_COUNT)
			{
				struct vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream & 15];
				offsets[stream & 15] += element->bytes;
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	object->id = device.next_vertex_shader_id++;
	parse_declaration(object, declaration);
	if (function)
	{
		/* header: program type in the low word, instruction count in the high */
		object->instruction_count = function[0] >> 16;
		object->instructions = malloc(object->instruction_count * 4 * sizeof(DWORD));
		if (object->instructions)
			memcpy(object->instructions, function + 1, object->instruction_count * 4 * sizeof(DWORD));
	}
	/* odd values are FVF codes; programmable shader handles are even */
	*handle = (DWORD)object;
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

/* the game names its model lighting programs as it creates them; the GL
device lights their draws for each pixel. The Wii lights for each vertex. */
void halo_vertex_shader_lighting(unsigned long handle)
{
	(void)handle;
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	/* programs stay; the object is small */
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
	{
		device.vertex_shader = object;
		device.program_address = 0;
		device.program_slots[0] = object;
	}
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
		device.vertex_shader = object;
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > VERTEX_CONSTANT_COUNT)
		constant_count = (DWORD)(VERTEX_CONSTANT_COUNT - first);
	memcpy(device.vertex_constants[first], constant_data, constant_count * 4 * sizeof(float));
	constant_rows_changed((unsigned long)first, constant_count);
	constants_generation++;
}

/* ---------- GX's transform unit kept from hanging

GX's transform unit can hang (and GX_DrawDone, or the frame's token, never
comes) when a primitive it clips follows a change of the raster state such
as an EFB copy, a draw-sync token or a draw-done. libogc names the remedy,
GX_SetMisc(GX_MT_XF_FLUSH, GX_XF_FLUSH_SAFE), which the device sets, but its
GX_Begin does nothing with it: GX_SetMisc keeps the value and marks the
vertex descriptor to be sent again, and GX_Begin only sends that dirty state
and the primitive's opcode and count (libogc.a's gx.o, disassembled). So
the device sends the flush itself (debug.gpu_xf_flush_safe, on by default):
after one of those changes (xf_flush_after), its next primitive (gx_begin)
is first given a strip of XF_FLUSH_VERTICES vertices of zeros in the
primitive's own format, which are all one point and draw nothing. A vertex's
size is libogc's vertex descriptor (GX_GetVtxDescv, what GX_Begin sends) and
the bytes of each direct attribute as the device set its format
(vertex_format); where an attribute's format is unknown, no flush is sent
and the log says so once. */

#define XF_FLUSH_VERTICES 8

/* the bytes of each attribute sent directly, by vertex format, as the
device set them (0: not set by it) */
static unsigned char vertex_format_bytes[8][GX_VA_MAXATTR];
/* (whether a flush is due; how many were sent) */
static BOOL xf_flush_due;
static unsigned long xf_flushes;

static void vertex_format(u8 format, u32 attribute, u32 count, u32 type, u32 fraction)
{
	/* (GX_U8, GX_S8, GX_U16, GX_S16, GX_F32; GX_RGB565 to GX_RGBA8) */
	static const unsigned char component_bytes[5] = { 1, 1, 2, 2, 4 };
	static const unsigned char color_bytes_of[6] = { 2, 3, 4, 2, 3, 4 };
	unsigned bytes = 0;

	GX_SetVtxAttrFmt(format, attribute, count, type, fraction);
	if (attribute == GX_VA_CLR0 || attribute == GX_VA_CLR1)
		bytes = type < 6 ? color_bytes_of[type] : 0;
	else if (type < 5)
	{
		if (attribute == GX_VA_POS)
			bytes = (count == GX_POS_XY ? 2 : 3) * component_bytes[type];
		else if (attribute == GX_VA_NRM)
			bytes = count == GX_NRM_XYZ ? 3 * component_bytes[type] : 0;
		else if (attribute >= GX_VA_TEX0 && attribute <= GX_VA_TEX7)
			bytes = (count == GX_TEX_S ? 1 : 2) * component_bytes[type];
	}
	if (attribute < GX_VA_MAXATTR)
		vertex_format_bytes[format & 7][attribute] = (unsigned char)bytes;
}

/* the bytes of a vertex of format as GX_Begin will send them; 0 where an
attribute's size is not known */
static unsigned vertex_bytes(u8 format)
{
	GXVtxDesc descriptor[GX_MAX_VTXDESC_LISTSIZE];
	unsigned index, bytes = 0;

	GX_GetVtxDescv(descriptor);
	for (index = 0; index < GX_MAX_VTXDESC_LISTSIZE && descriptor[index].attr != GX_VA_NULL; index++)
	{
		u8 attribute = descriptor[index].attr, type = descriptor[index].type;

		if (type == GX_NONE)
			continue;
		/* (a matrix index is a byte; an index one or two) */
		if (attribute <= GX_VA_TEX7MTXIDX || type == GX_INDEX8)
			bytes += 1;
		else if (type == GX_INDEX16)
			bytes += 2;
		else if (attribute < GX_VA_MAXATTR && vertex_format_bytes[format & 7][attribute])
			bytes += vertex_format_bytes[format & 7][attribute];
		else
			return 0;
	}
	return bytes;
}

/* a raster state change (an EFB copy, a token, a draw-done): the next
primitive is flushed first */
static void xf_flush_after(void)
{
	xf_flush_due = TRUE;
}

/* GX misreads a vertex whose descriptor holds only its position (indexed)
and matrix indices: a draw of the device whose vertices carry a matrix
index sends another attribute with it, a colour no channel reads where it
needs none (draw_hardware's pad_color, draw_run's EMIT_PAD; a skinned
model's pass whose stages read no texture coordinate from its vertices and
whose colours are constant). Checked before each primitive in a build with
assertions, whether the position is indexed or direct. */
#ifndef HALO_RELEASE
static void vertex_shape_check(void)
{
	GXVtxDesc descriptor[GX_MAX_VTXDESC_LISTSIZE];
	unsigned index, others = 0;
	BOOL position = FALSE, matrices = FALSE;

	GX_GetVtxDescv(descriptor);
	for (index = 0; index < GX_MAX_VTXDESC_LISTSIZE && descriptor[index].attr != GX_VA_NULL; index++)
	{
		if (descriptor[index].type == GX_NONE)
			continue;
		if (descriptor[index].attr <= GX_VA_TEX7MTXIDX)
			matrices = TRUE;
		else if (descriptor[index].attr == GX_VA_POS)
			position = TRUE;
		else
			others++;
	}
	if (position && matrices && !others)
		platform_log("Direct3D: a vertex of its position and matrix indices alone, which GX misreads");
	WL_ASSERT(!position || !matrices || others);
}
#endif

/* the flush before a primitive of format, if one is due (gx_begin; before
a display list's call) */
static void gx_flush_pending(u8 format)
{
#ifndef HALO_RELEASE
	vertex_shape_check();
#endif
	if (xf_flush_due && xf_flush_on)
	{
		unsigned long bytes = (unsigned long)vertex_bytes(format) * XF_FLUSH_VERTICES;

		if (bytes)
		{
			GX_Begin(GX_TRIANGLESTRIP, format, XF_FLUSH_VERTICES);
			for (; bytes >= 4; bytes -= 4)
				GX_Color1u32(0);
			for (; bytes; bytes--)
				GX_Position1x8(0);
			GX_End();
			xf_flushes++;
		}
		else
		{
			static BOOL logged;

			if (!logged)
			{
				logged = TRUE;
				platform_log("Direct3D: no flush before a primitive of format %u: an attribute's size is not known",
					(unsigned)format);
			}
		}
	}
	xf_flush_due = FALSE;
}

/* GX_Begin, after the flush if one is due */
static void gx_begin(u8 primitive, u8 format, u16 count)
{
	gx_flush_pending(format);
	GX_Begin(primitive, format, count);
}

/* where GX stood when it did not finish in time (WiiLibs' 250 ms wait): its
status flags, the FIFO's read and write pointers and fill, and the last
token it passed, written out at once (a hung console may not log again) */
static void gx_stall_log(const char *where)
{
	GXFifoObj gp, cpu;
	void *gp_read = NULL, *gp_write = NULL, *cpu_read = NULL, *cpu_write = NULL;
	u8 over_high, under_low, read_idle, command_idle, breakpoint;

	GX_GetGPStatus(&over_high, &under_low, &read_idle, &command_idle, &breakpoint);
	GX_GetGPFifo(&gp);
	GX_GetCPUFifo(&cpu);
	GX_GetFifoPtrs(&gp, &gp_read, &gp_write);
	GX_GetFifoPtrs(&cpu, &cpu_read, &cpu_write);
	platform_log("Direct3D: frame %lu: GX did not finish in time (%s): over the high mark %u, under the low mark %u, "
		"reading idle %u, commands idle %u, at a breakpoint %u; the GP's FIFO read %p, write %p, %lu bytes in it; the "
		"CPU's FIFO write %p; the last token passed %u; %lu flushes sent",
		device.frames, where, over_high, under_low, read_idle, command_idle, breakpoint, gp_read, gp_write,
		(unsigned long)GX_GetFifoCount(&gp), cpu_write, (unsigned)GX_GetDrawSync(), xf_flushes);
	wl_platform_log_flush();
}

/* ---------- the frame on GX

A frame is begun on GX by the first clear or draw after a Present (cleared
to the clear's colour), and ended, copied and shown by the Present. The
device sets GX itself, around WiiLibs' frame (wl_gx_invalidate_shadow): an
orthographic projection over the screen's pixels, the vertex format below,
the colours as the vertices give them. */

static BOOL projection_valid;
static void cull_measure(void);

/* whether the last draw left GX transforming the game's own vertices
(draw_hardware): its matrices, texture coordinate generation, arrays and
colour sources; transform_software puts back what the draws in screen
space (draw_run, the clears, the render targets' copies) take */
static BOOL transform_hardware;
/* (the draw's vertices carry a position matrix index: draw_keys) */
static BOOL emit_matrix_index;
/* the attributes a processor's draw sends (emit_attributes_set): bit 0 and 1
the colours, 2 to 5 the texture coordinates, 6 the fog; all of them unless
a draw says fewer; EMIT_PAD, where it sends none of them but a matrix index,
a colour of 0 in their place, which no channel reads (vertex_shape_check) */
#define EMIT_ALL 0x7F
#define EMIT_PAD 0x80
static unsigned emit_attributes = EMIT_ALL;

/* the pixel engine's state states_apply set last (blend, colour and alpha
updates, depth, alpha test and the depth test's place, culling): each is
written only when it changes, a write of the same value being a pipeline
flush on GX for nothing. Unknown (states_shadow_reset) after anything else
set them: a frame's beginning, WiiLibs' quads (a target's put back, a
clear), a view's sphere map, the cull's measure. */
static struct
{
	BOOL known;
	u8 blend[4], color_update, alpha_update, z[3], alpha_compare[3], z_before, cull;
} states_shadow;

static void states_shadow_reset(void)
{
	states_shadow.known = FALSE;
}

static void transform_software(void)
{
	if (!transform_hardware)
		return;
	transform_hardware = FALSE;
	emit_matrix_index = FALSE;
	emit_attributes = EMIT_ALL;
	GX_SetCurrentMtx(GX_PNMTX0);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX2, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX3, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX4, GX_DIRECT);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX2, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD3, GX_TG_MTX2x4, GX_TG_TEX3, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD4, GX_TG_MTX2x4, GX_TG_TEX4, GX_IDENTITY);
	/* (a draw in hardware sets only the coordinates and channels its stages
	read: draw_hardware) */
	GX_SetNumTexGens(5);
	GX_SetNumChans(2);
}

static void frame_begin(D3DCOLOR clear_color)
{
	Mtx identity;

	if (device.frame_open || !device.gx_ready)
		return;
	/* ARGB to the RGBA WiiLibs takes (and the back buffer has the EFB) */
	wl_gx_begin_frame((clear_color << 8) | (clear_color >> 24));
	states_shadow_reset();
	device.frame_open = TRUE;
	/* (the last frame's display copy, token and draw-done, and this one's
	clearing copy) */
	xf_flush_after();

	/* (x and y in pixels from the top left, the depth given as z - 1: GX's
	clip-space depth runs from -1, near, to 0, far; draw_run loads it) */
	projection_valid = FALSE;
	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	/* (and GX_IDENTITY, the texture matrix the draws in screen space read
	their coordinates through, as GX_Init left it: nothing else may load it,
	but a frame begins with it right whatever did) */
	GX_LoadTexMtxImm(identity, GX_IDENTITY, GX_MTX3x4);

	vertex_format(DEVICE_VERTEX_FORMAT, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	vertex_format(DEVICE_VERTEX_FORMAT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	vertex_format(DEVICE_VERTEX_FORMAT, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
	vertex_format(DEVICE_VERTEX_FORMAT, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	vertex_format(DEVICE_VERTEX_FORMAT, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
	vertex_format(DEVICE_VERTEX_FORMAT, GX_VA_TEX2, GX_TEX_ST, GX_F32, 0);
	vertex_format(DEVICE_VERTEX_FORMAT, GX_VA_TEX3, GX_TEX_ST, GX_F32, 0);
	/* (the fog factor, on the fog ramp: fog_coordinate) */
	vertex_format(DEVICE_VERTEX_FORMAT, GX_VA_TEX4, GX_TEX_S, GX_F32, 0);
	GX_SetNumChans(2);
	GX_SetNumTexGens(5);
	transform_hardware = TRUE;
	transform_software();
	GX_SetViewport(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 1.0f);
	GX_SetScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	{
		static BOOL cull_measured;

		if (!cull_measured)
		{
			cull_measured = TRUE;
			cull_measure();
		}
	}
}

/* ---------- render states to GX */

/* GX's cull mode that discards what is clockwise on the screen: measured
once (cull_measure), as GX's names for its windings are not the screen's */
static u8 gx_cull_clockwise = GX_CULL_FRONT;

/* a small clockwise triangle drawn at the EFB's corner with GX_CULL_FRONT,
and the pixel read back: drawn, then GX_CULL_FRONT keeps clockwise and
discards counterclockwise */
static void cull_measure(void)
{
	static const float corner[3][2] = { { 0.0f, 0.0f }, { 16.0f, 0.0f }, { 0.0f, 16.0f } };
	Mtx44 matrix;
	GXColor read = { 0, 0, 0, 0 };
	int pass, vertex, stage;

	memset(matrix, 0, sizeof(matrix));
	matrix[0][0] = 2.0f / SCREEN_WIDTH;
	matrix[0][3] = -1.0f;
	matrix[1][1] = -2.0f / SCREEN_HEIGHT;
	matrix[1][3] = 1.0f;
	matrix[2][2] = 1.0f;
	matrix[3][3] = 1.0f;
	GX_LoadProjectionMtx(matrix, GX_ORTHOGRAPHIC);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	/* black, unculled; then white, clockwise, with GX_CULL_FRONT */
	for (pass = 0; pass < 2; pass++)
	{
		GX_SetCullMode(pass ? GX_CULL_FRONT : GX_CULL_NONE);
		transform_software();

		gx_begin(GX_TRIANGLES, DEVICE_VERTEX_FORMAT, 3);
		for (vertex = 0; vertex < 3; vertex++)
		{
			u8 value = pass ? 255 : 0;

			GX_Position3f32(corner[vertex][0], corner[vertex][1], -0.5f);
			GX_Color4u8(value, value, value, 255);
			GX_Color4u8(value, value, value, 255);
			for (stage = 0; stage < 4; stage++)
				GX_TexCoord2f32(0.0f, 0.0f);
			GX_TexCoord1f32(1.0f);
		}
		GX_End();
	}
	GX_SetCullMode(GX_CULL_NONE);
	states_shadow_reset();
	GX_DrawDone();
	xf_flush_after();
	GX_PeekARGB(2, 2, &read);
	gx_cull_clockwise = read.r > 128 ? GX_CULL_BACK : GX_CULL_FRONT;
	platform_log("Direct3D: GX_CULL_FRONT %s clockwise triangles (read back %u): clockwise is culled by %s",
		read.r > 128 ? "keeps" : "discards", read.r, read.r > 128 ? "GX_CULL_BACK" : "GX_CULL_FRONT");
	projection_valid = FALSE;
}

static u8 gx_compare(DWORD function)
{
	switch (function)
	{
	case D3DCMP_NEVER: return GX_NEVER;
	case D3DCMP_LESS: return GX_LESS;
	case D3DCMP_EQUAL: return GX_EQUAL;
	case D3DCMP_LESSEQUAL: return GX_LEQUAL;
	case D3DCMP_GREATER: return GX_GREATER;
	case D3DCMP_NOTEQUAL: return GX_NEQUAL;
	case D3DCMP_GREATEREQUAL: return GX_GEQUAL;
	default: return GX_ALWAYS;
	}
}

static u8 gx_blend_factor(DWORD factor, BOOL source)
{
	switch (factor)
	{
	case D3DBLEND_ZERO: return GX_BL_ZERO;
	case D3DBLEND_ONE: return GX_BL_ONE;
	/* (GX's colour factors are the other side's: a source's factor of the
	destination colour, a destination's of the source colour) */
	case D3DBLEND_SRCCOLOR: return source ? GX_BL_ONE : GX_BL_SRCCLR;
	case D3DBLEND_INVSRCCOLOR: return source ? GX_BL_ZERO : GX_BL_INVSRCCLR;
	case D3DBLEND_SRCALPHA: case D3DBLEND_SRCALPHASAT: return GX_BL_SRCALPHA;
	case D3DBLEND_INVSRCALPHA: return GX_BL_INVSRCALPHA;
	case D3DBLEND_DESTALPHA: return GX_BL_DSTALPHA;
	case D3DBLEND_INVDESTALPHA: return GX_BL_INVDSTALPHA;
	case D3DBLEND_DESTCOLOR: return source ? GX_BL_DSTCLR : GX_BL_ONE;
	case D3DBLEND_INVDESTCOLOR: return source ? GX_BL_INVDSTCLR : GX_BL_ZERO;
	/* (no constant colour on GX's blender) */
	default: return GX_BL_ONE;
	}
}

static void states_apply(void)
{
	static int cull_on = -1;
	DWORD write = D3D__RenderState[D3DRS_COLORWRITEENABLE];
	u8 blend[4], color_update, alpha_update, z[3], alpha_compare[3], z_before, cull;
	BOOL known = states_shadow.known;

	if (D3D__RenderState[D3DRS_ALPHABLENDENABLE])
	{
		DWORD operation = D3D__RenderState[D3DRS_BLENDOP];

		blend[0] = operation == D3DBLENDOP_REVSUBTRACT ? GX_BM_SUBTRACT : GX_BM_BLEND;
		blend[1] = gx_blend_factor(D3D__RenderState[D3DRS_SRCBLEND], TRUE);
		blend[2] = gx_blend_factor(D3D__RenderState[D3DRS_DESTBLEND], FALSE);
	}
	else
	{
		blend[0] = GX_BM_NONE;
		blend[1] = GX_BL_ONE;
		blend[2] = GX_BL_ZERO;
	}
	blend[3] = GX_LO_CLEAR;
	color_update = (write & (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE)) ?
		GX_TRUE : GX_FALSE;
	alpha_update = (write & D3DCOLORWRITEENABLE_ALPHA) ? GX_TRUE : GX_FALSE;
	/* (no depth buffer: no depth test, as Direct3D) */
	if (device.depth_stencil)
	{
		z[0] = D3D__RenderState[D3DRS_ZENABLE] ? GX_TRUE : GX_FALSE;
		z[1] = gx_compare(D3D__RenderState[D3DRS_ZFUNC]);
		z[2] = D3D__RenderState[D3DRS_ZWRITEENABLE] ? GX_TRUE : GX_FALSE;
	}
	else
	{
		z[0] = GX_FALSE;
		z[1] = GX_ALWAYS;
		z[2] = GX_FALSE;
	}
	if (D3D__RenderState[D3DRS_ALPHATESTENABLE])
	{
		alpha_compare[0] = gx_compare(D3D__RenderState[D3DRS_ALPHAFUNC]);
		alpha_compare[1] = (u8)D3D__RenderState[D3DRS_ALPHAREF];
		/* (the test before the depth write) */
		z_before = GX_FALSE;
	}
	else
	{
		alpha_compare[0] = GX_ALWAYS;
		alpha_compare[1] = 0;
		z_before = GX_TRUE;
	}
	alpha_compare[2] = 0;
	/* the cull mode names the screen winding to discard (as the OpenGL
	port reads it); the vertices keep their screen winding on both paths */
	if (cull_on < 0)
		cull_on = config_boolean("debug.gpu_cull") ? 1 : 0;
	switch (cull_on ? D3D__RenderState[D3DRS_CULLMODE] : D3DCULL_NONE)
	{
	case D3DCULL_CW: cull = gx_cull_clockwise; break;
	case D3DCULL_CCW: cull = gx_cull_clockwise == GX_CULL_FRONT ? GX_CULL_BACK : GX_CULL_FRONT; break;
	default: cull = GX_CULL_NONE; break;
	}
	/* (each written where it changed) */
	if (!known || memcmp(blend, states_shadow.blend, sizeof(blend)))
		GX_SetBlendMode(blend[0], blend[1], blend[2], blend[3]);
	if (!known || color_update != states_shadow.color_update)
		GX_SetColorUpdate(color_update);
	if (!known || alpha_update != states_shadow.alpha_update)
		GX_SetAlphaUpdate(alpha_update);
	if (!known || memcmp(z, states_shadow.z, sizeof(z)))
		GX_SetZMode(z[0], z[1], z[2]);
	if (!known || memcmp(alpha_compare, states_shadow.alpha_compare, sizeof(alpha_compare)))
		GX_SetAlphaCompare(alpha_compare[0], alpha_compare[1], GX_AOP_AND, GX_ALWAYS, alpha_compare[2]);
	if (!known || z_before != states_shadow.z_before)
		GX_SetZCompLoc(z_before);
	if (!known || cull != states_shadow.cull)
		GX_SetCullMode(cull);
	memcpy(states_shadow.blend, blend, sizeof(blend));
	states_shadow.color_update = color_update;
	states_shadow.alpha_update = alpha_update;
	memcpy(states_shadow.z, z, sizeof(z));
	memcpy(states_shadow.alpha_compare, alpha_compare, sizeof(alpha_compare));
	states_shadow.z_before = z_before;
	states_shadow.cull = cull;
	states_shadow.known = TRUE;
}

/* ---------- textures

The maps' bitmaps are converted to GX's formats ahead of time
(tools/wii_gx_textures.py): a texture whose pixels the texture cache read
(xbox_texture_cache.c) is bound where it is, its levels those whose GX bytes
fit in the Xbox texture's (gx_native_levels, as the converter counts them).
Any other texture, and a bitmap the converter left as it was, is decoded
from its Xbox format (port/linux/src/xbox_texture_formats.c) into GX's RGBA8,
its first level only, and kept by its header (Data, Format, Size) while the
cache has room. */

/* the channels of a texel GX's format holds (I8 reads as all four) */
enum
{
	_channels_all,
	/* luminance: opaque */
	_channels_color,
	/* alpha: white */
	_channels_alpha,
};

struct texture_entry
{
	DWORD data, format, size;
	unsigned long width, height, bytes;
	void *texels;
	unsigned long used_frame;
	/* a converted bitmap's: in the texture cache, not the heap */
	BOOL native;
	u8 gx_format;
	unsigned char levels, channels;
	/* DXT3 and DXT5: the colour in CMPR, the alpha in I4 after it */
	void *alpha_texels;
	BOOL linear;
	/* a cube map the converter made a sphere map (sphere_coordinates) */
	BOOL sphere;
	/* a decoded texture's pixels when it was decoded, hashed: the game
	rewrites some (its text's glyph cache) */
	unsigned long hash, source_bytes;
	/* (a decoded texture outside the map's texture cache: one the game made
	and writes, hashed once a frame as well as when it is locked) */
	BOOL outside;
	/* a number given each time the entry is filled (textures_bind keeps a
	stage's texture object while it is the same) */
	unsigned long serial;
	/* the next entry in its bucket, plus one (texture_lookup) */
	unsigned short next;
};

struct gx_native_format
{
	u8 gx_format;
	unsigned char tile_width, tile_height, tile_bytes;
	unsigned char channels;
	BOOL dual;
};

static struct texture_entry texture_cache[TEXTURE_CACHE_ENTRIES];
static unsigned long texture_cache_bytes;
/* the entries by their data's address (each the first entry plus one) */
static unsigned short texture_buckets[TEXTURE_BUCKETS];
static unsigned long texture_serial;
/* GX's texture cache (TMEM) holds texels by their address: it is
invalidated before a draw only when memory GX reads textures from has
changed since the last time (a decode, a converted bitmap loaded again, a
copy out of the EFB) and at each frame's start, not at every bind */
static BOOL texture_memory_changed = TRUE;
/* what the textures cost, the last STATISTICS_FRAMES frames (the log) */
static struct
{
	unsigned long marked, looked_up, created, hashed, hashed_bytes, decoded, flushed, flushed_bytes,
		invalidations, objects;
} texture_counts;
/* why textures were not drawn (logged with the frame statistics) */
static unsigned long texture_failures[5];
enum { _failure_no_data, _failure_shape, _failure_memory, _failure_decode, _failure_format };
static unsigned long textures_native;
/* the decoded textures given up so that another could be decoded, the heap
being full (texture_cache_reclaim; the memory report) */
static unsigned long textures_reclaimed, texture_reclaim_bytes;

void *physical_memory_get_texture_cache_base_address(void);

/* the GX format a converted bitmap's Direct3D format became (the
converter's table) */
static BOOL gx_native_format(DWORD format, struct gx_native_format *out)
{
	memset(out, 0, sizeof(*out));
	switch (format)
	{
	case 0x06: case 0x12: case 0x07: case 0x1E:
		*out = (struct gx_native_format){ GX_TF_RGBA8, 4, 4, 64, _channels_all, FALSE };
		return TRUE;
	case 0x05: case 0x11:
		*out = (struct gx_native_format){ GX_TF_RGB565, 4, 4, 32, _channels_all, FALSE };
		return TRUE;
	case 0x02: case 0x10: case 0x04: case 0x1D:
		*out = (struct gx_native_format){ GX_TF_RGB5A3, 4, 4, 32, _channels_all, FALSE };
		return TRUE;
	case 0x00: case 0x13:
		*out = (struct gx_native_format){ GX_TF_I8, 8, 4, 32, _channels_color, FALSE };
		return TRUE;
	case 0x19: case 0x1F:
		*out = (struct gx_native_format){ GX_TF_I8, 8, 4, 32, _channels_alpha, FALSE };
		return TRUE;
	case 0x01: case 0x1B:
		*out = (struct gx_native_format){ GX_TF_I8, 8, 4, 32, _channels_all, FALSE };
		return TRUE;
	case 0x1A: case 0x20:
		*out = (struct gx_native_format){ GX_TF_IA8, 4, 4, 32, _channels_all, FALSE };
		return TRUE;
	case 0x0C:
		*out = (struct gx_native_format){ GX_TF_CMPR, 8, 8, 32, _channels_all, FALSE };
		return TRUE;
	case 0x0E: case 0x0F:
		*out = (struct gx_native_format){ GX_TF_CMPR, 8, 8, 64, _channels_all, TRUE };
		return TRUE;
	case 0x0B:
		/* (P8: the indices; the palette is a TLUT, palette_tlut) */
		*out = (struct gx_native_format){ GX_TF_CI8, 8, 4, 32, _channels_all, FALSE };
		return TRUE;
	default:
		return FALSE;
	}
}

static unsigned long gx_native_level_bytes(const struct gx_native_format *format, unsigned long width,
	unsigned long height, unsigned long level)
{
	unsigned long w = xgpu_texture_level_dimension(width, level);
	unsigned long h = xgpu_texture_level_dimension(height, level);

	return ((w + format->tile_width - 1) / format->tile_width) * ((h + format->tile_height - 1) / format->tile_height) *
		format->tile_bytes;
}

/* the levels a converted texture keeps: the most whose GX bytes fit in the
Xbox texture's (tools/wii_gx_textures.py's gx_levels) */
static unsigned long gx_native_levels(const struct xgpu_texture_description *description,
	const struct gx_native_format *format)
{
	unsigned long budget = 0, used = 0, kept = 0, level;

	if (description->width > 1024 || description->height > 1024)
		return 0;
	for (level = 0; level < description->levels; level++)
		budget += xgpu_texture_level_bytes(description, level);
	for (level = 0; level < description->levels; level++)
	{
		used += gx_native_level_bytes(format, description->width, description->height, level);
		if (used > budget)
			break;
		kept = level + 1;
	}
	return kept;
}

/* a palette as a GX TLUT, for a converted P8 bitmap (CI8): RGB565 when
every entry is opaque (the game's one palette, global_vector_palette's
normals), RGB5A3 otherwise. Kept by the palette's data and hashed once a
frame; loaded into the stage's TLUT when it differs from what is there. */
struct palette_tlut
{
	DWORD data;
	unsigned long hash, hashed_frame;
	u8 format;
	u16 entries[256] __attribute__((aligned(32)));
};

static struct palette_tlut palette_tluts[4] __attribute__((aligned(32)));
static const struct palette_tlut *loaded_tluts[D3DTSS_MAXSTAGES];
static unsigned long loaded_tlut_hashes[D3DTSS_MAXSTAGES];

static unsigned long palette_hash(const D3DCOLOR *colors)
{
	unsigned long hash = 2166136261UL, index;

	for (index = 0; index < 256; index++)
		hash = (hash ^ colors[index]) * 16777619UL;
	return hash;
}

static const struct palette_tlut *palette_tlut(const D3DPalette *palette)
{
	const D3DCOLOR *colors;
	struct palette_tlut *tlut = NULL;
	unsigned long hash, index;
	BOOL opaque = TRUE;

	if (!palette || !palette->Data)
		return NULL;
	for (index = 0; index < sizeof(palette_tluts) / sizeof(palette_tluts[0]); index++)
		if (palette_tluts[index].data == palette->Data)
		{
			tlut = &palette_tluts[index];
			break;
		}
	if (tlut && tlut->hashed_frame == device.frames)
		return tlut;
	colors = (const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(palette->Data);
	hash = palette_hash(colors);
	if (tlut && tlut->hash == hash)
	{
		tlut->hashed_frame = device.frames;
		return tlut;
	}
	if (!tlut)
	{
		/* (a new palette: the least recently hashed one goes) */
		tlut = &palette_tluts[0];
		for (index = 1; index < sizeof(palette_tluts) / sizeof(palette_tluts[0]); index++)
			if (palette_tluts[index].hashed_frame < tlut->hashed_frame)
				tlut = &palette_tluts[index];
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
			if (loaded_tluts[index] == tlut)
				loaded_tluts[index] = NULL;
	}
	for (index = 0; index < 256; index++)
		if ((colors[index] >> 24) != 0xFF)
			opaque = FALSE;
	for (index = 0; index < 256; index++)
	{
		unsigned long c = colors[index], a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;

		if (opaque)
			tlut->entries[index] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		else if (a >= 0xE0)
			tlut->entries[index] = (u16)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
		else
			tlut->entries[index] = (u16)(((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
	}
	DCFlushRange(tlut->entries, sizeof(tlut->entries));
	tlut->data = palette->Data;
	tlut->format = opaque ? GX_TL_RGB565 : GX_TL_RGB5A3;
	tlut->hash = hash;
	tlut->hashed_frame = device.frames;
	return tlut;
}

/* whether the texture's pixels are in the texture cache (a map's bitmap) */
static BOOL texture_in_texture_cache(const D3DBaseTexture *texture)
{
	unsigned long base = (unsigned long)physical_memory_get_texture_cache_base_address();
	unsigned long address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(texture->Data);

	return base && address >= base && address < base + HALO_PORT_TEXTURE_CACHE_SIZE;
}

/* a texture's first level, hashed (FNV-1a by words) */
static unsigned long texture_hash(const D3DBaseTexture *texture, unsigned long bytes)
{
	const unsigned long *words = (const unsigned long *)PLATFORM_PHYSICAL_TO_VIRTUAL(texture->Data);
	unsigned long hash = 2166136261UL, index;

	for (index = 0; index < bytes / 4; index++)
		hash = (hash ^ words[index]) * 16777619UL;
	texture_counts.hashed++;
	texture_counts.hashed_bytes += bytes;
	return hash;
}

/* The device's mark in a texture's Lock, D3D's own field (no other code
reads it): the entry the device found the texture in, and that its pixels
are as the device last saw them through this header. The game sets Lock to
0 when it builds a header (a bitmap loaded into the map's texture cache:
xbox_texture_cache.c), and the resources' lock functions do when it is
about to write the pixels (d3d8_resources.c: the text's glyph cache), so a
texture without the mark is checked again: a converted bitmap's pixels
flushed to memory, a decoded one's hashed. */
#define TEXTURE_MARK 0x47580000UL
#define TEXTURE_MARK_MASK 0xFFFF0000UL

static unsigned texture_bucket(DWORD data)
{
	return (unsigned)((data * 2654435761UL) >> 23) & (TEXTURE_BUCKETS - 1);
}

/* an entry, filled, into its bucket */
static void texture_link(struct texture_entry *entry)
{
	unsigned bucket = texture_bucket(entry->data);

	entry->serial = ++texture_serial;
	entry->next = texture_buckets[bucket];
	texture_buckets[bucket] = (unsigned short)(entry - texture_cache + 1);
}

static void texture_unlink(struct texture_entry *entry)
{
	unsigned short *link = &texture_buckets[texture_bucket(entry->data)];
	unsigned short self = (unsigned short)(entry - texture_cache + 1);

	while (*link)
	{
		if (*link == self)
		{
			*link = entry->next;
			break;
		}
		link = &texture_cache[*link - 1].next;
	}
}

/* the entry of a texture's data, format and size */
static struct texture_entry *texture_lookup(const D3DBaseTexture *texture)
{
	unsigned short link = texture_buckets[texture_bucket(texture->Data)];

	texture_counts.looked_up++;
	while (link)
	{
		struct texture_entry *entry = &texture_cache[link - 1];

		if (entry->data == texture->Data && entry->format == texture->Format && entry->size == texture->Size)
			return entry;
		link = entry->next;
	}
	return NULL;
}

/* the entry to fill: an empty one, else the least recently drawn */
static struct texture_entry *texture_victim(void)
{
	struct texture_entry *oldest = &texture_cache[0];
	unsigned long index;

	for (index = 0; index < TEXTURE_CACHE_ENTRIES; index++)
	{
		struct texture_entry *candidate = &texture_cache[index];

		if (!candidate->texels)
			return candidate;
		if (candidate->used_frame < oldest->used_frame)
			oldest = candidate;
	}
	return oldest;
}

/* the pixels of textures evicted while a frame is open: GX may still read
them for the frame's draws, so they are freed once it has drawn them
(texture_frees_flush, at the frame's end) */
#define PENDING_TEXTURE_FREES 256
static void *pending_texture_frees[PENDING_TEXTURE_FREES];
static unsigned pending_texture_free_count;

static void texture_frees_flush(void)
{
	while (pending_texture_free_count)
		free(pending_texture_frees[--pending_texture_free_count]);
}

static void texture_evict(struct texture_entry *entry)
{
	if (entry->texels)
		texture_unlink(entry);
	if (entry->texels && !entry->native)
	{
		if (device.frame_open)
		{
			if (pending_texture_free_count == PENDING_TEXTURE_FREES)
			{
				GX_DrawDone();
				xf_flush_after();
				texture_frees_flush();
			}
			pending_texture_frees[pending_texture_free_count++] = entry->texels;
		}
		else
			free(entry->texels);
		texture_cache_bytes -= entry->bytes;
	}
	memset(entry, 0, sizeof(*entry));
}

/* With the heap full, decoded textures go so that the one being decoded can
be (the least recently drawn first, keep excepted): their memory is freed
now, once GX has drawn what it was given, not at the frame's end. TRUE if
any memory came back. */
static BOOL texture_cache_reclaim(unsigned long bytes, const struct texture_entry *keep)
{
	unsigned long freed = 0;
	BOOL any = pending_texture_free_count != 0;

	while (freed < 2 * bytes)
	{
		struct texture_entry *victim = NULL;
		unsigned long index;

		for (index = 0; index < TEXTURE_CACHE_ENTRIES; index++)
		{
			if (&texture_cache[index] != keep && texture_cache[index].texels && !texture_cache[index].native &&
				(!victim || texture_cache[index].used_frame < victim->used_frame))
				victim = &texture_cache[index];
		}
		if (!victim)
			break;
		freed += victim->bytes;
		textures_reclaimed++;
		texture_reclaim_bytes += victim->bytes;
		texture_evict(victim);
		any = TRUE;
	}
	if (any)
	{
		if (device.frame_open)
		{
			GX_DrawDone();
			xf_flush_after();
		}
		texture_frees_flush();
	}
	return any;
}

/* a converted bitmap's entry, bound where the texture cache read it */
static struct texture_entry *texture_native(struct texture_entry *entry, const D3DBaseTexture *texture,
	const struct xgpu_texture_description *description, const struct gx_native_format *format,
	unsigned long levels)
{
	unsigned char *texels = (unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(texture->Data);
	unsigned long bytes = 0, level;

	for (level = 0; level < levels; level++)
		bytes += gx_native_level_bytes(format, description->width, description->height, level);
	if (entry->texels)
		texture_evict(entry);
	entry->data = texture->Data;
	entry->format = texture->Format;
	entry->size = texture->Size;
	entry->width = description->width;
	entry->height = description->height;
	entry->bytes = bytes;
	entry->texels = texels;
	entry->native = TRUE;
	entry->gx_format = format->gx_format;
	entry->levels = (unsigned char)levels;
	entry->channels = format->channels;
	entry->alpha_texels = format->dual ? texels + bytes / 2 : NULL;
	entry->linear = description->linear;
	entry->used_frame = device.frames;
	texture_link(entry);
	textures_native++;
	return entry;
}

/* a converted bitmap's pixels, which the processor wrote as it read them
into the texture cache, to memory before GX reads them */
static void texture_flush(const struct texture_entry *entry)
{
	DCFlushRange(entry->texels, entry->bytes);
	texture_counts.flushed++;
	texture_counts.flushed_bytes += entry->bytes;
	texture_memory_changed = TRUE;
}

/* the header marked as seen, in its entry (flushed: a converted bitmap's
pixels are in memory) */
#define TEXTURE_MARK_FLUSHED 0x8000UL

static void texture_mark(const D3DBaseTexture *texture, const struct texture_entry *entry, BOOL flushed)
{
	((D3DBaseTexture *)texture)->Lock = TEXTURE_MARK | (flushed ? TEXTURE_MARK_FLUSHED : 0) |
		(unsigned long)(entry - texture_cache);
}

/* ---------- textures drawn by the device

A render target's pixels are copied out of the EFB into its own memory in
GX's format (by WiiLibs, at efb_take), the same size as the Xbox's: its texture
is then bound from there, as GX's, by the record of the copy. */

#define RENDER_TARGET_RECORDS 16

struct render_target_record
{
	DWORD data;
	unsigned long width, height;
	u8 gx_format;
	struct texture_entry entry;
};

static struct render_target_record render_target_records[RENDER_TARGET_RECORDS];
static unsigned render_target_record_next;

static struct render_target_record *render_target_record_find(DWORD data)
{
	unsigned index;

	for (index = 0; index < RENDER_TARGET_RECORDS; index++)
	{
		if (render_target_records[index].data && render_target_records[index].data == data)
			return &render_target_records[index];
	}
	return NULL;
}

/* a drawn texture's entry, if the texture is one the device drew */
static struct texture_entry *render_target_texture(const D3DBaseTexture *texture)
{
	struct render_target_record *record = render_target_record_find(texture->Data);
	struct xgpu_texture_description description;
	struct texture_entry *entry;

	if (!record)
		return NULL;
	xgpu_texture_describe(texture->Format, texture->Size, &description);
	if (description.width != record->width || description.height != record->height || description.cube_map)
		return NULL;
	entry = &record->entry;
	memset(entry, 0, sizeof(*entry));
	entry->data = texture->Data;
	entry->format = texture->Format;
	entry->size = texture->Size;
	entry->width = record->width;
	entry->height = record->height;
	entry->texels = PLATFORM_PHYSICAL_TO_VIRTUAL(texture->Data);
	entry->native = TRUE;
	entry->gx_format = record->gx_format;
	entry->levels = 1;
	entry->channels = _channels_all;
	entry->linear = description.linear;
	entry->used_frame = device.frames;
	entry->serial = ++texture_serial;
	return entry;
}

static struct texture_entry *texture_get(const D3DBaseTexture *texture, const D3DPalette *palette)
{
	struct xgpu_texture_description description;
	struct texture_entry *entry = NULL, *oldest = NULL;
	struct format_information information;
	struct gx_native_format native;
	unsigned long *argb;
	unsigned char *rgba;
	const unsigned char *source;
	unsigned char *swapped;
	unsigned long index, texel_count, mark;
	BOOL seen, flushed;

	if (!texture || !texture->Data)
	{
		texture_failures[_failure_no_data]++;
		return NULL;
	}
	entry = render_target_texture(texture);
	if (entry)
		return entry;
	/* (the header's mark: its pixels unchanged since the device saw them
	through it, in the entry it names, which still holds them; a mark whose
	entry has gone, or whose header now names other pixels, counts for
	nothing) */
	mark = texture->Lock;
	index = mark & (TEXTURE_MARK_FLUSHED - 1);
	seen = (mark & TEXTURE_MARK_MASK) == TEXTURE_MARK && index < TEXTURE_CACHE_ENTRIES &&
		texture_cache[index].texels && texture_cache[index].data == texture->Data &&
		texture_cache[index].format == texture->Format && texture_cache[index].size == texture->Size;
	flushed = seen && (mark & TEXTURE_MARK_FLUSHED);
	if (seen)
	{
		entry = &texture_cache[index];
		texture_counts.marked++;
	}
	else
		entry = texture_lookup(texture);
	/* (a decoded texture is hashed again when its header has lost the mark:
	the glyph cache, whose strings bring their glyphs in just before they
	are drawn, is locked for each; one outside the map's texture cache once a
	frame as well, for pixels written without a lock) */
	if (entry && !entry->native && (!seen || (entry->outside && entry->used_frame != device.frames)) &&
		texture_hash(texture, entry->source_bytes) != entry->hash)
	{
		/* (rewritten since it was decoded: again) */
		texture_evict(entry);
		oldest = entry;
		entry = NULL;
	}
	if (entry)
	{
		if (entry->native && !flushed)
		{
			texture_flush(entry);
			flushed = TRUE;
		}
		entry->used_frame = device.frames;
		texture_mark(texture, entry, flushed);
		return entry;
	}
	if (!oldest)
		oldest = texture_victim();
	texture_counts.created++;

	xgpu_texture_describe(texture->Format, texture->Size, &description);
	if (!description.cube_map && description.depth <= 1 &&
		((texture->Format & D3DFORMAT_DIMENSION_MASK) >> D3DFORMAT_DIMENSION_SHIFT) == 2 &&
		gx_native_format(description.format, &native) && texture_in_texture_cache(texture) &&
		(native.gx_format != GX_TF_CI8 || (palette && palette->Data)))
	{
		unsigned long levels = gx_native_levels(&description, &native);

		if (levels)
		{
			static unsigned reported;

			entry = texture_native(oldest, texture, &description, &native, levels);
			if (reported++ < 12)
				platform_log("Direct3D: converted bitmap %lux%lu, format %02lx, %lu of %lu levels at %08lx",
					description.width, description.height, (unsigned long)description.format, levels,
					description.levels, (unsigned long)texture->Data);
			texture_flush(entry);
			texture_mark(texture, entry, TRUE);
			return entry;
		}
	}
	if (description.cube_map && texture_in_texture_cache(texture))
	{
		/* a cube map as the converter left it: a sphere map, and its
		trailer at the cube's end (tools/wii_gx_textures.py's convert_cube) */
		unsigned long total = 6 * xgpu_texture_face_size(&description);
		const unsigned char *pixels = (const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(texture->Data);
		const unsigned char *trailer = pixels + total - 32;

		if (total > 32 && !memcmp(trailer, "GXSPHERE", 8))
		{
			unsigned size = (unsigned)(trailer[8] << 8 | trailer[9]);

			if (oldest->texels)
				texture_evict(oldest);
			entry = oldest;
			entry->data = texture->Data;
			entry->format = texture->Format;
			entry->size = texture->Size;
			entry->width = entry->height = size;
			entry->bytes = (unsigned long)size * size * 2;
			entry->texels = (void *)pixels;
			entry->native = TRUE;
			entry->gx_format = trailer[10];
			entry->levels = 1;
			entry->channels = _channels_all;
			entry->sphere = TRUE;
			entry->used_frame = device.frames;
			texture_link(entry);
			texture_flush(entry);
			texture_mark(texture, entry, TRUE);
			return entry;
		}
	}
	if (description.width > 1024 || description.height > 1024 || description.cube_map || description.depth > 1)
	{
		texture_failures[_failure_shape]++;
		return NULL;
	}
	information = xgpu_texture_format_information(description.format);
	texel_count = description.width * description.height;
	if (!texel_count || information.kind == 0)
	{
		texture_failures[_failure_format]++;
		return NULL;
	}
	argb = malloc(texel_count * sizeof(*argb));
	if (!argb && texture_cache_reclaim(texel_count * sizeof(*argb), oldest))
		argb = malloc(texel_count * sizeof(*argb));
	if (!argb)
	{
		static unsigned reported;

		if (reported++ < 8)
			platform_log("Direct3D: no memory to decode a texture %lux%lu of format %02lx at %08lx (%s the texture cache)",
				description.width, description.height, (unsigned long)description.format, (unsigned long)texture->Data,
				texture_in_texture_cache(texture) ? "in" : "not in");
		texture_failures[_failure_memory]++;
		return NULL;
	}
	source = (const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(texture->Data);
	/* a texture the game wrote itself (the text's glyphs, the fallback checkers) holds its 16- and
	32-bit texels in the Wii's byte order; the decoder reads them as the Xbox stored them, so it is
	given a copy in that order */
	swapped = NULL;
	if (!description.compressed && !texture_in_texture_cache(texture) &&
		(information.bytes == 2 || information.bytes == 4))
	{
		unsigned long bytes = xgpu_texture_level_bytes(&description, 0);

		swapped = malloc(bytes);
		if (swapped)
		{
			for (index = 0; index + information.bytes <= bytes; index += information.bytes)
			{
				if (information.bytes == 2)
				{
					swapped[index] = source[index + 1];
					swapped[index + 1] = source[index];
				}
				else
				{
					swapped[index] = source[index + 3];
					swapped[index + 1] = source[index + 2];
					swapped[index + 2] = source[index + 1];
					swapped[index + 3] = source[index];
				}
			}
			source = swapped;
		}
	}
	if (description.compressed)
		xgpu_texture_dxt_decode_level(information.kind, source, description.width, description.height, 1, argb);
	else if (!xgpu_texture_decode_level(&description, 0, source,
		palette && palette->Data ? (const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(palette->Data) : NULL, argb))
	{
		texture_failures[_failure_decode]++;
		free(swapped);
		free(argb);
		return NULL;
	}
	free(swapped);
	/* ARGB values to RGBA bytes, in place */
	rgba = (unsigned char *)argb;
	for (index = 0; index < texel_count; index++)
	{
		unsigned long value = argb[index];

		rgba[index * 4 + 0] = (unsigned char)(value >> 16);
		rgba[index * 4 + 1] = (unsigned char)(value >> 8);
		rgba[index * 4 + 2] = (unsigned char)value;
		rgba[index * 4 + 3] = (unsigned char)(value >> 24);
	}

	entry = oldest;
	{
		size_t bytes = wl_gxc_tex_bytes(WL_GXC_RGBA8, (uint32_t)description.width, (uint32_t)description.height);
		void *texels;

		/* room for it: the least recently drawn go */
		while (texture_cache_bytes + bytes > TEXTURE_CACHE_BYTES)
		{
			struct texture_entry *victim = NULL;

			for (index = 0; index < TEXTURE_CACHE_ENTRIES; index++)
			{
				if (texture_cache[index].texels && !texture_cache[index].native &&
					(!victim || texture_cache[index].used_frame < victim->used_frame))
					victim = &texture_cache[index];
			}
			if (!victim)
				break;
			texture_evict(victim);
		}
		if (entry->texels)
			texture_evict(entry);
		texels = aligned_alloc(32, (bytes + 31) & ~(size_t)31);
		if (!texels && texture_cache_reclaim(bytes, entry))
			texels = aligned_alloc(32, (bytes + 31) & ~(size_t)31);
		if (!texels)
		{
			static unsigned reported;

			if (reported++ < 8)
				platform_log("Direct3D: no room for a decoded texture %lux%lu of format %02lx at %08lx (%lu bytes kept)",
					description.width, description.height, (unsigned long)description.format,
					(unsigned long)texture->Data, texture_cache_bytes);
			texture_failures[_failure_memory]++;
			free(argb);
			return NULL;
		}
		wl_gxc_encode(WL_GXC_RGBA8, rgba, (uint32_t)description.width, (uint32_t)description.height, texels, bytes);
		DCFlushRange(texels, bytes);
		free(argb);
		entry->data = texture->Data;
		entry->format = texture->Format;
		entry->size = texture->Size;
		entry->width = description.width;
		entry->height = description.height;
		entry->bytes = bytes;
		entry->texels = texels;
		entry->native = FALSE;
		entry->gx_format = GX_TF_RGBA8;
		entry->levels = 1;
		entry->channels = _channels_all;
		entry->alpha_texels = NULL;
		entry->linear = description.linear;
		entry->source_bytes = xgpu_texture_level_bytes(&description, 0);
		entry->hash = texture_hash(texture, entry->source_bytes);
		entry->outside = !texture_in_texture_cache(texture);
		entry->used_frame = device.frames;
		texture_link(entry);
		texture_cache_bytes += bytes;
		texture_counts.decoded++;
		/* (new texels, perhaps where others were: GX's texture cache may hold
		those) */
		texture_memory_changed = TRUE;
		texture_mark(texture, entry, flushed);
	}
	return entry;
}

static u8 gx_wrap(DWORD address)
{
	switch (address)
	{
	case D3DTADDRESS_MIRROR: return GX_MIRROR;
	case D3DTADDRESS_CLAMP: case D3DTADDRESS_BORDER: case D3DTADDRESS_CLAMPTOEDGE: return GX_CLAMP;
	default: return GX_REPEAT;
	}
}

static BOOL power_of_two(unsigned long value)
{
	return value && !(value & (value - 1));
}

/* GX's minification filter for the stage's (with its mip filter) */
static u8 gx_minification(DWORD stage, BOOL mipmapped)
{
	BOOL linear = D3D__TextureState[stage][D3DTSS_MINFILTER] != D3DTEXF_POINT;
	DWORD mip = D3D__TextureState[stage][D3DTSS_MIPFILTER];

	if (!mipmapped || mip == D3DTEXF_NONE)
		return linear ? GX_LINEAR : GX_NEAR;
	if (mip == D3DTEXF_POINT)
		return linear ? GX_LIN_MIP_NEAR : GX_NEAR_MIP_NEAR;
	return linear ? GX_LIN_MIP_LIN : GX_NEAR_MIP_LIN;
}

/* what each stage's texture is, for the pixel stage, and its texture object
(a bumped reflection's bump map is bound again with another TLUT:
bump_offset_bind) */
static struct texture_entry *bound_entries[D3DTSS_MAXSTAGES];
static GXTexObj bound_objects[D3DTSS_MAXSTAGES];
/* a linear texture is addressed in texels, GX's in fractions of it */
static float texcoord_scale[D3DTSS_MAXSTAGES][2] = { { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 } };

/* the stages' textures bound to GX's texture maps (a DXT3 or DXT5 texture's
alpha to the map four after); which stages have one */
/* the stages bound to a cube map the converter made a sphere map: their
texture coordinates are a direction, which sphere_coordinates maps */
static unsigned sphere_stages;

/* each stage's texture objects as last built, and what they were built from
(an entry's serial and the stage's states): a stage drawing the same texture
the same way loads them again without building them */
static struct
{
	unsigned long serial;
	u8 wrap_s, wrap_t, minification, magnification;
	GXTexObj alpha;
} stage_objects[D3DTSS_MAXSTAGES];

static unsigned textures_bind(void)
{
	unsigned stages = 0;
	DWORD stage;

	sphere_stages = 0;
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		struct texture_entry *entry;
		GXTexObj object;
		BOOL mipmapped, repeatable;
		u8 wrap_s, wrap_t, minification, magnification;

		bound_entries[stage] = NULL;
		texcoord_scale[stage][0] = texcoord_scale[stage][1] = 1.0f;
		if (!((D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f))
		{
			if (device.textures[stage])
				device.statistics.stages_without_mode++;
			continue;
		}
		entry = texture_get(device.textures[stage], device.palettes[stage]);
		if (!entry)
		{
			device.statistics.textures_failed++;
			continue;
		}
		device.statistics.textures_bound++;
		repeatable = power_of_two(entry->width) && power_of_two(entry->height);
		mipmapped = entry->levels > 1 && repeatable;
		wrap_s = repeatable && !entry->sphere ? gx_wrap(D3D__TextureState[stage][D3DTSS_ADDRESSU]) : GX_CLAMP;
		wrap_t = repeatable && !entry->sphere ? gx_wrap(D3D__TextureState[stage][D3DTSS_ADDRESSV]) : GX_CLAMP;
		if (entry->sphere)
			sphere_stages |= 1U << stage;
		minification = gx_minification(stage, mipmapped);
		magnification = D3D__TextureState[stage][D3DTSS_MAGFILTER] == D3DTEXF_POINT ? GX_NEAR : GX_LINEAR;
		if (entry->gx_format == GX_TF_CI8)
		{
			const struct palette_tlut *tlut = palette_tlut(device.palettes[stage]);

			if (!tlut)
			{
				device.statistics.textures_failed++;
				continue;
			}
			if (loaded_tluts[stage] != tlut || loaded_tlut_hashes[stage] != tlut->hash)
			{
				GXTlutObj table;

				GX_InitTlutObj(&table, (void *)tlut->entries, tlut->format, 256);
				GX_LoadTlut(&table, GX_TLUT0 + stage);
				loaded_tluts[stage] = tlut;
				loaded_tlut_hashes[stage] = tlut->hash;
			}
		}
		if (stage_objects[stage].serial != entry->serial || stage_objects[stage].wrap_s != wrap_s ||
			stage_objects[stage].wrap_t != wrap_t || stage_objects[stage].minification != minification ||
			stage_objects[stage].magnification != magnification)
		{
			if (entry->gx_format == GX_TF_CI8)
				GX_InitTexObjCI(&object, entry->texels, (u16)entry->width, (u16)entry->height, GX_TF_CI8, wrap_s,
					wrap_t, mipmapped ? GX_TRUE : GX_FALSE, GX_TLUT0 + stage);
			else
				GX_InitTexObj(&object, entry->texels, (u16)entry->width, (u16)entry->height, entry->gx_format, wrap_s,
					wrap_t, mipmapped ? GX_TRUE : GX_FALSE);
			GX_InitTexObjLOD(&object, minification, magnification, 0.0f,
				mipmapped ? (f32)(entry->levels - 1) : 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
			bound_objects[stage] = object;
			if (entry->alpha_texels)
			{
				GX_InitTexObj(&stage_objects[stage].alpha, entry->alpha_texels, (u16)entry->width, (u16)entry->height,
					GX_TF_I4, wrap_s, wrap_t, mipmapped ? GX_TRUE : GX_FALSE);
				GX_InitTexObjLOD(&stage_objects[stage].alpha, minification, magnification, 0.0f,
					mipmapped ? (f32)(entry->levels - 1) : 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
			}
			stage_objects[stage].serial = entry->serial;
			stage_objects[stage].wrap_s = wrap_s;
			stage_objects[stage].wrap_t = wrap_t;
			stage_objects[stage].minification = minification;
			stage_objects[stage].magnification = magnification;
			texture_counts.objects++;
		}
		GX_LoadTexObj(&bound_objects[stage], (u8)(GX_TEXMAP0 + stage));
		if (entry->alpha_texels)
			GX_LoadTexObj(&stage_objects[stage].alpha, (u8)(GX_TEXMAP4 + stage));
		bound_entries[stage] = entry;
		if (entry->linear)
		{
			texcoord_scale[stage][0] = 1.0f / (float)entry->width;
			texcoord_scale[stage][1] = 1.0f / (float)entry->height;
		}
		stages |= 1U << stage;
	}
	if (texture_memory_changed)
	{
		GX_InvalidateTexAll();
		texture_memory_changed = FALSE;
		texture_counts.invalidations++;
	}
	return stages;
}

/* ---------- the pixel stage

The pixel shader's register combiners are translated to TEV stages
(gx_psh.c), kept by the combiners, the textures' kinds and their constants'
signatures (gx_psh_cache.c). A shader GX cannot hold takes the first stage's
texture, modulated by the diffuse colour. */

/* the last draw's program (NULL before the first), and whether it was
usable */
static const struct gx_psh_program *pixel_shader_program;
static BOOL pixel_shader_usable;
static unsigned long pixel_shaders_logged;

/* ---------- bumped lookups and a pass's own vertex terms

A bumped reflection (texture modes DOTPRODUCT, DOT_RFLCT_DIFF and
DOT_RFLCT_SPEC after the bump map) looks its cube map up at the reflection
of the bump map's normal, carried into the world by the tangent frame of
texture coordinates 1 to 3 (their xyz the frame's rows, their w the eye).
GX has no such lookup; its indirect unit offsets a lookup by a texture, as
its environment-mapped bump does (GX_SetTevIndBumpST). So the cube map's
sphere map is read at the sphere coordinates of the flat normal's
reflection, moved by the bump map's x and y (gx_psh.c places the stages):
the bump map's texels again, through a TLUT of its normals' x (as alpha,
the indirect unit's s) and y (as intensity, its t), and per vertex, how far
the sphere coordinates move as the normal tilts along the frame's x and y
(texture coordinates 1 and 2: a difference over a tilt of
BUMP_OFFSET_DELTA). The two stages before a lookup take each term (GX's
dynamic S and T matrices, by matrix 0's scale), the lookup adds its own
coordinate. This is a linear approximation of the reflection's move, per
vertex; the Fresnel term reads the flat normal (colour 1) where NV2A reads
the bumped one (DOT_RFLCT_DIFF's lookup).

A specular light from a lightmap takes e.l of the vertices (gx_psh.c's
recipe): colour 0. */

/* the program's vertex terms (_gx_psh_vertices_*) and the TEV stages set
for the indirect unit */
static unsigned char vertex_terms = _gx_psh_vertices_program;
static unsigned indirect_stages;

enum
{
	/* the TLUT of the bump map's normals' x and y */
	BUMP_OFFSET_TLUT = GX_TLUT4,
	/* matrix 0's scale: a dynamic matrix's offset is the coordinate times
	the indirect texel less 128, over 256, times 2 to this; with the texel
	127.5 x + 128, a term of the coordinate times x */
	BUMP_OFFSET_SCALE = 1,
};
/* the tilt the terms are measured over, and their largest (in the sphere
map's coordinates, near its rim's singularity) */
#define BUMP_OFFSET_DELTA 0.25f
#define BUMP_OFFSET_LIMIT 2.0f

static struct
{
	DWORD data;
	unsigned long hash, hashed_frame;
	BOOL loaded;
	u16 entries[256] __attribute__((aligned(32)));
} bump_offset;

/* the bump map (stage 0's) as the indirect unit's map, through a TLUT of
its palette's x (alpha) and y (intensity) */
static void bump_offset_bind(void)
{
	const D3DPalette *palette = device.palettes[0];
	GXTexObj object;

	if (!bound_entries[0] || bound_entries[0]->gx_format != GX_TF_CI8 || !palette || !palette->Data)
		return;
	if (!bump_offset.loaded || bump_offset.data != palette->Data || bump_offset.hashed_frame != device.frames)
	{
		const D3DCOLOR *colors = (const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(palette->Data);
		unsigned long hash = palette_hash(colors), index;

		if (!bump_offset.loaded || bump_offset.data != palette->Data || bump_offset.hash != hash)
		{
			GXTlutObj table;

			for (index = 0; index < 256; index++)
				bump_offset.entries[index] = (u16)((((colors[index] >> 16) & 0xFF) << 8) | ((colors[index] >> 8) & 0xFF));
			DCFlushRange(bump_offset.entries, sizeof(bump_offset.entries));
			GX_InitTlutObj(&table, bump_offset.entries, GX_TL_IA8, 256);
			GX_LoadTlut(&table, BUMP_OFFSET_TLUT);
			bump_offset.data = palette->Data;
			bump_offset.hash = hash;
			bump_offset.loaded = TRUE;
		}
		bump_offset.hashed_frame = device.frames;
	}
	object = bound_objects[0];
	GX_InitTexObjTlut(&object, BUMP_OFFSET_TLUT);
	GX_LoadTexObj(&object, (u8)(GX_TEXMAP0 + GX_PSH_BUMP_TEXMAP));
}

/* the indirect unit as the program's stages ask (NULL: off, for stages the
device sets itself) */
/* display.bumped_highlights: the bumped reflections' indirect lookups and the
specular lights' programs made by hand (gx_psh.c), read once */
static BOOL bumped_highlights(void)
{
	static int enabled = -1;

	if (enabled < 0)
	{
		enabled = config_boolean("display.bumped_highlights") ? 1 : 0;
		gx_psh_specular_recipes = enabled ? TRUE : FALSE;
	}
	return enabled ? TRUE : FALSE;
}

static void indirect_apply(const struct gx_psh_program *program)
{
	static f32 matrix[2][3] = { { 0.5f, 0.0f, 0.0f }, { 0.0f, 0.5f, 0.0f } };
	unsigned used = 0, stage;

	if (program)
	{
		for (stage = 0; stage < program->tev.stage_count; stage++)
			if (program->indirect[stage] != _gx_psh_indirect_none)
				used |= 1U << stage;
	}
	for (stage = 0; stage < GX_PSH_MAXIMUM_STAGES; stage++)
		if (indirect_stages & ~used & (1U << stage))
			GX_SetTevDirect((u8)stage);
	if (!used)
	{
		if (indirect_stages)
			GX_SetNumIndStages(0);
		indirect_stages = 0;
		return;
	}
	bump_offset_bind();
	GX_SetNumIndStages(1);
	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, (u8)(GX_TEXMAP0 + GX_PSH_BUMP_TEXMAP));
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);
	GX_SetIndTexMatrix(GX_ITM_0, matrix, BUMP_OFFSET_SCALE);
	for (stage = 0; stage < program->tev.stage_count; stage++)
	{
		const WlTevStage *tev = &program->tev.stages[stage];

		switch (program->indirect[stage])
		{
		case _gx_psh_indirect_s:
			/* (no lookup, but the lookup's map scales the coordinate) */
			GX_SetTevOrder((u8)stage, GX_TEXCOORD0 + GX_PSH_BUMP_TEXCOORD_S,
				(u32)program->tev.stages[stage + 2].texmap | GX_TEXMAP_DISABLE, tev->channel);
			GX_SetTevIndirect((u8)stage, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_ST, GX_ITM_S0, GX_ITW_0, GX_ITW_0, GX_FALSE,
				GX_FALSE, GX_ITBA_OFF);
			break;
		case _gx_psh_indirect_t:
			GX_SetTevOrder((u8)stage, GX_TEXCOORD0 + GX_PSH_BUMP_TEXCOORD_T,
				(u32)program->tev.stages[stage + 1].texmap | GX_TEXMAP_DISABLE, tev->channel);
			GX_SetTevIndirect((u8)stage, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_ST, GX_ITM_T0, GX_ITW_0, GX_ITW_0, GX_TRUE,
				GX_FALSE, GX_ITBA_OFF);
			break;
		case _gx_psh_indirect_lookup:
			GX_SetTevIndirect((u8)stage, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_OFF, GX_ITW_OFF,
				GX_TRUE, GX_FALSE, GX_ITBA_OFF);
			break;
		case _gx_psh_indirect_repeat:
			GX_SetTevIndRepeat((u8)stage);
			break;
		}
	}
	indirect_stages = used;
}

static void tev_fallback(unsigned stages)
{
	struct texture_entry *entry = bound_entries[0];

	indirect_apply(NULL);
	vertex_terms = _gx_psh_vertices_program;

	if (!(stages & 1) || !entry)
	{
		GX_SetNumTevStages(1);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
		return;
	}
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
}

/* what the stages set up last read of the vertices: the texture
coordinates (bit n: GX_TEXCOORDn) and the colour channels (bit 0 COLOR0A0,
bit 1 COLOR1A1). draw_hardware leaves an output nothing reads alone */
static unsigned tev_texcoords = 0x1F, tev_channels = 3;
/* the textures the stages set up last were translated for (a lit draw's
planar fog translates them again with D0's alpha a ramp's: lit_fog_ramp) */
static struct gx_psh_textures tev_textures;

static void tev_setup(unsigned stages)
{
	static int debug = -1;
	struct gx_psh_textures textures;

	/* (all of the vertices' values, until a translation says less) */
	tev_texcoords = 0x1F;
	tev_channels = 3;
	const struct gx_psh_program *program;
	BOOL usable, translated;
	DWORD stage;
	BOOL bumped_reflection, flat_reflection;
	DWORD modes = D3D__RenderState[D3DRS_PSTEXTUREMODES];

	/* the desktop ports' debug views (port_config.c): the diffuse colour
	alone, or the first texture alone, opaque */
	if (debug < 0)
		debug = config_boolean("debug.gpu_debug_flat") ? 1 : config_boolean("debug.gpu_debug_texture0") ? 2 : 0;
	if (debug)
	{
		indirect_apply(NULL);
		vertex_terms = _gx_psh_vertices_program;
		GX_SetNumTevStages(1);
		GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
		if (debug == 2 && (stages & 1))
		{
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
			GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
		}
		else
		{
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
			GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, debug == 2 ? GX_CC_ZERO : GX_CC_RASC);
		}
		GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		return;
	}

	memset(&textures, 0, sizeof(textures));
	/* a bumped reflection (after the bump map, DOTPRODUCT, DOT_RFLCT_DIFF
	and DOT_RFLCT_SPEC): its cube map's sphere map read through the indirect
	unit, the normal's lookup colour 1 (indirect_apply) */
	bumped_reflection = bumped_highlights() && (D3D__RenderState[D3DRS_PSTEXTUREMODES] & 0xFFFE0UL) == (0x11UL << 5 | 0x0BUL << 10 | 0x0CUL << 15) &&
		(stages & 1) && bound_entries[0] && bound_entries[0]->gx_format == GX_TF_CI8 && (stages & 8) &&
		bound_entries[3] && bound_entries[3]->sphere;
	/* a reflection whose bump GX cannot follow (the water's: DOTPRODUCT in
	stages 1 and 2, its bump map drawn each frame, not a palette of
	normals; or with display.bumped_highlights off): the cube map's sphere
	map at the flat normal's reflection, which the device computes per
	vertex as a bumped reflection's (bumped_reflection_vertex) */
	flat_reflection = !bumped_reflection && ((modes >> 5) & 0x1f) == 0x11 &&
		(((modes >> 10) & 0x1f) == 0x11 || ((modes >> 10) & 0x1f) == 0x0B) && ((modes >> 15) & 0x1f) == 0x0C &&
		(stages & 8) && bound_entries[3] && bound_entries[3]->sphere;
	for (stage = 0; stage < GX_PSH_TEXTURES; stage++)
	{
		DWORD mode = (modes >> (5 * stage)) & 0x1f;

		if (flat_reflection && stage == 2 && mode == 0x0B)
		{
			textures.vertex_color |= 1U << stage;
			continue;
		}
		if (flat_reflection && stage == 3)
		{
			textures.present |= 1U << stage;
			textures.cube |= 1U << stage;
			textures.reflected |= 1U << stage;
			textures.contents[stage] = bound_entries[stage]->channels;
			continue;
		}

		if (bumped_reflection && stage == 2)
		{
			textures.vertex_color |= 1U << stage;
			continue;
		}
		if (bumped_reflection && stage == 3)
		{
			textures.present |= 1U << stage;
			textures.cube |= 1U << stage;
			textures.bumped |= 1U << stage;
			textures.contents[stage] = bound_entries[stage]->channels;
			continue;
		}

		/* (2D, projected and sphere-mapped cube map lookups: the rest, the dot
		product and bump map modes, read as nothing) */
		if (!(stages & (1U << stage)) || !bound_entries[stage] ||
			(mode != 1 && mode != 2 && !(mode == 3 && bound_entries[stage]->sphere)))
		{
			if (mode)
				textures.unavailable |= 1U << stage;
			continue;
		}
		textures.present |= 1U << stage;
		if (mode == 3)
			textures.cube |= 1U << stage;
		if (bound_entries[stage]->alpha_texels)
			textures.separate_alpha |= 1U << stage;
		textures.contents[stage] = bound_entries[stage]->channels;
	}
	/* (the fog ramp is map 7, which a fourth stage's DXT3/5 alpha would
	take: no fog then) */
	textures.fog_color = D3D__RenderState[D3DRS_FOGCOLOR] & 0x00FFFFFFUL;
	if (D3D__RenderState[D3DRS_FOGENABLE] && !(textures.separate_alpha & 8))
	{
		textures.fog = 1;
		wl_gx_ramp_bind(GX_TEXMAP7);
	}
	/* (padding and all, as the cache compares them) */
	memcpy(&tev_textures, &textures, sizeof(tev_textures));
	program = gx_psh_cache_program(D3D__RenderState, &textures, &usable, &translated);
	pixel_shader_program = program;
	pixel_shader_usable = usable;
	if (translated && !usable && pixel_shaders_logged++ < 8)
	{
		const DWORD *state = D3D__RenderState;
		unsigned long count = state[D3DRS_PSCOMBINERCOUNT] & 0xff, combiner;

		platform_log("Direct3D: pixel shader beyond GX (%s): %lu combiners (%08lx), final %08lx %08lx, modes %08lx",
			program->reason ? program->reason : "no stages", count,
			(unsigned long)state[D3DRS_PSCOMBINERCOUNT], (unsigned long)state[D3DRS_PSFINALCOMBINERINPUTSABCD],
			(unsigned long)state[D3DRS_PSFINALCOMBINERINPUTSEFG], (unsigned long)state[D3DRS_PSTEXTUREMODES]);
		for (combiner = 0; combiner < count && combiner < 8; combiner++)
			platform_log("  %lu: rgb %08lx -> %08lx, alpha %08lx -> %08lx", combiner,
				(unsigned long)state[D3DRS_PSRGBINPUTS0 + combiner], (unsigned long)state[D3DRS_PSRGBOUTPUTS0 + combiner],
				(unsigned long)state[D3DRS_PSALPHAINPUTS0 + combiner], (unsigned long)state[D3DRS_PSALPHAOUTPUTS0 + combiner]);
	}
	tev_texcoords = 0x1F;
	tev_channels = 3;
	if (usable)
	{
		gx_psh_apply(program);
		indirect_apply(program);
		vertex_terms = program->vertices;
		tev_texcoords = program->texcoords;
		tev_channels = program->channels;
	}
	else
	{
		/* (the fallback's texture 0 and colour 0 alone: the draws in hardware
		compute nothing else, a cube map's sphere map coordinates among it) */
		tev_fallback(stages);
		tev_texcoords = (stages & 1) && bound_entries[0] ? 1 : 0;
		tev_channels = 1;
	}
}

/* ---------- vertices

Each draw's vertices are the program's: fetched from the streams (or the
immediate registers), run, then sent to GX as they come out. */

/* (by the reciprocal, which the callers' constant maxima fold) */
static inline float normalized(long value, float maximum)
{
	float result = (float)value * (1.0f / maximum);

	return result < -1.0f ? -1.0f : result;
}

/* how an element's data becomes its register (each sets all four
components: what the data doesn't give is 0, and w 1). A draw's elements
are planned once (stream_fetch_plan), each with its own small case, rather
than a switch on the Direct3D type for each element of each vertex. */
enum fetch_kind
{
	FETCH_FLOAT1, FETCH_FLOAT2, FETCH_FLOAT3, FETCH_FLOAT4, FETCH_FLOAT2H, FETCH_COLOR,
	FETCH_SHORTS, FETCH_NORMSHORT1, FETCH_NORMSHORT2, FETCH_NORMSHORTS, FETCH_NORMPACKED3, FETCH_BYTES,
};

/* the conversions of a byte and of a packed normal's fields, made once by
the same expressions (/ 255.0f, normalized): a load in place of an
integer's conversion */
static float byte_units[256];
static float packed_elevens[2048], packed_tens[1024];
static BOOL fetch_tables_made;

static void fetch_tables_make(void)
{
	long value;

	for (value = 0; value < 256; value++)
		byte_units[value] = value / 255.0f;
	for (value = 0; value < 2048; value++)
		packed_elevens[value] = normalized(value < 1024 ? value : value - 2048, 1023.0f);
	for (value = 0; value < 1024; value++)
		packed_tens[value] = normalized(value < 512 ? value : value - 1024, 511.0f);
	fetch_tables_made = TRUE;
}

static enum fetch_kind fetch_kind_of(unsigned type, BOOL *known)
{
	*known = TRUE;
	switch (type)
	{
	case D3DVSDT_FLOAT1: return FETCH_FLOAT1;
	case D3DVSDT_FLOAT2: return FETCH_FLOAT2;
	case D3DVSDT_FLOAT3: return FETCH_FLOAT3;
	case D3DVSDT_FLOAT4: return FETCH_FLOAT4;
	case D3DVSDT_FLOAT2H: return FETCH_FLOAT2H;
	case D3DVSDT_D3DCOLOR: return FETCH_COLOR;
	case D3DVSDT_SHORT1: case D3DVSDT_SHORT2: case D3DVSDT_SHORT3: case D3DVSDT_SHORT4: return FETCH_SHORTS;
	case D3DVSDT_NORMSHORT1: return FETCH_NORMSHORT1;
	case D3DVSDT_NORMSHORT2: return FETCH_NORMSHORT2;
	case D3DVSDT_NORMSHORT3: case D3DVSDT_NORMSHORT4: return FETCH_NORMSHORTS;
	case D3DVSDT_NORMPACKED3: return FETCH_NORMPACKED3;
	case D3DVSDT_PBYTE1: case D3DVSDT_PBYTE2: case D3DVSDT_PBYTE3: case D3DVSDT_PBYTE4: return FETCH_BYTES;
	default:
		/* (any other: the register as 0, 0, 0, 1) */
		*known = FALSE;
		return FETCH_BYTES;
	}
}

/* a float's bits, as the data has them */
static inline void word_copy(float *out, const unsigned char *source)
{
	u32 word;

	memcpy(&word, source, sizeof(word));
	memcpy(out, &word, sizeof(word));
}

static inline short short_at(const unsigned char *source)
{
	short value;

	memcpy(&value, source, sizeof(value));
	return value;
}

static u8 color_byte(float value)
{
	return (u8)(value <= 0.0f ? 0 : value >= 1.0f ? 255 : (int)(value * 255.0f + 0.5f));
}

/* how a draw's vertices reach GX: in screen space (orthographic), or, for a
program that divides by its clip-space w, as (x w, y w, -w) through a
perspective projection, so GX clips them at the eye and interpolates their
textures for the perspective. Its depth is then a + b / w, fitted from the
draw's vertices (vertices_project) as the game's projections give it. */
struct draw_projection
{
	BOOL perspective;
	float a, b;
	/* (for the draw log: whether this draw's vertices gave the fit) */
	BOOL fitted;
};

static struct draw_projection projection_loaded;
/* the z bias the loaded projection has (projection_bias) */
static float projection_loaded_bias;
/* the last fit, for a draw whose vertices are all at one w (any draw's
whose depth was fitted, on either path: projection_load) */
static float projection_a, projection_b = 0.0f;

/* The z bias (D3DRS_ZBIAS, which d3d8_state.c makes a polygon offset of
-bias depth units and -bias/4 times the depth's slope, as the Xbox's D3D8
does). GX has no polygon offset; the draw's depth is pulled toward the eye
instead, by a part of its distance: bias / 16384 of it (a decal's bias of 8,
half a millimetre a metre), through b of the projection's a + b / w. A
decal then stays in front of the surface it lies on whatever angle it is
seen at, as the slope's term keeps it on the Xbox, though the surface's
depth and its own are computed differently (GX's transform of the
surface's vertices, the processor's of the decal's). */
static float projection_bias(void)
{
	float offset;

	if (!D3D__RenderState[D3DRS_SOLIDOFFSETENABLE])
		return 0.0f;
	memcpy(&offset, &D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], sizeof(offset));
	return offset < 0.0f ? -offset : 0.0f;
}

static void projection_load(const struct draw_projection *projection)
{
	Mtx44 matrix;
	float bias = projection_bias();

	/* (the last fit: any draw's of its own depth) */
	if (projection->perspective && projection->fitted)
	{
		projection_a = projection->a;
		projection_b = projection->b;
	}
	if (projection_valid && projection_loaded.perspective == projection->perspective && projection_loaded_bias == bias &&
		(!projection->perspective || (projection_loaded.a == projection->a && projection_loaded.b == projection->b)))
		return;
	memset(matrix, 0, sizeof(matrix));
	if (projection->perspective)
	{
		/* x' = 2/W X + Z, y' = -2/H Y - Z, z' = (1 - a) Z + b, w' = -Z */
		matrix[0][0] = 2.0f / SCREEN_WIDTH;
		matrix[0][2] = 1.0f;
		matrix[1][1] = -2.0f / SCREEN_HEIGHT;
		matrix[1][2] = -1.0f;
		matrix[2][2] = 1.0f - projection->a;
		/* (b / w nearer by bias / 16384 of itself: projection_bias) */
		matrix[2][3] = projection->b - fabsf(projection->b) * bias * (1.0f / 16384.0f);
		matrix[3][2] = -1.0f;
		GX_LoadProjectionMtx(matrix, GX_PERSPECTIVE);
	}
	else
	{
		matrix[0][0] = 2.0f / SCREEN_WIDTH;
		matrix[0][3] = -1.0f;
		matrix[1][1] = -2.0f / SCREEN_HEIGHT;
		matrix[1][3] = 1.0f;
		matrix[2][2] = 1.0f;
		/* (in screen space, bias depth units of the 24 bits nearer) */
		matrix[2][3] = -bias * (1.0f / 16777216.0f);
		matrix[3][3] = 1.0f;
		GX_LoadProjectionMtx(matrix, GX_ORTHOGRAPHIC);
	}
	projection_loaded = *projection;
	projection_loaded_bias = bias;
	projection_valid = TRUE;
}

/* ---------- fog

The pixel shaders read the fog register: its colour is D3DRS_FOGCOLOR, its
alpha the vertex's fog factor, from the program's oFog through the fog
table (D3DRS_FOGTABLEMODE; Halo's own programs compute the factor, table
none). GX has no such value, so it is sent as a fifth texture coordinate,
which reads the fog ramp (texture map 7): an intensity texture whose value
is its coordinate (WiiLibs' ramp, wl_gx_ramp_bind). The translation
(gx_psh.c) reads its alpha. */

static float as_float(DWORD value)
{
	float result;

	memcpy(&result, &value, sizeof(result));
	return result;
}

/* a fog factor (1 no fog, 0 all fog) as its coordinate on the ramp */
static inline float fog_ramp_coordinate(float factor)
{
	return wl_gx_ramp_coordinate(factor);
}

/* the vertex's fog (oFog) through the table (D3DRS_FOGTABLEMODE) as its
coordinate on the ramp */
static float fog_coordinate(float fog)
{
	float factor;

	switch (D3D__RenderState[D3DRS_FOGTABLEMODE])
	{
	case D3DFOG_LINEAR:
	{
		float start = as_float(D3D__RenderState[D3DRS_FOGSTART]), end = as_float(D3D__RenderState[D3DRS_FOGEND]);

		factor = end != start ? (end - fog) / (end - start) : 1.0f;
		break;
	}
	case D3DFOG_EXP:
		factor = expf(-as_float(D3D__RenderState[D3DRS_FOGDENSITY]) * fog);
		break;
	case D3DFOG_EXP2:
	{
		float density = as_float(D3D__RenderState[D3DRS_FOGDENSITY]) * fog;

		factor = expf(-density * density);
		break;
	}
	default:
		factor = fog;
		break;
	}
	return fog_ramp_coordinate(factor);
}

/* a cube map's direction (s, t, r) on its sphere map: the mirror ball's
mapping, whose inverse made the map (tools/wii_gx_textures.py's
sphere_directions); only straight behind (0, 0, -1) has no place, and
reads the disc's edge */
static void sphere_coordinates(const float direction[4], float out[2])
{
	float x = direction[0], y = direction[1], z = direction[2];
	float length = nv2a_sqrtf(x * x + y * y + z * z), m;

	if (!(length > 1.0e-12f))
	{
		out[0] = out[1] = 0.5f;
		return;
	}
	x /= length;
	y /= length;
	z /= length;
	m = 2.0f * nv2a_sqrtf(x * x + y * y + (z + 1.0f) * (z + 1.0f));
	if (!(m > 1.0e-6f))
	{
		out[0] = 0.5f;
		out[1] = 1.0f;
		return;
	}
	out[0] = x / m + 0.5f;
	out[1] = y / m + 0.5f;
}

/* a vertex as GX takes it (DEVICE_VERTEX_FORMAT's attributes, in order) */
struct gx_vertex
{
	/* (its position matrix's index, sent when emit_matrix_index: draw_keys) */
	u8 matrix;
	/* (a lit draw in hardware's: D0's alpha, the planar fog's) */
	u8 alpha;
	float position[3];
	u8 color0[4], color1[4];
	float texcoord[4][2];
	float fog;
	/* (a lit draw's: the processor's skinned normal, in the world) */
	float normal[3];
};

/* what a processor's draw computes of each vertex: what vertex_emit sends
(emit_attributes), with what is the same for the whole draw found once
(store_plan_set, once emit_attributes is) */
static struct
{
	/* the program's outputs (NV2A_CPU_LIVE_...) */
	unsigned live;
	/* the texture stages sent; those of them sphere maps, and those whose
	coordinates are scaled (a linear texture's, in texels) */
	unsigned stages, sphere, scaled;
	/* whether the fog goes through a table (else it is oFog itself) */
	BOOL fog_table;
} store_plan;

static void store_plan_set(void)
{
	unsigned stage;

	store_plan.live = NV2A_CPU_LIVE_POSITION;
	if (emit_attributes & 1)
		store_plan.live |= NV2A_CPU_LIVE_D0;
	if (emit_attributes & 2)
		store_plan.live |= NV2A_CPU_LIVE_D1;
	if (emit_attributes & 0x40)
		store_plan.live |= NV2A_CPU_LIVE_FOG;
	store_plan.stages = (emit_attributes >> 2) & 15;
	/* (a bumped reflection's terms read the frame's rows, coordinates 1 to 3:
	bumped_reflection_vertex) */
	if (vertex_terms == _gx_psh_vertices_bumped_reflection)
		store_plan.live |= NV2A_CPU_LIVE_T0 << 1 | NV2A_CPU_LIVE_T0 << 2 | NV2A_CPU_LIVE_T0 << 3;
	store_plan.sphere = sphere_stages & store_plan.stages;
	store_plan.scaled = 0;
	for (stage = 0; stage < 4; stage++)
	{
		if (!(store_plan.stages & (1U << stage)))
			continue;
		store_plan.live |= NV2A_CPU_LIVE_T0 << stage;
		/* (a scale of 1 leaves a coordinate as it is) */
		if (!(store_plan.sphere & (1U << stage)) &&
			(texcoord_scale[stage][0] != 1.0f || texcoord_scale[stage][1] != 1.0f))
			store_plan.scaled |= 1U << stage;
	}
	switch (D3D__RenderState[D3DRS_FOGTABLEMODE])
	{
	case D3DFOG_LINEAR: case D3DFOG_EXP: case D3DFOG_EXP2:
		store_plan.fog_table = TRUE;
		break;
	default:
		store_plan.fog_table = FALSE;
		break;
	}
}

static inline void color_bytes(const float value[4], u8 out[4])
{
	out[0] = color_byte(value[0]);
	out[1] = color_byte(value[1]);
	out[2] = color_byte(value[2]);
	out[3] = color_byte(value[3]);
}

/* a clip-space position as GX's perspective projection takes it, (x w, y w,
-w) in pixels (position_matrix's rows make the same of a vertex's own) */
static inline void clip_position(const float clip[4], float out[3])
{
	const float *scale = device.vertex_constants[VERTEX_CONSTANT_BIAS - 38];
	const float *offset = device.vertex_constants[VERTEX_CONSTANT_BIAS - 37];
	float w = clip[3];

	out[0] = clip[0] * scale[0] + (offset[0] + 0.5f) * w;
	out[1] = clip[1] * scale[1] + (offset[1] + 0.5f) * w;
	out[2] = -w;
}

/* a reflection, as DOT_RFLCT_SPEC makes it: 2n(n.e)/(n.n) - e */
static void reflection_of(const float n[3], const float e[3], float out[4])
{
	float nn = n[0] * n[0] + n[1] * n[1] + n[2] * n[2], ne = n[0] * e[0] + n[1] * e[1] + n[2] * e[2];
	float k = nn > 1.0e-20f ? 2.0f * ne / nn : 0.0f;
	int index;

	for (index = 0; index < 3; index++)
		out[index] = k * n[index] - e[index];
	out[3] = 1.0f;
}

/* a bumped reflection's vertex: texture coordinate 3 the sphere coordinates
of the flat normal's reflection, 1 and 2 how far they move as the normal
tilts along the frame's x and y, colour 1 the flat normal */
static void bumped_reflection_vertex(const struct nv2a_cpu_output *out, struct gx_vertex *vertex)
{
	const float *row0 = out->t[1], *row1 = out->t[2], *row2 = out->t[3];
	float normal[3], eye[3], tilted[3], reflection[4], base[2], moved[2], length;
	int axis, index;

	for (index = 0; index < 3; index++)
	{
		const float *row = index == 0 ? row0 : index == 1 ? row1 : row2;

		normal[index] = row[2];
		eye[index] = row[3];
	}
	reflection_of(normal, eye, reflection);
	sphere_coordinates(reflection, base);
	vertex->texcoord[3][0] = base[0];
	vertex->texcoord[3][1] = base[1];
	for (axis = 0; axis < 2; axis++)
	{
		for (index = 0; index < 3; index++)
		{
			const float *row = index == 0 ? row0 : index == 1 ? row1 : row2;

			tilted[index] = normal[index] + BUMP_OFFSET_DELTA * row[axis];
		}
		reflection_of(tilted, eye, reflection);
		sphere_coordinates(reflection, moved);
		for (index = 0; index < 2; index++)
		{
			float term = (moved[index] - base[index]) * (1.0f / BUMP_OFFSET_DELTA);

			if (!(term > -BUMP_OFFSET_LIMIT))
				term = -BUMP_OFFSET_LIMIT;
			if (term > BUMP_OFFSET_LIMIT)
				term = BUMP_OFFSET_LIMIT;
			vertex->texcoord[GX_PSH_BUMP_TEXCOORD_S + axis][index] = term;
		}
	}
	length = nv2a_sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
	for (index = 0; index < 3; index++)
		vertex->color1[index] = color_byte(length > 1.0e-12f ? (normal[index] / length + 1.0f) * 0.5f : 0.5f);
	vertex->color1[3] = 255;
}

/* a specular light's vertex: colour 0 (1 - e.l) / 2 of the eye's and the
light's directions (texture coordinates 2 and 3), normalized */
static void specular_vertex(const struct nv2a_cpu_output *out, struct gx_vertex *vertex)
{
	const float *e = out->t[2], *l = out->t[3];
	float ee = e[0] * e[0] + e[1] * e[1] + e[2] * e[2], ll = l[0] * l[0] + l[1] * l[1] + l[2] * l[2];
	float el = 0.0f;
	u8 level;

	if (ee > 1.0e-20f && ll > 1.0e-20f)
		el = (e[0] * l[0] + e[1] * l[1] + e[2] * l[2]) / nv2a_sqrtf(ee * ll);
	level = color_byte((1.0f - el) * 0.5f);
	vertex->color0[0] = vertex->color0[1] = vertex->color0[2] = level;
	vertex->color0[3] = 255;
}

static void vertex_store(const struct nv2a_cpu_output *out, const struct draw_projection *projection,
	const float *raw_position, struct gx_vertex *vertex)
{
	unsigned stages = store_plan.stages;
	int stage;

	if (raw_position)
	{
		/* (GX's position matrix makes it what the branch below would:
		position_matrix) */
		vertex->position[0] = raw_position[0];
		vertex->position[1] = raw_position[1];
		vertex->position[2] = raw_position[2];
	}
	else if (projection->perspective)
		clip_position(out->clip, vertex->position);
	else
	{
		/* the program's screen position (pixel centres on integers, as
		Direct3D puts them), its depth scaled back to 0..1 */
		float depth = out->position[2] / 16777215.0f;

		if (!(depth >= 0.0f))
			depth = 0.0f;
		if (depth > 1.0f)
			depth = 1.0f;
		vertex->position[0] = out->position[0] + 0.5f;
		vertex->position[1] = out->position[1] + 0.5f;
		vertex->position[2] = depth - 1.0f;
	}
	if (store_plan.live & NV2A_CPU_LIVE_D0)
		color_bytes(out->d0, vertex->color0);
	if (store_plan.live & NV2A_CPU_LIVE_D1)
		color_bytes(out->d1, vertex->color1);
	for (stage = 0; stages; stage++, stages >>= 1)
	{
		if (!(stages & 1))
			continue;
		if (store_plan.sphere & (1U << stage))
			sphere_coordinates(out->t[stage], vertex->texcoord[stage]);
		else if (store_plan.scaled & (1U << stage))
		{
			vertex->texcoord[stage][0] = out->t[stage][0] * texcoord_scale[stage][0];
			vertex->texcoord[stage][1] = out->t[stage][1] * texcoord_scale[stage][1];
		}
		else
		{
			vertex->texcoord[stage][0] = out->t[stage][0];
			vertex->texcoord[stage][1] = out->t[stage][1];
		}
	}
	if (store_plan.live & NV2A_CPU_LIVE_FOG)
		vertex->fog = store_plan.fog_table ? fog_coordinate(out->fog) : fog_ramp_coordinate(out->fog);
	/* (a pass's own terms, in place of what the program made of them) */
	if (vertex_terms == _gx_psh_vertices_bumped_reflection)
		bumped_reflection_vertex(out, vertex);
	else if (vertex_terms == _gx_psh_vertices_specular)
		specular_vertex(out, vertex);
}

static inline u32 color_word(const u8 color[4])
{
	u32 word;

	memcpy(&word, color, sizeof(word));
	return word;
}

static inline void vertex_emit(const struct gx_vertex *vertex)
{
	int stage;

	if (emit_matrix_index)
		GX_MatrixIndex1x8(vertex->matrix);
	GX_Position3f32(vertex->position[0], vertex->position[1], vertex->position[2]);
	/* (a colour's four bytes as one word: the same bytes in the FIFO) */
	if (emit_attributes == EMIT_ALL)
	{
		GX_Color1u32(color_word(vertex->color0));
		GX_Color1u32(color_word(vertex->color1));
		for (stage = 0; stage < 4; stage++)
			GX_TexCoord2f32(vertex->texcoord[stage][0], vertex->texcoord[stage][1]);
		GX_TexCoord1f32(vertex->fog);
		return;
	}
	if (emit_attributes & 1)
		GX_Color1u32(color_word(vertex->color0));
	else if (emit_attributes & EMIT_PAD)
		GX_Color1u32(0);
	if (emit_attributes & 2)
		GX_Color1u32(color_word(vertex->color1));
	for (stage = 0; stage < 4; stage++)
		if (emit_attributes & (4U << stage))
			GX_TexCoord2f32(vertex->texcoord[stage][0], vertex->texcoord[stage][1]);
	if (emit_attributes & 0x40)
		GX_TexCoord1f32(vertex->fog);
}

/* a processor's draw sends only what its stages read (after
transform_software, which takes every attribute back) */
static void emit_attributes_set(void)
{
	static const u8 attributes[7] = { GX_VA_CLR0, GX_VA_CLR1, GX_VA_TEX0, GX_VA_TEX1, GX_VA_TEX2, GX_VA_TEX3,
		GX_VA_TEX4 };
	unsigned wanted = (tev_channels & 3) | (tev_texcoords & 0x1F) << 2, index;

	if (wanted == EMIT_ALL)
		return;
	/* (never the position and its matrix index alone: the first colour
	stays, sent as 0) */
	if (!wanted && emit_matrix_index)
		wanted = EMIT_PAD;
	for (index = 0; index < 7; index++)
		if (!(wanted & (1U << index)) && !(index == 0 && (wanted & EMIT_PAD)))
			GX_SetVtxDesc(attributes[index], GX_NONE);
	emit_attributes = wanted;
	transform_hardware = TRUE;
}

/* an indexed draw's vertices, each run through its program once however
many of its indices name it (a strip names most twice): kept by index less
the draw's least, for draws whose indices span no more than this */
#define VERTEX_MEMO_SIZE 4096
static struct gx_vertex *vertex_memo;
static unsigned long *vertex_memo_stamps;
static unsigned long vertex_memo_draw;

static u8 gx_primitive(D3DPRIMITIVETYPE type);

/* the registers a stream draw's program reads that no stream gives
(draw_streams; 0 for the immediate draws, whose vertices carry every
register): they hold what D3DDevice_SetVertexData last set, in every vertex,
as the NV2A keeps a register its streams do not give. The detail objects'
cells are drawn so: each cell's place in register 2 and its reference
vector in register 3, then its quads from the one buffer. */
static unsigned long draw_unstreamed;

static void inputs_seed(float (*inputs)[4])
{
	unsigned long reg;

	for (reg = 0; reg < NV2A_CPU_INPUT_COUNT && (draw_unstreamed >> reg); reg++)
		if (draw_unstreamed & (1UL << reg))
			memcpy(inputs[reg], device.vertex_registers[reg], sizeof(inputs[reg]));
}

/* the draw log (debug.draw_log_tick, debug.draw_log_points): the first frame
of a level (not the main menu's map) drawn at or after that game tick has
each draw described in the log, the processor's and GX's alike, and the
given pixels of the back buffer read back after each draw, those it changed
logged, to name the draws that make a pixel what it is. Off, it costs a draw
a load and a compare (draw_logging). */
#define DRAW_LOG_POINTS_MOST 16
#define DRAW_LOG_WRITERS 8
static struct
{
	/* 0 the settings not read yet, 1 waiting for its tick, 2 off or done */
	int state;
	long tick;
	/* (this frame is the logged one; the frame last looked at) */
	BOOL on;
	unsigned long frame;
	/* the logged frame's draws so far (the number of each in the log) */
	unsigned long draws;
	unsigned point_count;
	u16 points[DRAW_LOG_POINTS_MOST][2];
	GXColor last[DRAW_LOG_POINTS_MOST];
	/* each point's last writers (draw numbers), and how many there were */
	unsigned long writers[DRAW_LOG_POINTS_MOST][DRAW_LOG_WRITERS];
	unsigned long writer_count[DRAW_LOG_POINTS_MOST];
} draw_log_state;

/* the game's clock (source/game/game.h) and whether the main menu's map is
the one playing (source/interface/ui_widget.h): its byte and long */
extern unsigned char game_time_initialized(void);
extern unsigned char game_in_progress(void);
extern long game_time_get(void);
extern unsigned char main_menu_is_active(void);

/* the settings: the tick, and the points ("x,y;x,y;...", the back buffer's
pixels, 640 by 480) */
static void draw_log_settings(void)
{
	const char *text = config_string("debug.draw_log_points");

	draw_log_state.tick = config_integer("debug.draw_log_tick");
	draw_log_state.state = draw_log_state.tick >= 0 ? 1 : 2;
	draw_log_state.point_count = 0;
	while (text && *text && draw_log_state.point_count < DRAW_LOG_POINTS_MOST)
	{
		char *end;
		long x = strtol(text, &end, 10), y;

		if (end == text || *end != ',')
			break;
		text = end + 1;
		y = strtol(text, &end, 10);
		if (end == text)
			break;
		if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT)
		{
			draw_log_state.points[draw_log_state.point_count][0] = (u16)x;
			draw_log_state.points[draw_log_state.point_count][1] = (u16)y;
			draw_log_state.point_count++;
		}
		while (*end == ' ' || *end == ';')
			end++;
		text = end;
	}
	if (draw_log_state.state == 1)
		platform_log("Direct3D: the draw log: the frame after tick %ld, %u points", draw_log_state.tick,
			draw_log_state.point_count);
}

/* the back buffer's pixels at the points, once GX has drawn what it was sent */
static void draw_log_peek(GXColor *pixels)
{
	unsigned point;

	GX_DrawDone();
	xf_flush_after();
	for (point = 0; point < draw_log_state.point_count; point++)
		GX_PeekARGB(draw_log_state.points[point][0], draw_log_state.points[point][1], &pixels[point]);
}

/* (a new frame: whether it is the one logged, and if so its points' pixels
before its first draw) */
static void draw_log_frame(void)
{
	unsigned point;

	draw_log_state.frame = device.frames;
	if (draw_log_state.state == 0)
		draw_log_settings();
	if (draw_log_state.state != 1 || draw_log_state.on || !game_time_initialized() || !game_in_progress() ||
		main_menu_is_active() || game_time_get() < draw_log_state.tick)
		return;
	draw_log_state.on = TRUE;
	draw_log_state.draws = 0;
	memset(draw_log_state.writer_count, 0, sizeof(draw_log_state.writer_count));
	platform_log("draw log: frame %lu, drawn after tick %ld", device.frames, (long)game_time_get());
	if (!draw_log_state.point_count)
		return;
	if (device.render_target == &device.back_buffer)
		draw_log_peek(draw_log_state.last);
	else
		memset(draw_log_state.last, 0, sizeof(draw_log_state.last));
	for (point = 0; point < draw_log_state.point_count; point++)
		platform_log("  pixel (%u, %u) before the frame's draws: %u %u %u %u", draw_log_state.points[point][0],
			draw_log_state.points[point][1], draw_log_state.last[point].r, draw_log_state.last[point].g,
			draw_log_state.last[point].b, draw_log_state.last[point].a);
}

/* whether this draw is described in the log (and then its number counted) */
static inline BOOL draw_logging(void)
{
	if (draw_log_state.state == 2)
		return FALSE;
	if (draw_log_state.frame != device.frames || draw_log_state.state == 0)
		draw_log_frame();
	return draw_log_state.on;
}

/* (in the logged frame, the render target set) */
static void draw_log_target(void)
{
	if (draw_log_state.on)
	{
		unsigned long width = SCREEN_WIDTH, height = SCREEN_HEIGHT;

		if (device.render_target && device.render_target != &device.back_buffer)
			surface_dimensions(device.render_target, &width, &height);
		platform_log("draw log: after draw %lu, the render target %s %lux%lu", draw_log_state.draws,
			device.render_target == &device.back_buffer ? "the back buffer" : "a surface", width, height);
	}
}

/* (the logged frame presented: each point's writers, and the log off) */
static void draw_log_frame_end(void)
{
	unsigned point;

	if (!draw_log_state.on)
		return;
	platform_log("draw log: frame %lu: %lu draws", device.frames, draw_log_state.draws);
	for (point = 0; point < draw_log_state.point_count; point++)
	{
		char line[200];
		unsigned long count = draw_log_state.writer_count[point], first, which;
		int length = snprintf(line, sizeof(line), "  pixel (%u, %u): %lu draws changed it",
			draw_log_state.points[point][0], draw_log_state.points[point][1], count);

		first = count > DRAW_LOG_WRITERS ? count - DRAW_LOG_WRITERS : 0;
		for (which = first; which < count && length < (int)sizeof(line) - 16; which++)
			length += snprintf(line + length, sizeof(line) - (size_t)length, "%s %lu",
				which == first ? (first ? ", the last:" : ":") : "", draw_log_state.writers[point][which % DRAW_LOG_WRITERS]);
		platform_log("%s; at the end %u %u %u %u", line, draw_log_state.last[point].r, draw_log_state.last[point].g,
			draw_log_state.last[point].b, draw_log_state.last[point].a);
	}
	draw_log_state.on = FALSE;
	draw_log_state.state = 2;
}

/* the logged frame's pixels after a draw to the back buffer: those it changed */
static void draw_log_pixels(void)
{
	GXColor pixels[DRAW_LOG_POINTS_MOST];
	unsigned point;

	if (device.render_target != &device.back_buffer || !draw_log_state.point_count)
		return;
	draw_log_peek(pixels);
	for (point = 0; point < draw_log_state.point_count; point++)
	{
		GXColor *was = &draw_log_state.last[point];
		const GXColor *now = &pixels[point];

		if (was->r == now->r && was->g == now->g && was->b == now->b && was->a == now->a)
			continue;
		platform_log("  pixel (%u, %u) changed by draw %lu: %u %u %u %u, was %u %u %u %u",
			draw_log_state.points[point][0], draw_log_state.points[point][1], draw_log_state.draws,
			now->r, now->g, now->b, now->a, was->r, was->g, was->b, was->a);
		draw_log_state.writers[point][draw_log_state.writer_count[point]++ % DRAW_LOG_WRITERS] = draw_log_state.draws;
		*was = *now;
	}
}

/* (a logged draw described: hardware the path, GX's transform or the
processor's) */
static void draw_log(D3DPRIMITIVETYPE primitive_type, UINT count, const struct draw_projection *projection,
	void (*fetch)(void *, UINT, float (*)[4]), void *context, struct nv2a_cpu_program *program, float point_size,
	BOOL hardware)
{
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	struct nv2a_cpu_output out;
	float minimum[2] = { 1.0e9f, 1.0e9f }, maximum[2] = { -1.0e9f, -1.0e9f };
	char textures[240];
	int length = 0, stage;
	UINT vertex;

	inputs_seed(inputs);
	for (vertex = 0; vertex < count && vertex < 64; vertex++)
	{
		fetch(context, vertex, inputs);
		nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size, &out);
		for (stage = 0; stage < 2; stage++)
		{
			if (out.position[stage] < minimum[stage])
				minimum[stage] = out.position[stage];
			if (out.position[stage] > maximum[stage])
				maximum[stage] = out.position[stage];
		}
	}
	textures[0] = 0;
	for (stage = 0; stage < 4; stage++)
	{
		const struct texture_entry *entry = bound_entries[stage];

		if (entry && length < (int)sizeof(textures) - 56)
			length += snprintf(textures + length, sizeof(textures) - (size_t)length, " t%d %lux%lu/%u%s%s@%08lx", stage,
				entry->width, entry->height, entry->gx_format, entry->native ? "n" : "d", entry->alpha_texels ? "+a" : "",
				(unsigned long)entry->data);
	}
	draw_log_state.draws++;
	platform_log("draw %lu (%s, program %08lx, target %s): prim %d x%u %s screen %.0f,%.0f-%.0f,%.0f d0 %.2f %.2f %.2f %.2f | blend %lu %lu>%lu atest %lu %lu/%lu z %lu/%lu/%lu wr %lx",
		draw_log_state.draws, hardware ? "GX's transform" : "the processor's", program->hash,
		device.render_target == &device.back_buffer ? "the back buffer" : "a texture", (int)primitive_type, count,
		projection->perspective ? "persp" : "ortho", minimum[0], minimum[1], maximum[0], maximum[1],
		out.d0[0], out.d0[1], out.d0[2], out.d0[3],
		(unsigned long)D3D__RenderState[D3DRS_ALPHABLENDENABLE], (unsigned long)D3D__RenderState[D3DRS_SRCBLEND],
		(unsigned long)D3D__RenderState[D3DRS_DESTBLEND], (unsigned long)D3D__RenderState[D3DRS_ALPHATESTENABLE],
		(unsigned long)D3D__RenderState[D3DRS_ALPHAFUNC], (unsigned long)D3D__RenderState[D3DRS_ALPHAREF],
		(unsigned long)D3D__RenderState[D3DRS_ZENABLE], (unsigned long)D3D__RenderState[D3DRS_ZFUNC],
		(unsigned long)D3D__RenderState[D3DRS_ZWRITEENABLE], (unsigned long)D3D__RenderState[D3DRS_COLORWRITEENABLE]);
	{
		float w_minimum = 1.0e30f, w_maximum = -1.0e30f;

		for (vertex = 0; vertex < count && vertex < 64; vertex++)
		{
			fetch(context, vertex, inputs);
			nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size, &out);
			if (out.clip[3] < w_minimum)
				w_minimum = out.clip[3];
			if (out.clip[3] > w_maximum)
				w_maximum = out.clip[3];
		}
		platform_log("  projection: %s, %s a %.6f b %.4f, w %.4f to %.4f (clip %d)",
			projection->perspective ? "perspective" : "screen", projection->fitted ? "fitted" : "the last fit's",
			projection->a, projection->b, w_minimum, w_maximum, out.clip_captured);
	}
	platform_log("  a vertex: t0 %.3f %.3f %.3f %.3f t1 %.3f %.3f t2 %.3f %.3f t3 %.3f %.3f d1 %.2f %.2f %.2f %.2f fog %.2f",
		out.t[0][0], out.t[0][1], out.t[0][2], out.t[0][3], out.t[1][0], out.t[1][1], out.t[2][0], out.t[2][1],
		out.t[3][0], out.t[3][1], out.d1[0], out.d1[1], out.d1[2], out.d1[3], out.fog);
	platform_log("  ps %lx fin %08lx %08lx modes %05lx |%s", (unsigned long)D3D__RenderState[D3DRS_PSCOMBINERCOUNT],
		(unsigned long)D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD],
		(unsigned long)D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG], (unsigned long)D3D__RenderState[D3DRS_PSTEXTUREMODES],
		textures);
	{
		unsigned long combiner;

		for (combiner = 0; combiner < (D3D__RenderState[D3DRS_PSCOMBINERCOUNT] & 0xff) && combiner < 8; combiner++)
			platform_log("  %lu: rgb %08lx -> %08lx, alpha %08lx -> %08lx", combiner,
				(unsigned long)D3D__RenderState[D3DRS_PSRGBINPUTS0 + combiner],
				(unsigned long)D3D__RenderState[D3DRS_PSRGBOUTPUTS0 + combiner],
				(unsigned long)D3D__RenderState[D3DRS_PSALPHAINPUTS0 + combiner],
				(unsigned long)D3D__RenderState[D3DRS_PSALPHAOUTPUTS0 + combiner]);
	}
	{
		const struct gx_psh_program *shader = pixel_shader_program;
		const DWORD *rs = D3D__RenderState;

		platform_log("  c0 %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx, final %08lx %08lx",
			(unsigned long)rs[D3DRS_PSCONSTANT0_0], (unsigned long)rs[D3DRS_PSCONSTANT0_0 + 1],
			(unsigned long)rs[D3DRS_PSCONSTANT0_0 + 2], (unsigned long)rs[D3DRS_PSCONSTANT0_0 + 3],
			(unsigned long)rs[D3DRS_PSCONSTANT0_0 + 4], (unsigned long)rs[D3DRS_PSCONSTANT0_0 + 5],
			(unsigned long)rs[D3DRS_PSCONSTANT0_0 + 6], (unsigned long)rs[D3DRS_PSCONSTANT0_0 + 7],
			(unsigned long)rs[D3DRS_PSFINALCOMBINERCONSTANT0], (unsigned long)rs[D3DRS_PSFINALCOMBINERCONSTANT1]);
		platform_log("  c1 %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx, fog %lu %08lx, shader %s%s%s",
			(unsigned long)rs[D3DRS_PSCONSTANT1_0], (unsigned long)rs[D3DRS_PSCONSTANT1_0 + 1],
			(unsigned long)rs[D3DRS_PSCONSTANT1_0 + 2], (unsigned long)rs[D3DRS_PSCONSTANT1_0 + 3],
			(unsigned long)rs[D3DRS_PSCONSTANT1_0 + 4], (unsigned long)rs[D3DRS_PSCONSTANT1_0 + 5],
			(unsigned long)rs[D3DRS_PSCONSTANT1_0 + 6], (unsigned long)rs[D3DRS_PSCONSTANT1_0 + 7],
			(unsigned long)rs[D3DRS_FOGENABLE], (unsigned long)rs[D3DRS_FOGCOLOR],
			!shader ? "?" : !pixel_shader_usable ? "beyond GX (texture times colour)" :
				shader->approximate ? "approximate: " : "exact",
			shader && pixel_shader_usable && shader->approximate && shader->approximation ?
				shader->approximation : "",
			"");
	}
}

/* a draw's vertices: each fetched (its inputs), run and sent */
typedef void (*vertex_fetch_proc)(void *context, UINT vertex, float (*inputs)[4]);

static struct frame_timing timing;
/* (the sums when the last frame ended: a slow frame's own share) */
static struct frame_timing last_timing;

/* the projection of a draw: a few of its vertices, for their w and depth */
static struct draw_projection projection_fit(struct nv2a_cpu_program *program, UINT count, vertex_fetch_proc fetch,
	void *context, float point_size)
{
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	struct nv2a_cpu_output out;
	struct draw_projection projection = { FALSE, 0.0f, 0.0f };
	UINT vertex;

	inputs_seed(inputs);
	{
		float first_w = 0.0f, first_depth = 0.0f;
		BOOL have_first = FALSE, fitted = FALSE;
		UINT probes = count < 24 ? count : 24;

		for (vertex = 0; vertex < probes && !fitted; vertex++)
		{
			fetch(context, vertex, inputs);
			nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size, &out);
			if (!out.clip_captured)
				break;
			projection.perspective = TRUE;
			if (!(out.clip[3] > 1.0e-4f))
				continue;
			if (!have_first)
			{
				first_w = out.clip[3];
				first_depth = out.position[2] / 16777215.0f;
				have_first = TRUE;
			}
			else
			{
				float inverse_difference = 1.0f / out.clip[3] - 1.0f / first_w;

				if (fabsf(inverse_difference) > 1.0e-6f * (1.0f / first_w))
				{
					float depth = out.position[2] / 16777215.0f;

					projection_b = (depth - first_depth) / inverse_difference;
					projection_a = first_depth - projection_b / first_w;
					fitted = TRUE;
				}
			}
		}
		/* (a w of 1 for all the vertices looked at: screen space, as the
		interface's quads are; any other w, the last fit's depth) */
		if (!fitted && have_first && fabsf(first_w - 1.0f) < 1.0e-4f)
			projection.perspective = FALSE;
		projection.a = projection_a;
		projection.b = projection_b;
		projection.fitted = fitted;
	}
	return projection;
}

/* the inputs a draw has the same in every vertex, as its program reads them
(draw_dependencies): the matrices are made with these values, and kept by
them */
static struct
{
	unsigned long mask;
	float values[NV2A_CPU_INPUT_COUNT][4];
} draw_uniform;

/* ... or, where those inputs take a few values (a model of several nodes, each
vertex bound to one), each value's vertices: a position matrix each, which
GX picks per vertex by its matrix index (GX_PNMTX1 on: GX_PNMTX0 is the
identity of the draws in screen space).

A model's vertex (model_vertex_compressed: two node indices, bytes, in v5,
and the first node's weight, a normalized short, in v6) whose weight is 1
or 0 hangs off one node, whatever its other index: the game's programs give
it what that node alone gives (port/wii/tests/nv2a_native_check.c checks
each), so such vertices are keyed by their node, and those that blend by
their two nodes and weight (node_keys). Those of a value with too few
vertices for a matrix of its own, past the nine, are the processor's
(KEY_PROCESSOR): the program computes their position, sent as draw_run
sends any through GX_PNMTX0 and the same projection, in the same draw. */
#define MATRIX_KEYS 9
#define KEY_PROCESSOR 0xFF
#define NODE_INDICES 5
#define NODE_WEIGHT 6
/* (the indices a node needs for a matrix: a matrix costs the program's
position four times, which so many vertices' positions on GX repay) */
#define NODE_KEY_LEAST 6
static struct
{
	unsigned count;
	/* the draw's vertices keyed KEY_PROCESSOR */
	UINT processor;
	/* a vertex of each value, of the draw's */
	UINT representative[MATRIX_KEYS];
	Mtx rows[MATRIX_KEYS];
} draw_keys;
/* each index's key, as node_keys_find and draw_dependencies find them, and
as the draw reads them (there, or a record's: node_keys) */
static unsigned char vertex_keys[65536];
static const unsigned char *draw_key_of = vertex_keys;
/* (the KEY_PROCESSOR vertices the processor ran, in the statistics' window) */
static unsigned long processor_positions;

/* whether the draw's vertices carry a matrix index (else every vertex is the
one matrix's) */
static inline BOOL draw_keyed(void)
{
	return draw_keys.count > 1 || draw_keys.processor;
}

struct stream_fetch;
static const struct nv2a_cpu_dependencies *draw_dependencies(struct vertex_shader_object *shader,
	struct nv2a_cpu_program *program, struct stream_fetch *fetch, UINT count);
static void stream_fetch(void *context, UINT vertex, float (*inputs)[4]);

/* ---------- the position in hardware

A program whose clip-space x, y and w are affine in one register of three
floats has them as a matrix (its outputs for no input and for each
component alone): GX's position matrix then makes draw_run's (x w, y w, -w)
from the vertex's own position, for every draw of such a program, whether
GX computes the rest (draw_hardware) or the processor does (draw_run). A
surface's passes, some on each path, then reach the same depths, which
their equal depth tests need. */

static int position_register_of(const struct vertex_shader_object *shader,
	const struct nv2a_cpu_dependencies *dependencies)
{
	unsigned long mask = dependencies->clip[0] | dependencies->clip[1] | dependencies->clip[3];
	unsigned long index, bits = mask & 0xFFFF;
	int reg;

	if (!dependencies->clip_captured || (mask & NV2A_CPU_NOT_AFFINE) || !bits || (bits & (bits - 1)))
		return -1;
	for (reg = 0; !(bits & (1UL << reg)); reg++)
		;
	for (index = 0; index < shader->element_count; index++)
		if ((shader->elements[index].reg & 15) == reg)
			return shader->elements[index].type == D3DVSDT_FLOAT3 ? reg : -1;
	return -1;
}

/* the inputs a matrix's columns start from: none, but each element's w 1
(as stream_fetch gives it), and the registers no stream gives as they are
set (draw_unstreamed) */
static void matrix_inputs(const struct vertex_shader_object *shader, float (*inputs)[4])
{
	unsigned long index;

	memset(inputs, 0, NV2A_CPU_INPUT_COUNT * sizeof(*inputs));
	for (index = 0; index < shader->element_count; index++)
		inputs[shader->elements[index].reg & 15][3] = 1.0f;
	inputs_seed(inputs);
	for (index = 0; index < NV2A_CPU_INPUT_COUNT; index++)
		if (draw_uniform.mask & (1UL << index))
			memcpy(inputs[index], draw_uniform.values[index], sizeof(inputs[index]));
}

static Mtx position_matrix_rows;
/* in the statistics' window: the position matrices' runs of a program's four
at once, and of one at a time */
static unsigned long position_lanes_runs, position_single_runs;
/* the depth's a + b / w for the same constants (position_matrix): from the
program's outputs for the origin and the axes, not from a draw's vertices,
so every draw of these constants has the same, to the bit */
static struct draw_projection position_projection;
/* (config.toml's debug.gpu_transform; -1 not read yet) */
static int gpu_transform = -1;
/* (debug.gx_display_lists: index_list_draw) */
static int index_lists = -1;

/* the matrices and depths of recent constants, by the program, its vertex
declaration, the constants it reads and the viewport's */
#define POSITION_CACHE_ENTRIES 64
static struct
{
	unsigned long long key;
	Mtx rows;
	/* (its fitted is FALSE where the depth was not wanted: a model's other
	nodes, position_keys) */
	struct draw_projection projection;
} position_cache[POSITION_CACHE_ENTRIES];

/* each draw's number (draw_streams, D3DDevice_End), by which what is the
same for the whole draw is found once (draw_dependencies) */
static unsigned long draw_serial;

/* the constants a draw's program reads (and the viewport's), hashed: the
start of the matrices' keys (constants_key, hardware_key). The hash of a
program and declaration is kept while no constant is set
(constants_generation), for its next draw: a model's other passes and
parts, and each matrix of its nodes. */
#define CONSTANTS_HASHES 8

static unsigned long long constants_hash(const struct vertex_shader_object *shader,
	const struct nv2a_cpu_program *program)
{
	static struct
	{
		unsigned long generation;
		const void *shader, *program;
		unsigned long long hash;
	} hashes[CONSTANTS_HASHES];
	static unsigned next;
	const struct nv2a_cpu_dependencies *dependencies = &shader->dependencies;
	unsigned long long hash;
	u32 lanes[2] = { 0, 0 };
	unsigned long part;
	unsigned which;

	for (which = 0; which < CONSTANTS_HASHES; which++)
		if (hashes[which].generation == constants_generation && hashes[which].shader == shader &&
			hashes[which].program == program)
			return hashes[which].hash;
	if (!constant_rows_ready)
		constant_rows_changed(0, VERTEX_CONSTANT_COUNT);
	/* the rows read (every row for a program that indexes them, a0), and the
	viewport's: the exclusive or of their hashes, each row once */
	if (dependencies->relative)
	{
		lanes[0] = constant_rows_all[0];
		lanes[1] = constant_rows_all[1];
	}
	else
		for (part = 0; part < NV2A_CPU_CONSTANT_COUNT / 32; part++)
		{
			unsigned long bits = dependencies->constants[part];

			if (part == (VERTEX_CONSTANT_BIAS - 38) / 32)
				bits |= 1UL << ((VERTEX_CONSTANT_BIAS - 38) % 32);
			if (part == (VERTEX_CONSTANT_BIAS - 37) / 32)
				bits |= 1UL << ((VERTEX_CONSTANT_BIAS - 37) % 32);
			while (bits)
			{
				unsigned long row = part * 32 + (unsigned long)__builtin_ctzl(bits);

				lanes[0] ^= constant_row_hashes[row][0];
				lanes[1] ^= constant_row_hashes[row][1];
				bits &= bits - 1;
			}
		}
	/* (and the program and declaration) */
	hash = ((unsigned long long)hash_mix(lanes[0] ^ (u32)(unsigned long)program) << 32 |
		hash_mix(lanes[1] ^ (u32)(unsigned long)shader * 0x9E3779B9U)) ^ (unsigned long long)dependencies->relative;
	which = next++ % CONSTANTS_HASHES;
	hashes[which].generation = constants_generation;
	hashes[which].shader = shader;
	hashes[which].program = program;
	hashes[which].hash = hash;
	return hash;
}

/* a key's hash and a word more: two lanes of 32-bit FNV-1 (the processor
multiplies 64 bits in some twenty instructions, 32 in one) */
static inline unsigned long long hash_step(unsigned long long hash, u32 word)
{
	u32 low = ((u32)hash ^ word) * 16777619U, high = ((u32)(hash >> 32) ^ word) * 2246822519U;

	return (unsigned long long)high << 32 | low;
}

static unsigned long long hash_words(unsigned long long hash, const void *data, unsigned long bytes)
{
	const u32 *words = data;
	unsigned long index;

	for (index = 0; index < bytes / 4; index++)
		hash = hash_step(hash, words[index]);
	return hash;
}

static unsigned long long constants_key(const struct vertex_shader_object *shader, const struct nv2a_cpu_program *program)
{
	unsigned long long hash = constants_hash(shader, program);
	unsigned long row, word;

	hash = hash_step(hash, (u32)draw_uniform.mask);
	for (row = 0; row < NV2A_CPU_INPUT_COUNT; row++)
		if (draw_uniform.mask & (1UL << row))
		{
			const unsigned long *words = (const unsigned long *)draw_uniform.values[row];

			for (word = 0; word < 4; word++)
				hash = hash_step(hash, (u32)words[word]);
		}
	/* (and the registers no stream gives: draw_unstreamed) */
	hash = hash_step(hash, (u32)draw_unstreamed);
	for (row = 0; row < NV2A_CPU_INPUT_COUNT; row++)
		if (draw_unstreamed & (1UL << row))
		{
			const unsigned long *words = (const unsigned long *)device.vertex_registers[row];

			for (word = 0; word < 4; word++)
				hash = hash_step(hash, (u32)words[word]);
		}
	return hash ? hash : 1;
}

static BOOL position_matrix_compute(const struct vertex_shader_object *shader, struct nv2a_cpu_program *program,
	int reg, float point_size, BOOL fit);

/* the position matrix (position_matrix_rows) for the constants and
draw_uniform, and where fit, its depth (position_projection) */
static BOOL position_matrix(const struct vertex_shader_object *shader, struct nv2a_cpu_program *program, int reg,
	float point_size, BOOL fit)
{
	unsigned long long key = constants_key(shader, program);
	unsigned slot = (unsigned)(key % POSITION_CACHE_ENTRIES);

	if (position_cache[slot].key == key && (position_cache[slot].projection.fitted || !fit))
	{
		memcpy(position_matrix_rows, position_cache[slot].rows, sizeof(position_matrix_rows));
		position_projection = position_cache[slot].projection;
		return TRUE;
	}
	if (!position_matrix_compute(shader, program, reg, point_size, fit))
		return FALSE;
	position_cache[slot].key = key;
	memcpy(position_cache[slot].rows, position_matrix_rows, sizeof(position_matrix_rows));
	position_cache[slot].projection = position_projection;
	return TRUE;
}

/* the program's clip-space and screen-space positions for up to four
values of the position's register (points), the other inputs as given: its
compiled function for four at once where it has one and the position is v0
(nv2a_cpu_run_positions, the same to the bit), else a run for each */
static void position_runs(struct nv2a_cpu_program *program, float (*inputs)[4], int reg, float point_size,
	const float (*points)[4], int count, float (*clips)[4], float (*screens)[4])
{
	float saved[4];
	int point;

	if (reg == 0 && nv2a_cpu_run_positions(program, (const float (*)[4])device.vertex_constants,
		(const float (*)[4])inputs, points, clips, screens))
	{
		position_lanes_runs++;
		return;
	}
	memcpy(saved, inputs[reg], sizeof(saved));
	for (point = 0; point < count; point++)
	{
		struct nv2a_cpu_output out;

		memcpy(inputs[reg], points[point], sizeof(inputs[reg]));
		nv2a_cpu_run_live(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size,
			&out, NV2A_CPU_LIVE_POSITION);
		memcpy(clips[point], out.clip, sizeof(clips[point]));
		memcpy(screens[point], out.position, sizeof(screens[point]));
	}
	memcpy(inputs[reg], saved, sizeof(saved));
	position_single_runs += (unsigned long)count;
}

static BOOL position_matrix_compute(const struct vertex_shader_object *shader, struct nv2a_cpu_program *program,
	int reg, float point_size, BOOL fit)
{
	float ws[4];
	const float *scale = device.vertex_constants[VERTEX_CONSTANT_BIAS - 38];
	const float *offset = device.vertex_constants[VERTEX_CONSTANT_BIAS - 37];
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	/* (the origin, then each axis: the base and the columns) */
	float points[4][4], clips[4][4], screens[4][4];
	int component;

	/* (the program for its position alone: the same, bit for bit) */
	matrix_inputs(shader, inputs);
	for (component = 0; component < 4; component++)
	{
		memcpy(points[component], inputs[reg], sizeof(points[component]));
		if (component)
			points[component][component - 1] = 1.0f;
	}
	position_runs(program, inputs, reg, point_size, (const float (*)[4])points, 4, clips, screens);
	for (component = 0; component < 4; component++)
	{
		float x = clips[0][0], y = clips[0][1], w = clips[0][3];

		if (component < 3)
		{
			x = clips[1 + component][0] - clips[0][0];
			y = clips[1 + component][1] - clips[0][1];
			w = clips[1 + component][3] - clips[0][3];
		}
		position_matrix_rows[0][component] = x * scale[0] + (offset[0] + 0.5f) * w;
		position_matrix_rows[1][component] = y * scale[1] + (offset[1] + 0.5f) * w;
		position_matrix_rows[2][component] = -w;
		ws[component] = component < 3 ? clips[1 + component][3] : clips[0][3];
	}
	/* two points along w's gradient, at w 1 and 1000, give the fit (draw_run's
	projection_fit, for points of the program's own choosing) */
	{
		float gradient[3], length = 0.0f, targets[2] = { 1.0f, 1000.0f }, d[2], v[2];
		int point;

		for (component = 0; component < 3; component++)
		{
			gradient[component] = ws[component] - ws[3];
			length += gradient[component] * gradient[component];
		}
		position_projection.perspective = TRUE;
		position_projection.fitted = FALSE;
		if (!fit)
			return TRUE;
		if (!(length > 1.0e-12f))
			return FALSE;
		for (point = 0; point < 4; point++)
		{
			float t = (targets[point & 1] - ws[3]) / length;

			memcpy(points[point], inputs[reg], sizeof(points[point]));
			for (component = 0; component < 3; component++)
				points[point][component] = t * gradient[component];
		}
		position_runs(program, inputs, reg, point_size, (const float (*)[4])points, 2, clips, screens);
		for (point = 0; point < 2; point++)
		{
			d[point] = screens[point][2] / 16777215.0f;
			v[point] = clips[point][3];
		}
		if (!(fabsf(v[0]) > 1.0e-6f) || !(fabsf(v[1]) > 1.0e-6f) || !(fabsf(1.0f / v[1] - 1.0f / v[0]) > 1.0e-6f))
			return FALSE;
		position_projection.b = (d[1] - d[0]) / (1.0f / v[1] - 1.0f / v[0]);
		position_projection.a = d[0] - position_projection.b / v[0];
		position_projection.fitted = TRUE;
	}
	return position_projection.fitted;
}

/* (after transform_software, which takes GX back to the identity) */
static void position_matrix_load(void)
{
	if (draw_keyed())
	{
		unsigned key;

		for (key = 0; key < draw_keys.count; key++)
			GX_LoadPosMtxImm(draw_keys.rows[key], (u32)(GX_PNMTX1 + 3 * key));
		GX_SetVtxDesc(GX_VA_PTNMTXIDX, GX_DIRECT);
	}
	else
	{
		GX_LoadPosMtxImm(position_matrix_rows, GX_PNMTX1);
		GX_SetCurrentMtx(GX_PNMTX1);
	}
	transform_hardware = TRUE;
}

/* each of the draw's values a position matrix (draw_keys), its first's left
in position_matrix_rows, position_projection (the only depth fitted: the
draw's) and draw_uniform */
static BOOL position_keys(const struct vertex_shader_object *shader, struct nv2a_cpu_program *program, void *fetch,
	int reg, float point_size)
{
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	unsigned key;
	int index;

	inputs_seed(inputs);
	for (key = draw_keys.count; key-- > 0; )
	{
		stream_fetch(fetch, draw_keys.representative[key], inputs);
		for (index = 0; index < NV2A_CPU_INPUT_COUNT; index++)
			if (draw_uniform.mask & (1UL << index))
				memcpy(draw_uniform.values[index], inputs[index], sizeof(inputs[index]));
		if (!position_matrix(shader, program, reg, point_size, key == 0))
			return FALSE;
		memcpy(draw_keys.rows[key], position_matrix_rows, sizeof(position_matrix_rows));
	}
	return TRUE;
}

/* the vertices each program ran for, in the statistics' window: which to
draw in hardware first */
#define PROGRAM_STATISTICS 24
static struct { unsigned long hash, vertices, draws; } program_statistics[PROGRAM_STATISTICS];

static void program_statistics_add(const struct nv2a_cpu_program *program, unsigned long vertices)
{
	unsigned index, least = 0;

	for (index = 0; index < PROGRAM_STATISTICS; index++)
	{
		if (program_statistics[index].hash == program->hash || !program_statistics[index].vertices)
		{
			program_statistics[index].hash = program->hash;
			program_statistics[index].vertices += vertices;
			program_statistics[index].draws++;
			return;
		}
		if (program_statistics[index].vertices < program_statistics[least].vertices)
			least = index;
	}
	program_statistics[least].hash = program->hash;
	program_statistics[least].vertices = vertices;
	program_statistics[least].draws = 1;
}

static void program_statistics_log(void)
{
	char line[400];
	int length = 0, rank;

	for (rank = 0; rank < 8; rank++)
	{
		unsigned index, best = PROGRAM_STATISTICS;

		for (index = 0; index < PROGRAM_STATISTICS; index++)
			if (program_statistics[index].vertices &&
				(best == PROGRAM_STATISTICS || program_statistics[index].vertices > program_statistics[best].vertices))
				best = index;
		if (best == PROGRAM_STATISTICS)
			break;
		length += snprintf(line + length, sizeof(line) - (size_t)length, " %08lx %lu (%lu draws)",
			program_statistics[best].hash, program_statistics[best].vertices, program_statistics[best].draws);
		if (length >= (int)sizeof(line))
			break;
		program_statistics[best].vertices = 0;
	}
	platform_log("Direct3D: vertex programs run most:%s", line);
	memset(program_statistics, 0, sizeof(program_statistics));
}

static void draw_run(struct nv2a_cpu_program *program, D3DPRIMITIVETYPE primitive_type, UINT count,
	vertex_fetch_proc fetch, void *context, const WORD *indices, const struct vertex_shader_object *shader)
{
	int position_register = -1;
	uint64_t started = timing_now();
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	struct nv2a_cpu_output out;
	struct draw_projection projection = { FALSE, 0.0f, 0.0f };
	float point_size;
	UINT vertex;
	/* (the draw described in the log: draw_logging) */
	BOOL logged = draw_logging();
	/* (whether the vertices carry a matrix index, KEY_PROCESSOR's the
	identity; the outputs of a vertex whose position GX computes) */
	BOOL keyed = FALSE;
	unsigned live_gx;

	memcpy(&point_size, &D3D__RenderState[D3DRS_POINTSIZE], sizeof(point_size));
	inputs_seed(inputs);
	if (gpu_transform < 0)
		gpu_transform = config_boolean("debug.gpu_transform") ? 1 : 0;
	if (shader && gpu_transform)
	{
		position_register = position_register_of(shader,
			draw_dependencies((struct vertex_shader_object *)shader, program, (struct stream_fetch *)context, count));
		if (position_register >= 0 && (draw_keyed() ?
			position_keys(shader, program, context, position_register, point_size) :
			position_matrix(shader, program, position_register, point_size, TRUE)))
		{
			projection = position_projection;
			keyed = draw_keyed();
		}
		else
			position_register = -1;
	}
	if (position_register < 0)
		projection = projection_fit(program, count, fetch, context, point_size);
	projection_load(&projection);
	if (logged)
		draw_log(primitive_type, count, &projection, fetch, context, program, point_size, FALSE);
	if (indices && count > 3 && !vertex_memo)
	{
		vertex_memo = malloc(VERTEX_MEMO_SIZE * sizeof(*vertex_memo));
		vertex_memo_stamps = calloc(VERTEX_MEMO_SIZE, sizeof(*vertex_memo_stamps));
		if (!vertex_memo || !vertex_memo_stamps)
		{
			free(vertex_memo);
			free(vertex_memo_stamps);
			vertex_memo = NULL;
			vertex_memo_stamps = NULL;
			indices = NULL;
		}
	}
	if (indices && count > 3 && vertex_memo)
	{
		WORD least = 0xFFFF, most = 0;

		for (vertex = 0; vertex < count; vertex++)
		{
			if (indices[vertex] < least)
				least = indices[vertex];
			if (indices[vertex] > most)
				most = indices[vertex];
		}
		if ((unsigned)(most - least) >= VERTEX_MEMO_SIZE)
			indices = NULL;
		else
		{
			vertex_memo_draw++;
			transform_software();
			if (position_register >= 0)
			{
				position_matrix_load();
				emit_matrix_index = keyed;
			}
			if (shader)
				emit_attributes_set();
			store_plan_set();
			live_gx = position_register >= 0 ? store_plan.live & ~NV2A_CPU_LIVE_POSITION : store_plan.live;

			gx_begin(gx_primitive(primitive_type), DEVICE_VERTEX_FORMAT, (u16)count);
			for (vertex = 0; vertex < count; vertex++)
			{
				unsigned slot = indices[vertex] - least;

				if (vertex_memo_stamps[slot] != vertex_memo_draw)
				{
					u32 t0 = PPCMftb(), t1, t2;
					BOOL processor = keyed && draw_key_of[vertex] == KEY_PROCESSOR;

					fetch(context, vertex, inputs);
					t1 = PPCMftb();
					nv2a_cpu_run_live(program, (const float (*)[4])device.vertex_constants,
						(const float (*)[4])inputs, point_size, &out, processor ? store_plan.live : live_gx);
					t2 = PPCMftb();
					vertex_store(&out, &projection, position_register >= 0 && !processor ? inputs[position_register] : NULL,
						&vertex_memo[slot]);
					vertex_memo[slot].matrix = (u8)(processor ? GX_PNMTX0 : GX_PNMTX1 + 3 * draw_key_of[vertex]);
					vertex_memo_stamps[slot] = vertex_memo_draw;
					if (processor)
						processor_positions++;
					device.statistics.vertices_run++;
					timing.fetch_ticks += t1 - t0;
					timing.program_ticks += t2 - t1;
					timing.store_ticks += PPCMftb() - t2;
				}
				{
					u32 t3 = PPCMftb();

					vertex_emit(&vertex_memo[slot]);
					timing.emit_ticks += PPCMftb() - t3;
				}
			}
			GX_End();
		}
	}
	else
		indices = NULL;
	if (!indices)
	{
		transform_software();
		if (position_register >= 0)
		{
			position_matrix_load();
			emit_matrix_index = keyed;
		}
		if (shader)
			emit_attributes_set();
		store_plan_set();
		live_gx = position_register >= 0 ? store_plan.live & ~NV2A_CPU_LIVE_POSITION : store_plan.live;

		gx_begin(gx_primitive(primitive_type), DEVICE_VERTEX_FORMAT, (u16)count);
		for (vertex = 0; vertex < count; vertex++)
		{
			struct gx_vertex sent;
			BOOL processor = keyed && draw_key_of[vertex] == KEY_PROCESSOR;

			sent.matrix = (u8)(processor ? GX_PNMTX0 : GX_PNMTX1 + 3 * draw_key_of[vertex]);
			fetch(context, vertex, inputs);
			nv2a_cpu_run_live(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs,
				point_size, &out, processor ? store_plan.live : live_gx);
			vertex_store(&out, &projection, position_register >= 0 && !processor ? inputs[position_register] : NULL,
				&sent);
			vertex_emit(&sent);
			if (processor)
				processor_positions++;
		}
		device.statistics.vertices_run += count;
		GX_End();
	}
	/* (the logged frame: a pixel read back after each draw, to see which
	draw makes it what it is) */
	if (logged)
		draw_log_pixels();
	program_statistics_add(program, count);
	{
		uint64_t spent = timing_now() - started;

		timing.draws += spent;
		if (shader)
			timing.processor += spent;
		else
			timing.immediate += spent;
	}
}

static u8 gx_primitive(D3DPRIMITIVETYPE type)
{
	switch (type)
	{
	case D3DPT_POINTLIST: return GX_POINTS;
	case D3DPT_LINELIST: return GX_LINES;
	case D3DPT_LINELOOP: case D3DPT_LINESTRIP: return GX_LINESTRIP;
	case D3DPT_TRIANGLELIST: return GX_TRIANGLES;
	case D3DPT_TRIANGLESTRIP: case D3DPT_QUADSTRIP: return GX_TRIANGLESTRIP;
	case D3DPT_TRIANGLEFAN: case D3DPT_POLYGON: return GX_TRIANGLEFAN;
	case D3DPT_QUADLIST: return GX_QUADS;
	default: return GX_TRIANGLES;
	}
}

static struct nv2a_cpu_program *program_current(void)
{
	struct vertex_shader_object *shader = device.vertex_shader;

	if (!shader || !shader->instructions)
		return NULL;
	if (!shader->program)
	{
		shader->program = malloc(sizeof(*shader->program));
		if (!shader->program)
			return NULL;
		nv2a_cpu_compile(shader->instructions, shader->instruction_count, shader->program);
		nv2a_cpu_analyze(shader->program, &shader->dependencies);
		nv2a_cpu_analyze_constants(shader->program, &shader->constant_reads);
	}
	return shader->program;
}

/* ---------- the EFB's owner

GX draws into one frame buffer, the EFB. The back buffer has it for the
frame; a draw or clear into another render target takes it over. WiiLibs
moves it (wl/platform/gx_target.h): the back buffer, if anything was drawn
into it, is parked first, its colour and depth copied out; the target is
drawn at the EFB's origin, within a scissor of its size; when another
surface takes the EFB, the target's pixels are copied into its own memory
in GX's format, and the back buffer is put back by a quad that writes the
parked colour, and the parked depth through a Z texture.

The device says which surface each draw goes to (efb_take), records the
targets that were copied (render_target_resolved), so that their textures
are bound from there, and sets again what the quads leave of its own GX
state (efb_device_state). A clear once the frame has begun is a quad of its
colour and depth (efb_clear_quad). */

/* where the back buffer is parked: in the contiguous window, where there is
room; else the heap */
static void *efb_park_alloc(size_t bytes)
{
	void *memory = platform_contiguous_alloc((unsigned long)bytes, 32, PLATFORM_ANY_PHYSICAL_ADDRESS, 0);

	if (!memory)
		memory = aligned_alloc(32, bytes);
	if (!memory)
		platform_log("Direct3D: no memory to park the back buffer: render targets draw over it");
	return memory;
}

static void efb_park_free(void *memory)
{
	if (platform_is_contiguous(memory))
		platform_contiguous_free(memory);
	else
		free(memory);
}

/* GX's format for a render target's texture, of the same size as the
Xbox's; 0 when it has none */
static u8 render_target_gx_format(const D3DSurface *surface)
{
	switch ((surface->Format & D3DFORMAT_FORMAT_MASK) >> D3DFORMAT_FORMAT_SHIFT)
	{
	case 0x06: case 0x07: case 0x12: case 0x1E: return GX_TF_RGBA8;
	case 0x05: case 0x11: return GX_TF_RGB565;
	default: return 0;
	}
}

/* a render target as WiiLibs takes it: its memory (the texture's, where the
copy goes), size and GX format, and its Data for the record of the copy; the
format WiiLibs has none for where GX has none */
static void render_target_describe(D3DSurface *surface, WlGxTarget *target)
{
	unsigned long width, height;
	u8 format = render_target_gx_format(surface);

	surface_dimensions(surface, &width, &height);
	memset(target, 0, sizeof(*target));
	target->id = surface;
	target->memory = surface->Data ? PLATFORM_PHYSICAL_TO_VIRTUAL(surface->Data) : NULL;
	target->width = (uint16_t)(width < 0xFFFF ? width : 0xFFFF);
	target->height = (uint16_t)(height < 0xFFFF ? height : 0xFFFF);
	target->format = format == GX_TF_RGBA8 ? WL_GX_TEX_RGBA8 : format == GX_TF_RGB565 ? WL_GX_TEX_RGB565 :
		WL_GX_TEX_FORMAT_COUNT;
	target->user = (uintptr_t)surface->Data;
}

/* a target whose pixels were copied into its memory is recorded, so that its
texture is bound from there */
static void render_target_resolved(const WlGxTarget *target)
{
	struct render_target_record *record = render_target_record_find((DWORD)target->user);

	if (!record)
	{
		record = &render_target_records[render_target_record_next];
		render_target_record_next = (render_target_record_next + 1) % RENDER_TARGET_RECORDS;
	}
	record->data = (DWORD)target->user;
	record->width = target->width;
	record->height = target->height;
	record->gx_format = target->format == WL_GX_TEX_RGB565 ? GX_TF_RGB565 : GX_TF_RGBA8;
}

/* what the frame's display copy (wl_gx_end_frame's) reads, as WiiLibs set
it up: the whole EFB, its scale to the XFB and its filter, and the screen's
viewport and scissor */
static void display_copy_restore(void)
{
	GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);

	GX_SetViewport(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 1.0f);
	GX_SetScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	if (!mode)
		return;
	GX_SetDispCopySrc(0, 0, mode->fbWidth, mode->efbHeight);
	GX_SetDispCopyDst(mode->fbWidth, (u16)GX_SetDispCopyYScale(GX_GetYScaleFactor(mode->efbHeight, mode->xfbHeight)));
	GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
}

/* WiiLibs' quads (the back buffer put back, a clear) set GX for themselves
and leave the device's state as it found it only in the shadow's sense: the
projection, the colour channels and texture coordinates the frame began with,
and the vertices the processor's draws send, are put back */
static void efb_device_state(void)
{
	projection_valid = FALSE;
	states_shadow_reset();
	GX_SetNumChans(2);
	GX_SetNumTexGens(5);
	transform_hardware = TRUE;
	transform_software();
}

/* the EFB for the current render target */
static void efb_take(void)
{
	D3DSurface *surface = device.render_target;
	const WlGxTarget *held = wl_gx_target_current();
	const void *before = held ? held->id : NULL;
	const void *after = surface == &device.back_buffer ? NULL : (const void *)surface;
	const WlGxTarget *resolved;

	if (surface == &device.back_buffer)
	{
		/* (the back buffer put back by a quad: no indirect stage reads) */
		if (before)
			indirect_apply(NULL);
		resolved = wl_gx_target_use(NULL);
		if (before)
			states_shadow_reset();
	}
	else
	{
		WlGxTarget target;

		render_target_describe(surface, &target);
		resolved = wl_gx_target_use(&target);
		states_shadow_reset();
	}
	if (resolved)
		render_target_resolved(resolved);
	/* (a target's copy out of the EFB, the back buffer's parked or put back) */
	if (resolved || before != after)
		xf_flush_after();
	if (before != after)
	{
		/* (GX's texture cache may hold what was in the memory copied to) */
		texture_memory_changed = TRUE;
		efb_device_state();
	}
}

/* a clear of the current target's viewport, once the frame has begun */
static void efb_clear_quad(DWORD flags, D3DCOLOR color, float z)
{
	/* ARGB to the RGBA WiiLibs takes */
	uint32_t rgba = ((uint32_t)color << 8) | ((uint32_t)color >> 24);

	indirect_apply(NULL);
	wl_gx_target_clear((flags & D3DCLEAR_TARGET) ? true : false, (flags & D3DCLEAR_ZBUFFER) ? true : false, rgba, z,
		(int)device.viewport.X, (int)device.viewport.Y, (int)device.viewport.Width, (int)device.viewport.Height);
	xf_flush_after();
	efb_device_state();
}

/* whether a draw goes to the screen now (the frame begun, its state set) */
static BOOL draw_prepare(void)
{
	if (!device.gx_ready)
		return FALSE;
	frame_begin(device.clear_color);
	efb_take();
	if (device.render_target == &device.back_buffer)
		wl_gx_target_drawn();
	else
		device.draws_elsewhere++;
	states_apply();
	{
		uint64_t started = timing_now();
		unsigned stages = textures_bind();
		uint64_t bound = timing_now();

		tev_setup(stages);
		timing.textures += bound - started;
		timing.pixel_shaders += timing_now() - bound;
	}
	return TRUE;
}

/* vertices [first, first + count) of the streams, or those the indices
name, through the program */
struct fetch_element
{
	/* (the element's data in vertex 0: past its offset, and the draw's
	first vertex or base index) */
	const unsigned char *base;
	UINT stride;
	unsigned char kind, count, reg;
};

struct stream_fetch
{
	const struct vertex_shader_object *shader;
	const unsigned char *streams[MAXIMUM_STREAMS];
	const WORD *indices;
	UINT first;
	/* the elements of the registers the program reads, whose stream is
	set (stream_fetch_plan) */
	unsigned element_count;
	struct fetch_element elements[NV2A_CPU_INPUT_COUNT];
};

static void stream_fetch_plan(struct stream_fetch *fetch)
{
	const struct vertex_shader_object *shader = fetch->shader;
	unsigned long element, read = shader->dependencies.inputs;
	UINT first = fetch->indices ? device.base_vertex_index : fetch->first;

	if (!fetch_tables_made)
		fetch_tables_make();
	fetch->element_count = 0;
	draw_unstreamed = read & 0xFFFF;
	for (element = 0; element < shader->element_count && element < NV2A_CPU_INPUT_COUNT; element++)
	{
		const struct vertex_element *source = &shader->elements[element];
		unsigned stream = source->stream & (MAXIMUM_STREAMS - 1);
		struct fetch_element *planned;
		BOOL known;

		/* (a register the program never reads: left as the caller has it) */
		if (!fetch->streams[stream] || !(read & (1UL << (source->reg & 15))))
			continue;
		planned = &fetch->elements[fetch->element_count++];
		draw_unstreamed &= ~(1UL << (source->reg & 15));
		planned->stride = device.streams[stream].stride;
		planned->base = fetch->streams[stream] + first * planned->stride + source->offset;
		planned->reg = source->reg & 15;
		planned->kind = (unsigned char)fetch_kind_of(source->type, &known);
		planned->count = known ? (unsigned char)(source->type >> 4) : 0;
	}
}

static void stream_fetch(void *context, UINT vertex, float (*inputs)[4])
{
	const struct stream_fetch *fetch = context;
	UINT index = fetch->indices ? fetch->indices[vertex] : vertex;
	const struct fetch_element *element = fetch->elements, *end = element + fetch->element_count;

	for (; element < end; element++)
	{
		const unsigned char *source = element->base + index * element->stride;
		float *out = inputs[element->reg];
		int component;

		switch (element->kind)
		{
		case FETCH_FLOAT1:
			word_copy(&out[0], source);
			out[1] = out[2] = 0.0f;
			out[3] = 1.0f;
			break;
		case FETCH_FLOAT2:
			word_copy(&out[0], source);
			word_copy(&out[1], source + 4);
			out[2] = 0.0f;
			out[3] = 1.0f;
			break;
		case FETCH_FLOAT3:
			word_copy(&out[0], source);
			word_copy(&out[1], source + 4);
			word_copy(&out[2], source + 8);
			out[3] = 1.0f;
			break;
		case FETCH_FLOAT4:
			word_copy(&out[0], source);
			word_copy(&out[1], source + 4);
			word_copy(&out[2], source + 8);
			word_copy(&out[3], source + 12);
			break;
		case FETCH_FLOAT2H:
			word_copy(&out[0], source);
			word_copy(&out[1], source + 4);
			out[2] = 0.0f;
			word_copy(&out[3], source + 8);
			break;
		case FETCH_COLOR:
		{
			u32 word;

			memcpy(&word, source, sizeof(word));
			out[0] = byte_units[(word >> 16) & 0xff];
			out[1] = byte_units[(word >> 8) & 0xff];
			out[2] = byte_units[word & 0xff];
			out[3] = byte_units[word >> 24];
			break;
		}
		case FETCH_SHORTS:
			out[0] = out[1] = out[2] = 0.0f;
			out[3] = 1.0f;
			for (component = 0; component < element->count; component++)
				out[component] = (float)short_at(source + 2 * component);
			break;
		case FETCH_NORMSHORT1:
			out[0] = normalized(short_at(source), 32767.0f);
			out[1] = out[2] = 0.0f;
			out[3] = 1.0f;
			break;
		case FETCH_NORMSHORT2:
			out[0] = normalized(short_at(source), 32767.0f);
			out[1] = normalized(short_at(source + 2), 32767.0f);
			out[2] = 0.0f;
			out[3] = 1.0f;
			break;
		case FETCH_NORMSHORTS:
			out[3] = 1.0f;
			for (component = 0; component < element->count; component++)
				out[component] = normalized(short_at(source + 2 * component), 32767.0f);
			break;
		case FETCH_NORMPACKED3:
		{
			u32 word;

			memcpy(&word, source, sizeof(word));
			out[0] = packed_elevens[word & 0x7FF];
			out[1] = packed_elevens[(word >> 11) & 0x7FF];
			out[2] = packed_tens[word >> 22];
			out[3] = 1.0f;
			break;
		}
		default:
			out[0] = out[1] = out[2] = 0.0f;
			out[3] = 1.0f;
			for (component = 0; component < element->count; component++)
				out[component] = byte_units[source[component]];
			break;
		}
	}
}

/* ---------- draws in hardware

A program whose outputs are affine functions of its inputs (nv2a_cpu_analyze)
is, for one draw's constants, a matrix: GX's own transform computes it from
the game's vertex buffers, which it reads by index, and the processor runs
the program only for the matrix's columns (the outputs for no input and for
each input component alone) and the projection's fit. What GX can take:
- the clip-space x, y and w from one input register of three floats (the
  position matrix: draw_run's (x w, y w, -w) for the same projection);
- each texture coordinate (s, t) from the position or from one register of
  two floats (a texture matrix on GX_TG_POS or a GX_TG_TEXn);
- the fog factor from the position, through a table GX's ramp clamps as
  fog_coordinate does (none or linear);
- constant colours (the channels' material colour). A colour from the
  vertices is Direct3D's ARGB, an order GX does not read: those draws, the
  skinned ones (a product of a weight and a matrix) and the lit ones stay on
  the processor (draw_run). */

static unsigned long hardware_draws, hardware_vertices, hardware_colors_run, hardware_keyed_draws,
	hardware_blended_draws;
/* draws whose sphere maps' coordinates the processor computes, and the
vertices it ran for them */
static unsigned long hardware_sphere_draws, hardware_sphere_lit_draws, hardware_spheres_run;

static int single_register(unsigned long mask)
{
	int index;

	mask &= 0xFFFF;
	if (!mask || (mask & (mask - 1)))
		return -1;
	for (index = 0; !(mask & (1UL << index)); index++)
		;
	return index;
}

static const struct vertex_element *element_of(const struct vertex_shader_object *shader, int reg)
{
	unsigned long index;

	for (index = 0; index < shader->element_count; index++)
		if ((shader->elements[index].reg & 15) == reg)
			return &shader->elements[index];
	return NULL;
}

/* a model's vertex's value (node_keys): its node, 0 to 255, where it hangs
off one (its weight 1 on the first, or 0: on the second), else 256 and its
blend's place among the draw's (two nodes and a weight, which make one
matrix), or -1 past BLEND_VALUES of them */
#define BLEND_VALUES 64

struct node_values
{
	unsigned long blends[BLEND_VALUES];
	unsigned blend_count, last;
};

static int vertex_value(struct node_values *values, const unsigned char *nodes, const unsigned char *weights,
	BOOL add)
{
	short weight = short_at(weights);
	unsigned long blend;
	unsigned which;

	if (weight == 32767)
		return nodes[0];
	if (weight == 0)
		return nodes[1];
	blend = (unsigned long)nodes[0] << 24 | (unsigned long)nodes[1] << 16 | (unsigned short)weight;
	/* (a strip's neighbours are often the last's blend) */
	if (values->last < values->blend_count && values->blends[values->last] == blend)
		return 256 + (int)values->last;
	for (which = 0; which < values->blend_count; which++)
		if (values->blends[which] == blend)
			break;
	if (which == values->blend_count)
	{
		if (!add || values->blend_count == BLEND_VALUES)
			return -1;
		values->blends[values->blend_count++] = blend;
	}
	values->last = which;
	return 256 + (int)which;
}

/* a model's vertices keyed by their values (draw_keys' comment): those of the
most vertices take the matrices, every one where they all fit, else each
that has NODE_KEY_LEAST of the draw's indices; the other vertices are
KEY_PROCESSOR. FALSE where no value takes one. */
static BOOL node_keys_find(const struct stream_fetch *fetch, UINT count, const unsigned char *nodes,
	const unsigned char *weights, UINT node_stride, UINT weight_stride)
{
	UINT indices_of[256 + BLEND_VALUES], untracked = 0, vertex, least;
	unsigned char key_of[256 + BLEND_VALUES];
	unsigned short seen[256 + BLEND_VALUES];
	unsigned seen_count = 0, which;
	struct node_values values;
	/* (each vertex's value, from the first pass to the second; in the heap,
	as long as the longest draw yet) */
	static short *values_of;
	static UINT values_room;

	if (count > values_room)
	{
		UINT room = (count + 1023) & ~1023u;
		short *grown = realloc(values_of, room * sizeof(*values_of));

		if (!grown)
			return FALSE;
		values_of = grown;
		values_room = room;
	}
	memset(indices_of, 0, sizeof(indices_of));
	values.blend_count = values.last = 0;
	for (vertex = 0; vertex < count; vertex++)
	{
		unsigned long whole = fetch->indices ? (unsigned long)fetch->indices[vertex] + device.base_vertex_index :
			fetch->first + vertex;
		int value = vertex_value(&values, nodes + whole * node_stride, weights + whole * weight_stride, TRUE);

		values_of[vertex] = (short)value;
		if (value < 0)
			untracked++;
		else if (!indices_of[value]++)
			seen[seen_count++] = (unsigned short)value;
	}
	least = !untracked && seen_count <= MATRIX_KEYS ? 1 : NODE_KEY_LEAST;
	memset(key_of, KEY_PROCESSOR, sizeof(key_of));
	draw_keys.count = 0;
	while (draw_keys.count < MATRIX_KEYS)
	{
		unsigned best = 0;
		BOOL found = FALSE;

		for (which = 0; which < seen_count; which++)
		{
			unsigned value = seen[which];

			if (key_of[value] == KEY_PROCESSOR && indices_of[value] >= least &&
				(!found || indices_of[value] > indices_of[best]))
			{
				best = value;
				found = TRUE;
			}
		}
		if (!found)
			break;
		draw_keys.representative[draw_keys.count] = count;
		key_of[best] = (unsigned char)draw_keys.count++;
	}
	if (!draw_keys.count)
		return FALSE;
	draw_keys.processor = 0;
	for (vertex = 0; vertex < count; vertex++)
	{
		/* (as the first pass found it: a blend it could not add is untracked
		in both) */
		int value = values_of[vertex];
		unsigned char key = value < 0 ? KEY_PROCESSOR : key_of[value];

		vertex_keys[vertex] = key;
		if (key == KEY_PROCESSOR)
			draw_keys.processor++;
		else if (draw_keys.representative[key] == count)
			draw_keys.representative[key] = vertex;
	}
	return TRUE;
}

/* ---------- what a draw's indices make, kept

Every indexed draw in hardware finds its indices' range (the arrays it
flushes for GX, GX's 16-bit indices), and a model's node_keys_find two
passes over its indices: each vertex's value from its node indices and
weight, and the values that take the draw's matrices (some 50 instructions
an index). The structure's sections are drawn with the same indices each
frame (written again into the game's dynamic triangles), and a model's part
with the same indices and vertices in each of its passes and frames. So
what they make is kept, by the indices' address and count and the base
vertex, with a hash of the indices (two lanes of 32-bit FNV-1, over their
words in turn): the range, and for a model's part, its keys, with a copy of the
node indices and weight of each vertex in the range. A later draw whose
indices hash the same, and whose vertices' node indices and weights are
the copy's, takes them as they were: about two instructions an index and
two a vertex. A buffer the game rewrites only fails the comparison. */
#define INDEX_RECORDS 256
#define INDEX_RECORD_BYTES (256UL * 1024UL)
#define INDEX_LISTS 4

struct index_record
{
	const WORD *indices;
	UINT count, base;
	unsigned long long hash;
	/* the index range, and the frame the record was last used in */
	unsigned long least, most, used;
	/* node_keys_find's result for these node indices' and weights' elements
	(where block is not NULL), and draw_keys */
	const unsigned char *nodes, *weights;
	UINT node_stride, weight_stride;
	BOOL found;
	unsigned key_count;
	UINT processor, representative[MATRIX_KEYS];
	/* the static buffers' kept outputs (static_outputs_find) whose every
	vertex these indices name is computed: the entry's generation and values */
	unsigned long kept_generation;
	const unsigned char *kept_values;
	/* in one block: each vertex's node indices and weight (two shorts), then
	each index's key */
	unsigned char *block;
	unsigned long bytes;
	/* the indices as GX display lists, one for each primitive and number
	of indexed attributes a draw of them sent (index_list_draw) */
	struct index_list
	{
		void *data;
		u32 size, capacity;
		u8 primitive, attributes;
	} lists[INDEX_LISTS];
	/* (the kept_room a list was refused in: index_list_draw) */
	unsigned long lists_refused;
};

static struct index_record index_records[INDEX_RECORDS];
static unsigned long index_record_bytes;
/* in the statistics' window: draws whose range, and models' draws whose
keys, were taken as kept, and those that found them */
static unsigned long ranges_kept, ranges_found, key_records_kept, key_records_made;

/* the index range of the draw being made, and its record (if it has one) */
static struct
{
	unsigned long serial, least, most;
	struct index_record *record;
	/* (the record's indices hashed the same in an earlier draw) */
	BOOL kept;
} draw_range;

static void index_record_lists_release(struct index_record *record, BOOL drawn);

static unsigned short *index_record_vertices(const struct index_record *record)
{
	return (unsigned short *)record->block;
}

static unsigned char *index_record_keys(const struct index_record *record)
{
	return record->block + 4 * (record->most - record->least + 1);
}

static void index_record_keys_free(struct index_record *record)
{
	free(record->block);
	index_record_bytes -= record->bytes;
	record->block = NULL;
	record->bytes = 0;
}

/* the indices' hash: two lanes, each a word's of two (an index alone first
where they are not aligned to a word, and those left after the last two
words, each lane's in turn) */
static unsigned long long indices_hash(const WORD *indices, UINT count)
{
	u32 a = 2166136261U ^ count, b = 0x9E3779B9U;
	const WORD *end = indices + count;

	if (((unsigned long)indices & 3) && indices < end)
		a = (a ^ *indices++) * 16777619U;
	for (; indices + 3 < end; indices += 4)
	{
		a = (a ^ ((const u32 *)indices)[0]) * 16777619U;
		b = (b ^ ((const u32 *)indices)[1]) * 2246822519U;
	}
	for (; indices < end; indices++)
		b = (b ^ *indices) * 2246822519U;
	return (unsigned long long)a << 32 | b;
}

/* the draw's index range, from its record or found (and recorded): once a
draw, in draw_range */
static void draw_range_find(const struct stream_fetch *fetch, UINT count)
{
	struct index_record *record = NULL;
	unsigned long long hash;
	unsigned smallest = 0xFFFF, largest = 0, set, way;
	UINT vertex;

	if (draw_range.serial == draw_serial)
		return;
	draw_range.serial = draw_serial;
	draw_range.record = NULL;
	draw_range.kept = FALSE;
	if (!fetch->indices)
	{
		draw_range.least = fetch->first;
		draw_range.most = fetch->first + count - 1;
		return;
	}
	hash = indices_hash(fetch->indices, count);
	/* (four ways for each place, by the indices' address and count) */
	set = (unsigned)(((((unsigned long)fetch->indices >> 1) + count) * 2654435761UL) >> 16) % (INDEX_RECORDS / 4) * 4;
	for (way = 0; way < 4; way++)
	{
		struct index_record *candidate = &index_records[set + way];

		if (candidate->indices == fetch->indices && candidate->count == count &&
			candidate->base == device.base_vertex_index)
		{
			record = candidate;
			break;
		}
		if (!record || (record->indices && (!candidate->indices || candidate->used < record->used)))
			record = candidate;
	}
	if (way < 4 && record->hash == hash)
	{
		record->used = device.frames;
		draw_range.least = record->least;
		draw_range.most = record->most;
		draw_range.record = record;
		draw_range.kept = TRUE;
		ranges_kept++;
		return;
	}
	for (vertex = 0; vertex < count; vertex++)
	{
		unsigned value = fetch->indices[vertex];

		smallest = value < smallest ? value : smallest;
		largest = value > largest ? value : largest;
	}
	draw_range.least = smallest + (unsigned long)device.base_vertex_index;
	draw_range.most = largest + (unsigned long)device.base_vertex_index;
	ranges_found++;
	/* (a place used this frame or the last is kept: a structure of more
	draws than the records goes without, rather than every draw losing its
	record each frame) */
	if (record->indices && record->indices != fetch->indices && record->used + 1 >= device.frames)
		return;
	if (record->block)
		index_record_keys_free(record);
	index_record_lists_release(record, FALSE);
	record->lists_refused = 0;
	record->indices = fetch->indices;
	record->count = count;
	record->base = device.base_vertex_index;
	record->hash = hash;
	record->kept_generation = 0;
	record->kept_values = NULL;
	record->least = draw_range.least;
	record->most = draw_range.most;
	record->used = device.frames;
	draw_range.record = record;
}

/* whether the record's node keys were found of these vertices' node indices
and weights (and are still theirs) */
static BOOL index_record_keys_equal(const struct index_record *record, const unsigned char *nodes,
	const unsigned char *weights, UINT node_stride, UINT weight_stride)
{
	const unsigned short *kept = index_record_vertices(record);
	unsigned long vertex, vertices = record->most - record->least + 1;

	if (!record->block || record->nodes != nodes || record->weights != weights || record->node_stride != node_stride ||
		record->weight_stride != weight_stride)
		return FALSE;
	nodes += record->least * node_stride;
	weights += record->least * weight_stride;
	/* (a word at a time where the weight follows the node indices, aligned,
	as in the maps) */
	if (weights == nodes + 2 && weight_stride == node_stride && !((unsigned long)nodes & 3) && !(node_stride & 3))
	{
		const u32 *pair = (const u32 *)kept, *now = (const u32 *)nodes;
		UINT stride = node_stride / 4;

		for (vertex = 0; vertex < vertices; vertex++, now += stride)
			if (*now != pair[vertex])
				return FALSE;
		return TRUE;
	}
	for (vertex = 0; vertex < vertices; vertex++)
	{
		unsigned short node, weight;

		memcpy(&node, nodes, sizeof(node));
		memcpy(&weight, weights, sizeof(weight));
		if (node != kept[0] || weight != kept[1])
			return FALSE;
		kept += 2;
		nodes += node_stride;
		weights += weight_stride;
	}
	return TRUE;
}

/* the record's node keys: what node_keys_find just found (in vertex_keys and
draw_keys), with a copy of each vertex's node indices and weight */
static void index_record_keys_make(struct index_record *record, UINT count, const unsigned char *nodes,
	const unsigned char *weights, UINT node_stride, UINT weight_stride, BOOL found)
{
	unsigned long vertices = record->most - record->least + 1, bytes = vertices * 4 + count, vertex;
	unsigned short *kept;

	if (record->block)
		index_record_keys_free(record);
	if (bytes > INDEX_RECORD_BYTES / 8)
		return;
	/* (room: the least recently used records' keys go, but those of this
	frame's and the last's draws) */
	while (index_record_bytes + bytes > INDEX_RECORD_BYTES)
	{
		struct index_record *oldest = NULL;
		unsigned index;

		for (index = 0; index < INDEX_RECORDS; index++)
			if (index_records[index].block && (!oldest || index_records[index].used < oldest->used))
				oldest = &index_records[index];
		if (!oldest || oldest->used + 1 >= device.frames)
			return;
		index_record_keys_free(oldest);
	}
	record->block = malloc(bytes);
	if (!record->block)
		return;
	record->bytes = bytes;
	index_record_bytes += bytes;
	record->nodes = nodes;
	record->weights = weights;
	record->node_stride = node_stride;
	record->weight_stride = weight_stride;
	record->found = found;
	record->key_count = draw_keys.count;
	record->processor = draw_keys.processor;
	memcpy(record->representative, draw_keys.representative, sizeof(record->representative));
	kept = index_record_vertices(record);
	for (vertex = 0; vertex < vertices; vertex++)
	{
		memcpy(&kept[2 * vertex], nodes + (record->least + vertex) * node_stride, sizeof(unsigned short));
		memcpy(&kept[2 * vertex + 1], weights + (record->least + vertex) * weight_stride, sizeof(unsigned short));
	}
	memcpy(index_record_keys(record), vertex_keys, count);
	key_records_made++;
}

/* node_keys_find's keys for the draw, as its indices' record has them, or
else found and recorded */
static BOOL node_keys(const struct vertex_shader_object *shader, const struct stream_fetch *fetch, UINT count)
{
	const struct vertex_element *node_element = element_of(shader, NODE_INDICES);
	const struct vertex_element *weight_element = element_of(shader, NODE_WEIGHT);
	unsigned node_stream = node_element->stream & (MAXIMUM_STREAMS - 1);
	unsigned weight_stream = weight_element->stream & (MAXIMUM_STREAMS - 1);
	const unsigned char *nodes = fetch->streams[node_stream] + node_element->offset;
	const unsigned char *weights = fetch->streams[weight_stream] + weight_element->offset;
	UINT node_stride = device.streams[node_stream].stride, weight_stride = device.streams[weight_stream].stride;
	struct index_record *record;
	BOOL found;

	if (count > sizeof(vertex_keys))
		return FALSE;
	draw_range_find(fetch, count);
	record = draw_range.record;
	if (record && index_record_keys_equal(record, nodes, weights, node_stride, weight_stride))
	{
		draw_keys.count = record->key_count;
		draw_keys.processor = record->processor;
		memcpy(draw_keys.representative, record->representative, sizeof(draw_keys.representative));
		draw_key_of = index_record_keys(record);
		key_records_kept++;
		return record->found;
	}
	found = node_keys_find(fetch, count, nodes, weights, node_stride, weight_stride);
	if (record)
		index_record_keys_make(record, count, nodes, weights, node_stride, weight_stride, found);
	return found;
}

/* what a draw's program depends on: its analysis, or, where its clip-space
position is not affine but the inputs it is not affine in (other than a
position of three floats) hold the same values in every vertex the draw
names (a skinned model's part bound to one node: its node indices and
weights), its analysis with those as constants, whose values draw_uniform
keeps for the matrices; where they hold a few values, draw_keys keeps them
(once a draw: draw_hardware's, then draw_run's if it is the processor's) */
static const struct nv2a_cpu_dependencies *draw_dependencies(struct vertex_shader_object *shader,
	struct nv2a_cpu_program *program, struct stream_fetch *fetch, UINT count)
{
	static unsigned long serial;
	static const struct nv2a_cpu_dependencies *found;
	const struct nv2a_cpu_dependencies *general = &shader->dependencies;
	unsigned long clip = general->clip[0] | general->clip[1] | general->clip[3], candidates = 0;
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	unsigned index;
	int reg;

	if (fetch && serial == draw_serial && found)
		return found;
	draw_uniform.mask = 0;
	draw_keys.count = 0;
	draw_keys.processor = 0;
	draw_key_of = vertex_keys;
	found = NULL;
	serial = draw_serial;
	if (!fetch || !count || !general->clip_captured || !(clip & NV2A_CPU_NOT_AFFINE))
		return found = general;
	for (reg = 0; reg < NV2A_CPU_INPUT_COUNT; reg++)
	{
		const struct vertex_element *element;

		if (!(clip & (1UL << reg)))
			continue;
		element = element_of(shader, reg);
		if (!element || !fetch->streams[element->stream & (MAXIMUM_STREAMS - 1)] || !element->bytes)
			return found = general;
		if (element->type != D3DVSDT_FLOAT3)
			candidates |= 1UL << reg;
	}
	if (!candidates)
		return found = general;
	if (candidates == (1UL << NODE_INDICES | 1UL << NODE_WEIGHT) && program->native &&
		element_of(shader, NODE_INDICES)->type == D3DVSDT_PBYTE2 &&
		element_of(shader, NODE_WEIGHT)->type == D3DVSDT_NORMSHORT1)
	{
		if (!node_keys(shader, fetch, count))
		{
			draw_keys.count = 0;
			return found = general;
		}
	}
	else
	{
		/* the candidates' bytes in each vertex, grouped by value */
		const unsigned char *streams[NV2A_CPU_INPUT_COUNT];
		UINT strides[NV2A_CPU_INPUT_COUNT], bytes[NV2A_CPU_INPUT_COUNT], vertex, key;
		unsigned long wholes[MATRIX_KEYS];
		int regs[NV2A_CPU_INPUT_COUNT], reg_count = 0, which;

		for (reg = 0; reg < NV2A_CPU_INPUT_COUNT; reg++)
		{
			const struct vertex_element *element;

			if (!(candidates & (1UL << reg)))
				continue;
			element = element_of(shader, reg);
			streams[reg_count] = fetch->streams[element->stream & (MAXIMUM_STREAMS - 1)] + element->offset;
			strides[reg_count] = device.streams[element->stream & (MAXIMUM_STREAMS - 1)].stride;
			bytes[reg_count] = element->bytes;
			regs[reg_count++] = reg;
		}
		draw_keys.count = 0;
		for (vertex = 0; vertex < count; vertex++)
		{
			unsigned long whole = fetch->indices ? (unsigned long)fetch->indices[vertex] + device.base_vertex_index :
				fetch->first + vertex;

			for (key = 0; key < draw_keys.count; key++)
			{
				for (which = 0; which < reg_count; which++)
					if (memcmp(streams[which] + whole * strides[which], streams[which] + wholes[key] * strides[which],
						bytes[which]))
						break;
				if (which == reg_count)
					break;
			}
			if (key == draw_keys.count)
			{
				if (draw_keys.count == MATRIX_KEYS || count > sizeof(vertex_keys))
				{
					draw_keys.count = 0;
					return found = general;
				}
				wholes[key] = whole;
				draw_keys.representative[key] = vertex;
				draw_keys.count++;
			}
			vertex_keys[vertex] = (unsigned char)key;
		}
		(void)regs;
	}
	stream_fetch(fetch, draw_keys.representative[0], inputs);
	for (reg = 0; reg < NV2A_CPU_INPUT_COUNT; reg++)
		if (candidates & (1UL << reg))
			memcpy(draw_uniform.values[reg], inputs[reg], sizeof(inputs[reg]));
	draw_uniform.mask = candidates;
	for (index = 0; index < shader->specialized_count; index++)
		if (shader->specialized[index].uniform == candidates)
			return found = &shader->specialized[index].dependencies;
	index = shader->specialized_count < 2 ? shader->specialized_count++ : 1;
	shader->specialized[index].uniform = candidates;
	nv2a_cpu_analyze_uniform(program, candidates, &shader->specialized[index].dependencies);
	return found = &shader->specialized[index].dependencies;
}

/* the matrices of recent draws, by what made them: the program, its
vertex declaration, the constants it reads, the texture coordinates' scales
and the stages that read them, the fog's table */
#define HARDWARE_CACHE_ENTRIES 128
struct hardware_matrices
{
	unsigned long long key;
	Mtx position;
	float stage_rows[5][2][4];
	float d0[4], d1[4];
	struct draw_projection projection;
};
static struct hardware_matrices hardware_cache[HARDWARE_CACHE_ENTRIES];
static unsigned long hardware_cache_hits;

static unsigned long long hardware_key(const struct vertex_shader_object *shader, const struct nv2a_cpu_program *program)
{
	/* (the program, its declaration and the constants: constants_hash) */
	unsigned long long hash = constants_hash(shader, program);
	unsigned long state[4], row;

	state[0] = tev_texcoords | sphere_stages << 8;
	state[1] = D3D__RenderState[D3DRS_FOGTABLEMODE];
	state[2] = D3D__RenderState[D3DRS_FOGSTART];
	state[3] = D3D__RenderState[D3DRS_FOGEND];
	hash = hash_words(hash, state, sizeof(state));
	hash = hash_words(hash, texcoord_scale, sizeof(texcoord_scale));
	hash = hash_words(hash, &draw_uniform.mask, sizeof(draw_uniform.mask));
	for (row = 0; row < NV2A_CPU_INPUT_COUNT; row++)
		if (draw_uniform.mask & (1UL << row))
			hash = hash_words(hash, draw_uniform.values[row], sizeof(draw_uniform.values[row]));
	hash = hash_words(hash, &draw_unstreamed, sizeof(draw_unstreamed));
	for (row = 0; row < NV2A_CPU_INPUT_COUNT; row++)
		if (draw_unstreamed & (1UL << row))
			hash = hash_words(hash, device.vertex_registers[row], sizeof(device.vertex_registers[row]));
	/* (0 is an empty entry's) */
	return hash ? hash : 1;
}

/* ---------- the models' lighting on GX's lights

A lit model's colours (its normal's lighting) and its reflection's sphere
map coordinates are the processor's, vertex by vertex, on the path above:
the lighting, the skinning of the normal, the reflection and the sphere
map's square roots are most of the program. With display.gx_lighting, the
programs gx_lighting.c has a recipe for (the models' lit permutations) are
lit by GX's lights, and their reflection read through GX's texture
coordinates from the normal (gx_lighting.c says how, and how close it is):
- the position matrices make E (gx_lighting_space: the device's (x w, y w,
  -w) is K E, K then in the projection), and their normal matrices E's
  turn, as GX lights in the space its position matrices make;
- the normal is sent with each vertex, three floats from the map's 11:11:10
  word (as the programs read it), or the processor's skinned one for a
  vertex whose position is the processor's (it goes through GX_PNMTX0,
  then K's inverse, the normal through the world's turn to E);
- COLOR0 is D0's colour, COLOR1A1 D1 (the Fresnel colour); D0's alpha is 0,
  or the planar fog's: read from a ramp at texture coordinate 5, which GX
  makes of the position through its matrix to E and a post-transform matrix
  (lit_fog_ramp, gx_lighting.h), the pixel shader translated to read it in
  place of the rasterized alpha; else (the product with the atmospheric fog,
  no texture map free, a pixel shader beyond GX with it) the processor's (a
  world position's, by each node's matrix), sent as the vertex's colour;
- texture coordinate 3 (the reflection's cube map) is made of the normal by
  a matrix for each node (lit_sphere_rows, given with each vertex as its
  position matrix is), into a sphere map of the view: drawn from the
  world's in the EFB's rows below the back buffer (sphere_bake), for each
  cube map and direction of a draw from the eye (rounded to
  LIT_SPHERE_STEP in the world: gx_lighting_view_frame), and kept while
  the draws' directions stay in their steps, however the camera turns. */

/* (config.toml's display.gx_lighting; -1 not read yet) */
static int gx_lighting_on = -1;
/* the view frames' rounding (tests/gx_lighting_check.c's REFLECTION_STEP) */
#define LIT_SPHERE_STEP (15.0f * 3.14159265f / 180.0f)
/* the texture matrices of a draw in hardware's stages (and the fog's, the
fifth), two rows each from GX_TEXMTX0's (rows 30 to 39: GX takes a matrix
by its first row), and of the normal's sphere map coordinates: the keys' in
order, then the processor's vertices', in the pairs after them, short of
GX_IDENTITY (row 60), the identity every draw in screen space reads its
texture coordinates through: a matrix over it draws them all wrong. Ten:
a model's nine keys and its blended vertices. */
#define STAGE_TEXMTX(stage) ((u32)(GX_TEXMTX0 + 2 * (stage)))
static const u8 lit_sphere_rows[] = { 40, 42, 44, 46, 48, 50, 52, 54, 56, 58 };
#define LIT_SPHERE_MATRICES (sizeof(lit_sphere_rows) / sizeof(lit_sphere_rows[0]))
/* (the first past the stages' pairs, the last short of GX_IDENTITY; room for
every key) */
typedef char lit_sphere_rows_short_of_identity[40 >= GX_TEXMTX0 + 2 * 5 && 58 + 2 <= GX_IDENTITY &&
	LIT_SPHERE_MATRICES >= MATRIX_KEYS + 1 ? 1 : -1];
/* the view's sphere maps kept (a buffer can be drawn again in the frame:
GX copies into it after the draws before it have read it), the most drawn
in a frame, and how far a view's frame may turn and still keep one (in its
rows' components: about a tenth of a degree). The frames are rounded in the
world (gx_lighting_view_frame), so a map is kept while the camera turns;
there are as many as a frame's draws of different cube maps and directions
from the eye use (b30's Pelican, its Marines about the camera: some 40,
more than the 32 there were, so that every map was drawn again in every
frame), in the contiguous window (9 KB each). A draw past the frame's
maps takes the nearest kept of its cube map (sphere_bake). */
#define SPHERE_BAKES 64
#define SPHERE_BAKES_A_FRAME 24
#define SPHERE_BAKE_KEPT 2.0e-3f
/* the first row of the EFB below the back buffer, where they are drawn */
#define SPHERE_BAKE_ROW SCREEN_HEIGHT

static struct
{
	const struct gx_lighting_recipe *recipe;
	/* what GX makes: the colours, the reflection */
	BOOL colors, reflection;
	/* D0's alpha the processor's (the fog), or GX's ramp's (lit_fog_ramp) */
	BOOL alpha, fog_ramped;
	struct gx_lighting_space space;
	struct gx_lighting lighting;
	/* each key's rows in E, and in the world (for the fog) */
	Mtx rows[MATRIX_KEYS];
	float world[MATRIX_KEYS][3][4];
	/* the view's frame, E's turn to it */
	float frame[3][3], turn[3][3];
	GXTexObj sphere;
	/* the normal's element */
	const unsigned char *normals;
	UINT normal_stride;
	BOOL normal_packed;
	/* where GX reads the normals by index (kept_normals_find), if it does */
	BOOL normals_indexed;
	const unsigned char *normals_array;
	UINT normals_array_stride;
	/* a point of the draw in E */
	float center[3];
} lit;

/* in the statistics' window: draws lit, their vertices, the draws refused
(no space, lights beyond GX's), sphere maps drawn */
static unsigned long lit_draws, lit_vertices, lit_refused, sphere_bakes_made, sphere_bakes_borrowed;

static struct
{
	const void *texels;
	float frame[3][3], scale;
	unsigned long frame_number;
	void *buffer;
} sphere_bakes[SPHERE_BAKES];
static unsigned sphere_bakes_next;
static unsigned long sphere_bakes_frame, sphere_bakes_in_frame;

/* the view's sphere map of the world's (bound to texture map 3 by the
draw: textures_bind) for the frame, drawn now if not yet this frame. GX's
state the draw set (draw_prepare's) is set again after. */
static BOOL sphere_bake(const struct texture_entry *world, const float frame[3][3], float scale, GXTexObj *out)
{
	enum { GRID = 24, SIZE = GX_LIGHTING_SPHERE_SIZE };
	unsigned which, row, column;
	Mtx44 matrix;
	static float coordinates[GRID + 1][GRID + 1][2], points[GRID + 1][GRID + 1][3];
	static BOOL points_made;
	void *buffer = NULL;

	if (sphere_bakes_frame != device.frames)
	{
		sphere_bakes_frame = device.frames;
		sphere_bakes_in_frame = 0;
	}
	for (which = 0; which < SPHERE_BAKES && !buffer; which++)
	{
		unsigned row, column;
		BOOL near = sphere_bakes[which].buffer && sphere_bakes[which].texels == world->texels &&
			sphere_bakes[which].scale == scale;

		for (row = 0; row < 3 && near; row++)
			for (column = 0; column < 3; column++)
				if (fabsf(sphere_bakes[which].frame[row][column] - frame[row][column]) > SPHERE_BAKE_KEPT)
					near = FALSE;
		if (near)
		{
			buffer = sphere_bakes[which].buffer;
			sphere_bakes[which].frame_number = device.frames;
		}
	}
	if (!buffer && sphere_bakes_in_frame >= SPHERE_BAKES_A_FRAME)
	{
		/* (past the frame's: the kept map of this cube map whose frame's z is
		nearest the draw's; a later frame draws the draw's own) */
		float best = -2.0f;

		for (which = 0; which < SPHERE_BAKES; which++)
		{
			float along;

			if (!sphere_bakes[which].buffer || sphere_bakes[which].texels != world->texels ||
				sphere_bakes[which].scale != scale)
				continue;
			along = sphere_bakes[which].frame[2][0] * frame[2][0] + sphere_bakes[which].frame[2][1] * frame[2][1] +
				sphere_bakes[which].frame[2][2] * frame[2][2];
			if (along > best)
			{
				best = along;
				buffer = sphere_bakes[which].buffer;
			}
		}
		if (!buffer)
			return FALSE;
		sphere_bakes_borrowed++;
	}
	if (!buffer)
	{
		/* (an empty one, else the one drawn longest ago) */
		which = sphere_bakes_next++ % SPHERE_BAKES;
		for (row = 0; row < SPHERE_BAKES; row++)
		{
			if (!sphere_bakes[row].buffer)
			{
				which = row;
				break;
			}
			if (sphere_bakes[row].frame_number < sphere_bakes[which].frame_number)
				which = row;
		}
		if (!sphere_bakes[which].buffer)
		{
			sphere_bakes[which].buffer = platform_contiguous_alloc(SIZE * SIZE * 4, 32, PLATFORM_ANY_PHYSICAL_ADDRESS, 0);
			if (!sphere_bakes[which].buffer)
				return FALSE;
		}
		buffer = sphere_bakes[which].buffer;
		sphere_bakes[which].texels = world->texels;
		memcpy(sphere_bakes[which].frame, frame, sizeof(sphere_bakes[which].frame));
		sphere_bakes[which].scale = scale;
		sphere_bakes[which].frame_number = device.frames;
		sphere_bakes_in_frame++;
		sphere_bakes_made++;

		/* each grid point's place on the world's sphere map (its point on the
		sphere found once) */
		if (!points_made)
		{
			for (row = 0; row <= GRID; row++)
				for (column = 0; column <= GRID; column++)
					gx_lighting_sphere_point((float)column / GRID, (float)row / GRID, points[row][column]);
			points_made = TRUE;
		}
		for (row = 0; row <= GRID; row++)
			for (column = 0; column <= GRID; column++)
				gx_lighting_sphere_point_texel(frame, scale, points[row][column], coordinates[row][column]);
		/* the grid over the rows below the back buffer, the world's sphere map
		(texture map 3) on it */
		GX_SetViewport(0.0f, (f32)SPHERE_BAKE_ROW, SIZE, SIZE, 0.0f, 1.0f);
		GX_SetScissor(0, SPHERE_BAKE_ROW, SIZE, SIZE);
		memset(matrix, 0, sizeof(matrix));
		matrix[0][0] = 2.0f;
		matrix[0][3] = -1.0f;
		matrix[1][1] = -2.0f;
		matrix[1][3] = 1.0f;
		/* (depth -1/2, inside GX's clip-space -1 to 0) */
		matrix[2][3] = -0.5f;
		matrix[3][3] = 1.0f;
		GX_LoadProjectionMtx(matrix, GX_ORTHOGRAPHIC);
		projection_valid = FALSE;
		GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
		GX_SetColorUpdate(GX_TRUE);
		GX_SetAlphaUpdate(GX_TRUE);
		GX_SetCullMode(GX_CULL_NONE);
		states_shadow_reset();
		indirect_apply(NULL);
		GX_SetNumTevStages(1);
		GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP3, GX_COLORNULL);
		GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
		/* (x and y over 0 to 1: the projection's) */
		GX_ClearVtxDesc();
		GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		vertex_format(SPHERE_BAKE_VERTEX_FORMAT, GX_VA_POS, GX_POS_XY, GX_F32, 0);
		vertex_format(SPHERE_BAKE_VERTEX_FORMAT, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
		GX_SetCurrentMtx(GX_PNMTX0);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
		transform_hardware = TRUE;
		for (row = 0; row < GRID; row++)
		{
			gx_begin(GX_TRIANGLESTRIP, SPHERE_BAKE_VERTEX_FORMAT, 2 * (GRID + 1));
			for (column = 0; column <= GRID; column++)
			{
				GX_Position2f32((f32)column / GRID, (f32)row / GRID);
				GX_TexCoord2f32(coordinates[row][column][0], coordinates[row][column][1]);
				GX_Position2f32((f32)column / GRID, (f32)(row + 1) / GRID);
				GX_TexCoord2f32(coordinates[row + 1][column][0], coordinates[row + 1][column][1]);
			}
			GX_End();
		}
		/* (copied unfiltered, and GX's texture cache invalidated) */
		wl_gx_copy_efb_texture(0, SPHERE_BAKE_ROW, SIZE, SIZE, buffer, WL_GX_TEX_RGBA8, false);
		xf_flush_after();
		/* the draw's own state again */
		GX_SetViewport(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 1.0f);
		GX_SetScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
		states_apply();
		gx_psh_apply(pixel_shader_program);
		indirect_apply(pixel_shader_program);
	}
	GX_InitTexObj(out, buffer, SIZE, SIZE, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(out, GX_LINEAR, GX_LINEAR);
	return TRUE;
}

/* the world's rows of E's (the inverse of the world's to E, a turn and a
scale: its transpose over sigma squared) */
static void lit_world_rows(const Mtx rows, float out[3][4])
{
	const struct gx_lighting_space *space = &lit.space;
	float scale = 1.0f / (space->sigma * space->sigma);
	int row, column, k;

	for (row = 0; row < 3; row++)
		for (column = 0; column < 4; column++)
		{
			float value = 0.0f;

			for (k = 0; k < 3; k++)
				value += space->world_to_eye[k][row] *
					(rows[k][column] - (column == 3 ? space->world_to_eye[k][3] : 0.0f));
			out[row][column] = value * scale;
		}
}

/* the normal of a vertex of the draw's buffer, as the program reads it */
static inline void lit_normal(UINT whole, float out[3])
{
	const unsigned char *source = lit.normals + whole * lit.normal_stride;

	if (lit.normal_packed)
	{
		u32 word;

		memcpy(&word, source, sizeof(word));
		out[0] = packed_elevens[word & 0x7FF];
		out[1] = packed_elevens[(word >> 11) & 0x7FF];
		out[2] = packed_tens[word >> 22];
	}
	else
	{
		word_copy(&out[0], source);
		word_copy(&out[1], source + 4);
		word_copy(&out[2], source + 8);
	}
}

/* GX's lights and channels for the draw (lit.lighting) */
static void lit_lights_load(const float d1_constant[4])
{
	const struct gx_lighting *lighting = &lit.lighting;
	unsigned index;
	u8 masks[2] = { 0, 0 };

	for (index = 0; index < lighting->light_count; index++)
	{
		const struct gx_lighting_light *light = &lighting->lights[index];
		GXLightObj object;

		GX_InitLightPos(&object, light->position[0], light->position[1], light->position[2]);
		GX_InitLightDir(&object, light->direction[0], light->direction[1], light->direction[2]);
		GX_InitLightColor(&object, (GXColor){ light->color[0], light->color[1], light->color[2], light->color[3] });
		GX_InitLightAttn(&object, light->a[0], light->a[1], light->a[2], light->k[0], light->k[1], light->k[2]);
		GX_LoadLightObj(&object, (u8)(GX_LIGHT0 << index));
	}
	for (index = 0; index < 2; index++)
	{
		unsigned light;

		for (light = 0; light < lighting->light_count; light++)
			if (lighting->masks[index] & (1u << light))
				masks[index] |= (u8)(GX_LIGHT0 << light);
	}
	GX_SetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, masks[0], GX_DF_CLAMP, GX_AF_SPOT);
	GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){ lighting->ambient[0][0], lighting->ambient[0][1],
		lighting->ambient[0][2], 255 });
	/* (D0's alpha: 0, or the vertex's) */
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){ 255, 255, 255, 0 });
	GX_SetChanCtrl(GX_ALPHA0, GX_DISABLE, GX_SRC_REG, lit.alpha ? GX_SRC_VTX : GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE,
		GX_AF_NONE);
	if (lighting->fresnel)
	{
		GX_SetChanCtrl(GX_COLOR1A1, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, masks[1], GX_DF_SIGNED, GX_AF_SPOT);
		GX_SetChanAmbColor(GX_COLOR1A1, (GXColor){ lighting->ambient[1][0], lighting->ambient[1][1],
			lighting->ambient[1][2], lighting->ambient[1][3] });
		GX_SetChanMatColor(GX_COLOR1A1, (GXColor){ 255, 255, 255, 255 });
	}
	else
	{
		GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		GX_SetChanMatColor(GX_COLOR1A1, (GXColor){ color_byte(d1_constant[0]), color_byte(d1_constant[1]),
			color_byte(d1_constant[2]), color_byte(d1_constant[3]) });
	}
}

/* the texture matrix of the sphere map's coordinates of a normal through
rows (E's): the frame's x and y of it, made unit by the rows' scale, as 0
to 1 */
static void lit_sphere_matrix(const float rows[3][4], u32 id)
{
	Mtx matrix;
	float scale = 0.0f, length;
	int row, column, k;

	for (row = 0; row < 3; row++)
		scale += nv2a_sqrtf(rows[row][0] * rows[row][0] + rows[row][1] * rows[row][1] + rows[row][2] * rows[row][2]);
	length = scale > 1.0e-12f ? 3.0f / scale : 0.0f;
	memset(matrix, 0, sizeof(matrix));
	for (row = 0; row < 2; row++)
	{
		for (column = 0; column < 3; column++)
		{
			float value = 0.0f;

			for (k = 0; k < 3; k++)
				value += lit.turn[row][k] * rows[k][column];
			matrix[row][column] = 0.5f * length * value;
		}
		matrix[row][3] = 0.5f;
	}
	GX_LoadTexMtxImm(matrix, id, GX_MTX2x4);
}

/* whether the draw is lit on GX (lit), found before its matrices: its
program has a recipe and the setting is on, and what GX would make of it
the processor otherwise computes for each vertex */
static BOOL lit_choose(const struct nv2a_cpu_program *program, const struct vertex_shader_object *shader,
	const struct stream_fetch *fetch, BOOL cpu_colors, unsigned cpu_spheres)
{
	const struct vertex_element *normal;

	lit.recipe = NULL;
	lit.colors = lit.reflection = lit.alpha = lit.fog_ramped = FALSE;
	if (gx_lighting_on < 0)
		gx_lighting_on = config_boolean("display.gx_lighting") ? 1 : 0;
	if (!gx_lighting_on || !(cpu_colors || (cpu_spheres & 8)))
		return FALSE;
	lit.recipe = gx_lighting_recipe_find(program->hash);
	if (!lit.recipe)
		return FALSE;
	normal = element_of(shader, 1);
	if (!normal || (normal->type != D3DVSDT_NORMPACKED3 && normal->type != D3DVSDT_FLOAT3) ||
		!fetch->streams[normal->stream & (MAXIMUM_STREAMS - 1)])
		return FALSE;
	lit.normals = fetch->streams[normal->stream & (MAXIMUM_STREAMS - 1)] + normal->offset;
	lit.normal_stride = device.streams[normal->stream & (MAXIMUM_STREAMS - 1)].stride;
	lit.normal_packed = normal->type == D3DVSDT_NORMPACKED3;
	lit.colors = cpu_colors;
	/* (the reflection: the world's sphere map, of a pixel shader GX holds,
	drawn into the back buffer, below which its view's is drawn) */
	lit.reflection = (cpu_spheres & 8) && gx_lighting_recipe_reflects(lit.recipe) && bound_entries[3] &&
		bound_entries[3]->sphere && pixel_shader_usable && device.render_target == &device.back_buffer;
	return lit.colors || lit.reflection;
}

/* the draw's lights, matrices in E and view's sphere map, once its keys'
matrices are made (FALSE: the processor's after all) */
static BOOL lit_prepare(const struct vertex_shader_object *shader, struct stream_fetch *fetch, int position_register,
	BOOL keyed, UINT first_vertex)
{
	const float (*constants)[4] = (const float (*)[4])device.vertex_constants + VERTEX_CONSTANT_BIAS;
	const struct vertex_element *position_element = element_of(shader, position_register);
	const unsigned char *positions;
	float point[3], center[3];
	unsigned key, keys = keyed ? draw_keys.count : 1;
	UINT whole;
	int row;

	if (!gx_lighting_space_make(constants, &lit.space))
		return FALSE;
	for (key = 0; key < keys; key++)
		gx_lighting_space_rows(&lit.space, (const float (*)[4])(keyed ? draw_keys.rows[key] : position_matrix_rows),
			lit.rows[key]);
	/* (the draw's center: a vertex of its first key) */
	whole = keyed ? draw_keys.representative[0] : first_vertex;
	whole = fetch->indices ? (UINT)fetch->indices[whole] + device.base_vertex_index : fetch->first + whole;
	positions = fetch->streams[position_element->stream & (MAXIMUM_STREAMS - 1)] + position_element->offset;
	word_copy(&point[0], positions + whole * device.streams[position_element->stream & (MAXIMUM_STREAMS - 1)].stride);
	word_copy(&point[1], positions + whole * device.streams[position_element->stream & (MAXIMUM_STREAMS - 1)].stride + 4);
	word_copy(&point[2], positions + whole * device.streams[position_element->stream & (MAXIMUM_STREAMS - 1)].stride + 8);
	for (row = 0; row < 3; row++)
		center[row] = lit.rows[0][row][0] * point[0] + lit.rows[0][row][1] * point[1] + lit.rows[0][row][2] * point[2] +
			lit.rows[0][row][3];
	if (!gx_lighting_make(lit.recipe, constants, &lit.space, center, &lit.lighting))
		return FALSE;
	memcpy(lit.center, center, sizeof(lit.center));
	lit.alpha = lit.colors && lit.lighting.alpha != _gx_lighting_alpha_zero;
	if (lit.alpha)
		for (key = 0; key < keys; key++)
			lit_world_rows(lit.rows[key], lit.world[key]);
	/* (a matrix for each key, and the processor's vertices', where they fit) */
	if (lit.reflection && (keyed ? draw_keys.count + (draw_keys.processor ? 1u : 0u) : 1u) > LIT_SPHERE_MATRICES)
		lit.reflection = FALSE;
	if (lit.reflection)
	{
		float toward[3];

		for (row = 0; row < 3; row++)
			toward[row] = lit.space.eye[row] - center[row];
		gx_lighting_view_frame(&lit.space, toward, LIT_SPHERE_STEP, lit.frame, lit.turn);
		if (!sphere_bake(bound_entries[3], (const float (*)[3])lit.frame, lit.lighting.reflection_scale, &lit.sphere))
			lit.reflection = FALSE;
	}
	return lit.colors || lit.reflection;
}

/* the fogs' ramps, each of one form (gx_lighting_fog_form: the fog's c-85
y and z, and the product's atmospheric fog), kept while it is the same:
one is written again only when no draw of this frame has read it (the
frames before are drawn: GX_DrawDone at Present). A ramp of
GX_LIGHTING_FOG_SIZE rows is held here, a longer one (the product's, over
the atmospheric fog's distance too) in the kept buffers' memory. */
#define FOG_RAMPS 4
static void *kept_alloc(unsigned long bytes);
static void kept_free_later(void *data, unsigned long bytes);
static struct
{
	unsigned char small[GX_LIGHTING_FOG_SIZE * GX_LIGHTING_FOG_SIZE] __attribute__((aligned(32)));
	/* (small, or the kept buffer of bytes) */
	unsigned char *texels;
	unsigned long bytes;
	struct gx_lighting_fog_form form;
	unsigned long frame_number;
	BOOL made;
} fog_ramps[FOG_RAMPS];
static unsigned fog_ramps_next;
/* in the statistics' window: draws whose fog GX made (of them, the
product's), ramps written, and draws whose fog stayed the processor's (a
product of fogs of other directions or too long, no map free, no room for
the ramp, beyond the ramp's range, a pixel shader beyond GX with it) */
static unsigned long lit_fog_draws, lit_fog_products, fog_ramps_made, lit_fog_refused;

/* D0's alpha the fog's ramp (lit.fog_ramped): the ramp bound to a
texture map the pixel shader leaves free, the pixel shader translated again
to read its alpha there for D0's, and texture coordinate 5 made of the
position through the vertex's own matrix (to E) and the post-transform
matrix GX_DTTMTX0 (to the ramp's coordinates), so the vertices send no
alpha. FALSE: the vertices' alpha, the pixel shader as it was. */
static BOOL lit_fog_ramp(BOOL keyed)
{
	const struct gx_psh_textures *textures = &tev_textures;
	struct gx_psh_textures ramped;
	const struct gx_psh_program *program;
	BOOL usable, translated;
	float matrix[3][4];
	unsigned which, slot;
	int map;
	GXTexObj object;
	struct gx_lighting_fog_form form;

	if (!pixel_shader_usable || !gx_lighting_fog_form(&lit.lighting, &form))
		return FALSE;
	/* (a map of 4 to 7 that the stages' separate alphas, the fog's ramp
	and a bumped lookup leave free) */
	for (map = 7; map >= 4; map--)
		if (!(textures->separate_alpha & (1U << (map - 4))) && !(map == GX_PSH_FOG_TEXMAP && textures->fog) &&
			!(map == GX_PSH_BUMP_TEXMAP && (textures->bumped || textures->vertex_color)))
			break;
	if (map < 4 || !gx_lighting_fog_matrix(&lit.lighting, &form, &lit.space, lit.center, matrix))
		return FALSE;
	for (which = 0; which < FOG_RAMPS; which++)
		if (fog_ramps[which].made && !memcmp(&fog_ramps[which].form, &form, sizeof(form)))
			break;
	if (which == FOG_RAMPS)
	{
		unsigned char band[4][GX_LIGHTING_FOG_SIZE];
		unsigned long bytes = (unsigned long)form.rows * GX_LIGHTING_FOG_SIZE;
		unsigned char *texels;
		unsigned row, column;

		for (slot = 0; slot < FOG_RAMPS; slot++)
		{
			which = (fog_ramps_next + slot) % FOG_RAMPS;
			if (!fog_ramps[which].made || fog_ramps[which].frame_number != device.frames)
				break;
		}
		if (slot == FOG_RAMPS)
			return FALSE;
		/* (the slot's memory: its own, or a kept buffer of the ramp's rows;
		the one it had freed once GX has drawn the frame) */
		texels = form.rows == GX_LIGHTING_FOG_SIZE ? fog_ramps[which].small :
			fog_ramps[which].bytes == bytes ? fog_ramps[which].texels : kept_alloc(bytes);
		if (!texels)
			return FALSE;
		if (fog_ramps[which].bytes && texels != fog_ramps[which].texels)
		{
			kept_free_later(fog_ramps[which].texels, fog_ramps[which].bytes);
			fog_ramps[which].bytes = 0;
		}
		fog_ramps_next = which + 1;
		/* (I8 in 8x4 tiles, a band of four rows at a time) */
		for (row = 0; row < form.rows; row += 4)
		{
			gx_lighting_fog_ramp(&form, row, 4, &band[0][0]);
			for (column = 0; column < GX_LIGHTING_FOG_SIZE; column++)
			{
				unsigned line;

				for (line = 0; line < 4; line++)
					texels[((row / 4) * (GX_LIGHTING_FOG_SIZE / 8) + column / 8) * 32 + line * 8 + column % 8] =
						band[line][column];
			}
		}
		DCFlushRange(texels, bytes);
		/* (GX's texture cache may hold the texels it had) */
		GX_InvalidateTexAll();
		fog_ramps[which].texels = texels;
		fog_ramps[which].bytes = texels == fog_ramps[which].small ? 0 : bytes;
		fog_ramps[which].form = form;
		fog_ramps[which].made = TRUE;
		fog_ramps_made++;
	}
	memcpy(&ramped, textures, sizeof(ramped));
	ramped.d0_alpha_map = (unsigned char)map;
	program = gx_psh_cache_program(D3D__RenderState, &ramped, &usable, &translated);
	if (!usable)
	{
		/* (the draw's own again: the cache keeps the last program it gave) */
		pixel_shader_program = gx_psh_cache_program(D3D__RenderState, textures, &usable, &translated);
		return FALSE;
	}
	fog_ramps[which].frame_number = device.frames;
	pixel_shader_program = program;
	gx_psh_apply(program);
	indirect_apply(program);
	if (form.product)
		lit_fog_products++;
	GX_InitTexObj(&object, fog_ramps[which].texels, GX_LIGHTING_FOG_SIZE, (u16)form.rows, GX_TF_I8, GX_CLAMP,
		GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&object, GX_LINEAR, GX_LINEAR);
	GX_LoadTexObj(&object, (u8)map);
	/* (the first matrix the position's own: GX_PNMTX1 for a draw of one
	matrix, else each vertex's index, as its position's) */
	GX_LoadTexMtxImm(matrix, GX_DTTMTX0, GX_MTX3x4);
	GX_SetTexCoordGen2(GX_TEXCOORD5, GX_TG_MTX3x4, GX_TG_POS, GX_PNMTX1, GX_FALSE, GX_DTTMTX0);
	GX_SetNumTexGens(GX_PSH_D0_ALPHA_TEXCOORD + 1);
	if (keyed)
		GX_SetVtxDesc(GX_VA_TEX5MTXIDX, GX_DIRECT);
	return TRUE;
}

/* GX's state for a lit draw: E's matrices (the normal's too) and
projection, the normal, the lights and the sphere map's coordinates
(after position_matrix_load and the texture coordinates' generation) */
static void lit_state(BOOL keyed, const struct draw_projection *projection, const float d1_constant[4])
{
	float matrix[4][4];
	unsigned key, keys = keyed ? draw_keys.count : 1;

	gx_lighting_space_projection(&lit.space, SCREEN_WIDTH, SCREEN_HEIGHT, projection->a, projection->b, matrix);
	GX_LoadProjectionMtx(matrix, GX_PERSPECTIVE);
	projection_valid = FALSE;
	for (key = 0; key < keys; key++)
	{
		GX_LoadPosMtxImm(lit.rows[key], (u32)(GX_PNMTX1 + 3 * key));
		GX_LoadNrmMtxImm(lit.rows[key], (u32)(GX_PNMTX1 + 3 * key));
	}
	if (draw_keys.processor)
	{
		/* (the processor's (x w, y w, -w) through K's inverse; its normal, the
		world's, through the world's turn to E) */
		Mtx inverse, turn;
		int row;

		memset(inverse, 0, sizeof(inverse));
		inverse[0][0] = 1.0f / lit.space.k00;
		inverse[0][2] = -lit.space.k02 / lit.space.k00;
		inverse[1][1] = 1.0f / lit.space.k11;
		inverse[1][2] = -lit.space.k12 / lit.space.k11;
		inverse[2][2] = 1.0f;
		GX_LoadPosMtxImm(inverse, GX_PNMTX0);
		for (row = 0; row < 3; row++)
			memcpy(turn[row], lit.space.world_to_eye[row], sizeof(turn[row]));
		GX_LoadNrmMtxImm(turn, GX_PNMTX0);
	}
	GX_SetVtxDesc(GX_VA_NRM, lit.normals_indexed ? GX_INDEX16 : GX_DIRECT);
	vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
	if (lit.normals_indexed)
		GX_SetArray(GX_VA_NRM, (void *)lit.normals_array, (u8)lit.normals_array_stride);
	if (lit.colors)
	{
		if (lit.alpha && lit_fog_ramp(keyed))
		{
			lit.alpha = FALSE;
			lit.fog_ramped = TRUE;
			lit_fog_draws++;
		}
		else if (lit.alpha)
			lit_fog_refused++;
		lit_lights_load(d1_constant);
		if (lit.alpha)
		{
			GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
			vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
		}
	}
	if (lit.reflection)
	{
		for (key = 0; key < keys; key++)
			lit_sphere_matrix((const float (*)[4])lit.rows[key], lit_sphere_rows[key]);
		if (draw_keys.processor)
		{
			float rows[3][4];
			int row;

			for (row = 0; row < 3; row++)
				memcpy(rows[row], lit.space.world_to_eye[row], sizeof(rows[row]));
			lit_sphere_matrix((const float (*)[4])rows, lit_sphere_rows[keys]);
		}
		if (keyed)
			GX_SetVtxDesc(GX_VA_TEX3MTXIDX, GX_DIRECT);
		GX_SetTexCoordGen(GX_TEXCOORD3, GX_TG_MTX2x4, GX_TG_NRM, lit_sphere_rows[0]);
		GX_LoadTexObj(&lit.sphere, GX_TEXMAP3);
	}
}

/* (emit_indices' normals: none, each decoded and sent, or read by GX by
index) */
enum { EMIT_NORMALS_NONE, EMIT_NORMALS_SENT, EMIT_NORMALS_INDEXED };

/* GX's FIFO takes a vertex's bytes in order whatever the width of the
stores that write them: these write a byte, two or four, whatever
attribute they belong to, through libogc's own writes (which
tools/wii_draw_bench.py's recording header records) */
static inline __attribute__((always_inline)) void fifo_u8(u8 value)
{
	GX_MatrixIndex1x8(value);
}

static inline __attribute__((always_inline)) void fifo_u16(u16 value)
{
	GX_Position1x16(value);
}

static inline __attribute__((always_inline)) void fifo_u32(u32 value)
{
	GX_Color1u32(value);
}

/* three words sent as they are (floats' bits: the same bytes in the FIFO as
GX_Position3f32's or GX_Normal3f32's) */
static inline __attribute__((always_inline)) void hardware_words_send(const void *values)
{
	u32 words[3];

	memcpy(words, values, sizeof(words));
	fifo_u32(words[0]);
	fifo_u32(words[1]);
	fifo_u32(words[2]);
}

/* emit_indices' keys' matrix indices, a word each as hardware_key_words
(the plain draws' keys are their matrices', below MATRIX_KEYS) */
static u32 emit_key_words[256];

/* the vertices of a draw in hardware of which the processor computes nothing:
each index's matrix indices (keyed: its value's position matrix, and with
sphere_matrices its normal's sphere map matrix, with fog_post_matrices its
fog's (fog_post_matrix), with fog_matrices its planar fog ramp
coordinate's), its position and texture coordinates by index, and a
lit draw's normal. A loop for each case, so that an index costs only what
it sends. */
static void emit_indices(const WORD *indices, UINT first, UINT count, unsigned texture_count, BOOL keyed, int normals,
	BOOL sphere_matrices, BOOL fog_post_matrices, BOOL fog_matrices)
{
	UINT base = indices ? device.base_vertex_index : first, vertex;

/* (each index's own value, u16 index, then body) */
#define EMIT_EACH(body) \
	do { \
		if (indices) \
			for (vertex = 0; vertex < count; vertex++) \
			{ \
				u16 index = (u16)(indices[vertex] + base); \
				body \
			} \
		else \
			for (vertex = 0; vertex < count; vertex++) \
			{ \
				u16 index = (u16)(vertex + base); \
				body \
			} \
	} while (0)

	if (!keyed && !normals)
	{
		switch (texture_count)
		{
		case 0: EMIT_EACH(GX_Position1x16(index);); break;
		case 1: EMIT_EACH(GX_Position1x16(index); GX_TexCoord1x16(index);); break;
		case 2: EMIT_EACH(GX_Position1x16(index); GX_TexCoord1x16(index); GX_TexCoord1x16(index);); break;
		case 3:
			EMIT_EACH(GX_Position1x16(index); GX_TexCoord1x16(index); GX_TexCoord1x16(index); GX_TexCoord1x16(index););
			break;
		case 4:
			EMIT_EACH(GX_Position1x16(index); GX_TexCoord1x16(index); GX_TexCoord1x16(index); GX_TexCoord1x16(index);
				GX_TexCoord1x16(index););
			break;
		default:
			EMIT_EACH(
				unsigned stage;

				GX_Position1x16(index);
				for (stage = 0; stage < texture_count; stage++)
					GX_TexCoord1x16(index);
			);
			break;
		}
		return;
	}
	{
		/* (as hardware_emit's loops: what the loop reads in registers, each
		key's matrix indices a word, sent in one store or two, and the
		indices two to a word) */
		const unsigned char *const keys = draw_key_of;
		const unsigned matrices = keyed ? 1 + (sphere_matrices ? 1 : 0) + (fog_post_matrices ? 1 : 0) +
			(fog_matrices ? 1 : 0) : 0;
		const unsigned pairs = texture_count >> 1, odd = texture_count & 1;
		const BOOL indexed = normals == EMIT_NORMALS_INDEXED;
		u32 *const words = emit_key_words;
		/* (the words made for this layout of matrix indices, until another) */
		static unsigned words_layout = ~0U;
		unsigned key, layout = (sphere_matrices ? 1U : 0U) | (fog_post_matrices ? 2U : 0U) | (fog_matrices ? 4U : 0U);

		for (key = 0; keyed && layout != words_layout && key < MATRIX_KEYS; key++)
		{
			u8 position = (u8)(GX_PNMTX1 + 3 * key), bytes[4] = { 0, 0, 0, 0 };
			unsigned count_bytes = 1;

			bytes[0] = position;
			if (sphere_matrices)
				bytes[count_bytes++] = lit_sphere_rows[key];
			if (fog_post_matrices)
				bytes[count_bytes++] = position;
			if (fog_matrices)
				bytes[count_bytes++] = position;
			words[key] = (u32)bytes[0] << 24 | (u32)bytes[1] << 16 | (u32)bytes[2] << 8 | bytes[3];
		}
		if (keyed)
			words_layout = layout;
		EMIT_EACH(
			unsigned stage;
			u32 pair = (u32)index << 16 | index;

			if (matrices)
			{
				/* (a plain draw's keys are its matrices', none the processor's) */
				u32 word = words[keys[vertex]];

				if (matrices == 1)
					fifo_u8((u8)(word >> 24));
				else if (matrices == 2)
					fifo_u16((u16)(word >> 16));
				else if (matrices == 3)
				{
					fifo_u16((u16)(word >> 16));
					fifo_u8((u8)(word >> 8));
				}
				else
					fifo_u32(word);
			}
			if (indexed)
				fifo_u32(pair);
			else
			{
				fifo_u16(index);
				if (normals)
				{
					float normal[3];

					lit_normal(index, normal);
					hardware_words_send(normal);
				}
			}
			if (odd)
				fifo_u16(index);
			for (stage = 0; stage < pairs; stage++)
				fifo_u32(pair);
		);
	}
#undef EMIT_EACH
}

/* ---------- a model's fog from its nodes

The fog of a model's vertex is a function of where its nodes put it: one
texture matrix of its position does not serve every node's vertices. But it
is a function of the world position, which each node's position matrix
(GX_PNMTX1 on: the device's (x w, y w, -w), or a lit draw's E) maps the
vertex to by an affine map the camera makes invertible: so the fog is one
affine function of that matrix's result, for every node and for the
processor's own vertices (through GX_PNMTX0, the same space). Texture
coordinate 4 is then made from the position through the vertex's own
position matrix (its matrix index, as the planar fog's ramp's coordinate is:
lit_fog_ramp) and the post-transform matrix GX_DTTMTX1, that function. It is
solved from the first node's (the matrix loaded and the fog's row in the
model's space, stage_rows[4]) and checked against the program at a vertex
of each other node; FALSE where it does not hold (the draw is then the
processor's). */

/* the post-transform rows of the fog's ramp coordinate (row 0; row 1 none,
row 2 the q of 1) from the first key's position rows and fog row */
static BOOL fog_post_matrix(const float rows[3][4], const float fog_row[4], Mtx post)
{
	float a = rows[0][0], b = rows[0][1], c = rows[0][2], d = rows[1][0], e = rows[1][1], f = rows[1][2];
	float g = rows[2][0], h = rows[2][1], i = rows[2][2], determinant, inverse[3][3];
	int row, column;

	determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (!(fabsf(determinant) > 1.0e-20f))
		return FALSE;
	inverse[0][0] = (e * i - f * h) / determinant;
	inverse[0][1] = (c * h - b * i) / determinant;
	inverse[0][2] = (b * f - c * e) / determinant;
	inverse[1][0] = (f * g - d * i) / determinant;
	inverse[1][1] = (a * i - c * g) / determinant;
	inverse[1][2] = (c * d - a * f) / determinant;
	inverse[2][0] = (d * h - e * g) / determinant;
	inverse[2][1] = (b * g - a * h) / determinant;
	inverse[2][2] = (a * e - b * d) / determinant;
	memset(post, 0, sizeof(Mtx));
	/* (the fog's row of the model's position through the rows' inverse:
	fog = g . (R p + t) for g = f R^-1, and its constant f3 - g . t) */
	for (column = 0; column < 3; column++)
		for (row = 0; row < 3; row++)
			post[0][column] += fog_row[row] * inverse[row][column];
	post[0][3] = fog_row[3];
	for (row = 0; row < 3; row++)
		post[0][3] -= post[0][row] * rows[row][3];
	post[2][3] = 1.0f;
	return TRUE;
}

/* whether the post-transform rows give each key's fog: at its
representative vertex, the program's against the rows' (to a ramp's texel) */
static BOOL fog_post_check(const Mtx post, const Mtx *key_rows, struct nv2a_cpu_program *program, void *fetch,
	int position_register, float point_size)
{
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	struct nv2a_cpu_output out;
	unsigned key;

	inputs_seed(inputs);
	for (key = 0; key < draw_keys.count; key++)
	{
		const float *p;
		float place[3], predicted, actual;
		int row;

		stream_fetch(fetch, draw_keys.representative[key], inputs);
		nv2a_cpu_run_live(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size,
			&out, NV2A_CPU_LIVE_FOG);
		p = inputs[position_register];
		for (row = 0; row < 3; row++)
			place[row] = key_rows[key][row][0] * p[0] + key_rows[key][row][1] * p[1] + key_rows[key][row][2] * p[2] +
				key_rows[key][row][3];
		predicted = post[0][0] * place[0] + post[0][1] * place[1] + post[0][2] * place[2] + post[0][3];
		/* (as the ramp clamps it) */
		if (!(predicted > 0.5f / 256.0f))
			predicted = 0.5f / 256.0f;
		if (predicted > 255.5f / 256.0f)
			predicted = 255.5f / 256.0f;
		actual = fog_coordinate(out.fog);
		if (!(fabsf(predicted - actual) < 1.0f / 256.0f))
			return FALSE;
	}
	return TRUE;
}

/* in the statistics' window: draws whose model's fog went through the nodes'
matrices (fog_post_matrix) */
static unsigned long fog_post_draws;

/* why a program's draws stay on the processor: once a program, for the log */
static BOOL hardware_rejected(const struct nv2a_cpu_program *program, const char *reason)
{
	static unsigned long logged[32];
	static unsigned count;
	unsigned index;

	for (index = 0; index < count; index++)
		if (logged[index] == program->hash)
			return FALSE;
	if (count < 32)
	{
		logged[count++] = program->hash;
		platform_log("Direct3D: program %08lx on the processor: %s", program->hash, reason);
	}
	return FALSE;
}

/* ---------- the kept buffers' memory

What the device keeps for GX to read (the static buffers' outputs, the lit
models' normals, the structure's indices as display lists) took pages of
the contiguous window, top-down as the game's blocks are, a page at least
each (a display list of 1.5 KB took 4), and only while the window kept a
megabyte free in one run besides. The game's blocks and the device's then
lay between one another; once the level was loaded the window's largest
free run was the 1016 KB below the tag cache (b30: 2996 KB free in all), and
every kept buffer was refused.

Now they come from a pool of their own:
- Pages from the window's bottom (wii_memory_window_alloc_low): the
  smallest free run a block fits, at its bottom, where the game's top-down
  blocks reach last; the holes the game's blocks left between them first.
  A block is refused where it would leave the window's largest free run
  below KEPT_MARGIN (unless that run was not the one it took).
- A buffer of KEPT_SLAB_LARGEST or less is cut from a slab of
  KEPT_SLAB_BYTES (32-byte granules, a bit each); a larger one has pages of
  its own, as GX reads each draw's arrays and lists from one block.
- At most KEPT_POOL_BYTES in all (each kind keeps its own budget within).
- A buffer is freed only once GX has drawn what reads it: at the end of the
  frame (kept_free_later), or at once after GX_DrawDone.
- The game's blocks come first: where one finds no room in the window,
  wii_memory.c calls kept_reclaim, which (on the device's thread, outside a
  draw's use of the kept) waits for GX and frees every kept buffer, and the
  block is looked for again. KEPT_MARGIN keeps that rare: the blocks made
  once a level is loaded are the loading screen's (320 by 240, 300 KB) and
  the device's own (a sphere map's 9 KB, and the back buffer's parking,
  two of 1200 KB made once, which go to the heap where the window has no
  room).
- A buffer refused (a draw's) is not asked for again until room may have
  come: kept_room changes, once a frame is drawn, only where a block was
  freed (the window's pages, or a slab's granules); and within one, a
  window block as large as one refused is refused without a look. */
#define KEPT_SLAB_BYTES (64UL * 1024UL)
#define KEPT_SLAB_GRANULE 32UL
#define KEPT_SLAB_GRANULES (KEPT_SLAB_BYTES / KEPT_SLAB_GRANULE)
#define KEPT_SLAB_LARGEST (16UL * 1024UL)
#define KEPT_SLABS 32
#define KEPT_POOL_BYTES (2560UL * 1024UL)
#define KEPT_MARGIN (512UL * 1024UL)
#define KEPT_FREES 512

static struct kept_slab
{
	unsigned char *base;
	u32 used[KEPT_SLAB_GRANULES / 32];
	unsigned long granules;
	/* (no free run is longer: a request of more passes the slab by;
	KEPT_SLAB_GRANULES again once granules are freed) */
	unsigned long longest;
} kept_slabs[KEPT_SLABS];
/* the window's bytes the pool holds (slabs whole, blocks by their pages),
the slabs' granules in use, and the blocks of their own */
static unsigned long kept_pool_bytes, kept_slab_granules, kept_blocks;
/* the buffers to free once GX has drawn the frame */
static struct
{
	void *data;
	unsigned long bytes;
} kept_frees[KEPT_FREES];
static unsigned kept_free_count;
/* (changes when room may be had again: kept_frees_flush) */
static unsigned long kept_room = 1, kept_room_frees, kept_slab_frees;
/* (the smallest window block refused while kept_room was this) */
static unsigned long kept_refused_room, kept_refused_bytes;
/* (a draw reads the kept buffers: kept_reclaim leaves them; and the
device's thread, the only one that may free them) */
static BOOL kept_pinned;
static pthread_t kept_thread;
/* in the statistics' window: window blocks refused, and reclaims */
static unsigned long kept_window_refused, kept_reclaims;

/* the window's pages for the pool (NULL: none, or not without taking the
game's margin) */
static void *kept_window_alloc(unsigned long bytes)
{
	void *data;

	if ((kept_refused_room == kept_room && bytes >= kept_refused_bytes) || kept_pool_bytes + bytes > KEPT_POOL_BYTES)
		return NULL;
	data = wii_memory_window_alloc_low(bytes, KEPT_MARGIN);
	if (!data)
	{
		if (kept_refused_room != kept_room || bytes < kept_refused_bytes)
			kept_refused_bytes = bytes;
		kept_refused_room = kept_room;
		kept_window_refused++;
		return NULL;
	}
	kept_pool_bytes += bytes;
	return data;
}

/* a run of count free granules of the slab, marked used: its first; -1
where it has none */
static long kept_slab_take(struct kept_slab *slab, unsigned long count)
{
	unsigned long granule = 0, run = 0, start = 0, longest = 0;

	while (granule < KEPT_SLAB_GRANULES)
	{
		u32 word = slab->used[granule >> 5];

		if (!(granule & 31) && word == 0xFFFFFFFFU)
		{
			run = 0;
			granule += 32;
			continue;
		}
		if (word & (1U << (granule & 31)))
		{
			run = 0;
			granule++;
			continue;
		}
		if (!run)
			start = granule;
		if (!(granule & 31) && !word)
		{
			run += 32;
			granule += 32;
		}
		else
		{
			run++;
			granule++;
		}
		if (run > longest)
			longest = run;
		if (run >= count)
		{
			for (granule = start; granule < start + count; granule++)
				slab->used[granule >> 5] |= 1U << (granule & 31);
			slab->granules += count;
			return (long)start;
		}
	}
	slab->longest = longest;
	return -1;
}

/* a kept buffer of bytes, 32-byte aligned: NULL where there is no room */
static void *kept_alloc(unsigned long bytes)
{
	unsigned long count = (bytes + KEPT_SLAB_GRANULE - 1) / KEPT_SLAB_GRANULE;
	struct kept_slab *empty = NULL;
	unsigned index;

	if (!bytes)
		return NULL;
	if (bytes > KEPT_SLAB_LARGEST)
	{
		void *data = kept_window_alloc((bytes + 4095) & ~4095UL);

		if (data)
			kept_blocks++;
		return data;
	}
	for (index = 0; index < KEPT_SLABS; index++)
	{
		struct kept_slab *slab = &kept_slabs[index];
		long start;

		if (!slab->base)
		{
			if (!empty)
				empty = slab;
			continue;
		}
		if (count > slab->longest)
			continue;
		start = kept_slab_take(slab, count);
		if (start >= 0)
		{
			kept_slab_granules += count;
			return slab->base + (unsigned long)start * KEPT_SLAB_GRANULE;
		}
	}
	if (!empty || !(empty->base = kept_window_alloc(KEPT_SLAB_BYTES)))
		return NULL;
	memset(empty->used, 0, sizeof(empty->used));
	empty->granules = 0;
	empty->longest = KEPT_SLAB_GRANULES;
	kept_slab_take(empty, count);
	kept_slab_granules += count;
	return empty->base;
}

/* a kept buffer of bytes freed now (GX has drawn what reads it) */
static void kept_free(void *data, unsigned long bytes)
{
	unsigned long count = (bytes + KEPT_SLAB_GRANULE - 1) / KEPT_SLAB_GRANULE, granule, start;
	unsigned index;

	if (!data)
		return;
	if (bytes > KEPT_SLAB_LARGEST)
	{
		platform_contiguous_free(data);
		kept_pool_bytes -= (bytes + 4095) & ~4095UL;
		kept_blocks--;
		return;
	}
	for (index = 0; index < KEPT_SLABS; index++)
	{
		struct kept_slab *slab = &kept_slabs[index];

		if (!slab->base || (unsigned char *)data < slab->base || (unsigned char *)data >= slab->base + KEPT_SLAB_BYTES)
			continue;
		start = (unsigned long)((unsigned char *)data - slab->base) / KEPT_SLAB_GRANULE;
		for (granule = start; granule < start + count; granule++)
			slab->used[granule >> 5] &= ~(1U << (granule & 31));
		slab->granules -= count;
		slab->longest = KEPT_SLAB_GRANULES;
		kept_slab_granules -= count;
		kept_slab_frees++;
		if (!slab->granules)
		{
			/* (an empty slab's pages back to the window) */
			platform_contiguous_free(slab->base);
			kept_pool_bytes -= KEPT_SLAB_BYTES;
			slab->base = NULL;
		}
		return;
	}
}

/* a kept buffer freed once GX has drawn the frame (or now, once GX has
drawn what it was given, past room to wait) */
static void kept_free_later(void *data, unsigned long bytes)
{
	if (!data)
		return;
	if (kept_free_count < KEPT_FREES)
	{
		kept_frees[kept_free_count].data = data;
		kept_frees[kept_free_count].bytes = bytes;
		kept_free_count++;
		return;
	}
	GX_DrawDone();
	xf_flush_after();
	kept_free(data, bytes);
}

/* once GX has drawn the frame: the buffers pushed out freed, and whether
room may be had again */
static void kept_frees_flush(void)
{
	unsigned long frees;

	while (kept_free_count)
	{
		kept_free_count--;
		kept_free(kept_frees[kept_free_count].data, kept_frees[kept_free_count].bytes);
	}
	frees = wii_memory_window_frees() + kept_slab_frees;
	if (frees != kept_room_frees)
	{
		kept_room_frees = frees;
		kept_room++;
	}
}

/* ---------- the outputs of static vertex buffers, kept

An output of a program that reads no constant (nv2a_cpu_analyze_constants)
is, for a vertex, the same whatever the draw's constants: a function of the
vertex buffers alone. The environment's bumped lightmap pass (10c30c53)
writes one: its texture coordinate 3, the incoming light's direction (v7)
in the surface's tangent frame (v1 to v3), a cube map's direction whose
sphere map coordinates the processor computed for each vertex, 33,000 a
frame in a10. Where what a draw has the processor compute is such an output
(a sphere-mapped stage whose direction's x, y and z read no constant, or
both colours, every component), and the streams it reads are static, it is
computed once for each vertex, kept beside the buffers in the contiguous
window, and read by GX by index as the buffers are (draw_hardware).
- A vertex buffer is static while the game does not lock it: its header's
  Lock, D3D's own field, holds a serial the device gives it when it first
  keys outputs by it, and d3d8_resources.c's D3DVertexBuffer_Lock clears it
  (as it does a texture's mark). Kept outputs are keyed by each stream they
  read (its header, data, stride and serial), the program, the shader (its
  declaration) and the stages and colours kept.
- An entry's outputs are computed only from the frame after it was first
  seen: a buffer locked each frame (the dynamic ones) is never kept.
- A vertex is computed when a draw first names it (a bit a vertex) as
  draw_hardware computes it, nv2a_cpu_run_live for those outputs alone (the
  same to the bit: tests/nv2a_native_check.c), and what is new flushed from
  the processor's cache before GX reads it.
- At most STATIC_OUTPUTS_BYTES in all, in the kept buffers' memory (the
  game's own buffers and textures come first). An entry not drawn for
  STATIC_OUTPUTS_IDLE frames is freed, and one pushed out once GX has drawn
  the frame. An entry refused room is not asked for again while kept_room
  is the same. */
#define STATIC_OUTPUTS_ENTRIES 32
#define STATIC_OUTPUTS_STREAMS 4
#define STATIC_OUTPUTS_BYTES (1024UL * 1024UL)
#define STATIC_OUTPUTS_IDLE 120

struct static_outputs
{
	BOOL used;
	/* (given when the entry is made: an index record names it by this,
	index_record.kept_generation) */
	unsigned long generation;
	unsigned long program_hash, shader_id;
	/* the sphere-mapped stages kept (bit n: stage n), and the colours */
	unsigned stages;
	BOOL colors;
	struct
	{
		const D3DVertexBuffer *buffer;
		DWORD data, serial;
		UINT stride;
	} streams[STATIC_OUTPUTS_STREAMS];
	unsigned stream_count;
	unsigned long seen_frame, used_frame;
	/* (the kept_room its values were refused in) */
	unsigned long refused;
	/* each vertex's (stride bytes): the kept stages' sphere map coordinates,
	two floats each in the stages' order, then the colours' bytes, D0's and
	D1's; in the kept buffers' memory */
	unsigned char *values;
	unsigned stride;
	unsigned long capacity;
	/* (a bit a vertex: computed) */
	unsigned long *computed;
};

static struct static_outputs static_outputs[STATIC_OUTPUTS_ENTRIES];
static unsigned long static_outputs_generation;
static unsigned long static_outputs_bytes;
static DWORD vertex_buffer_serial;
/* the log's: draws that read kept outputs, their vertices, the vertices
computed for them, and draws whose outputs could not be kept for want of
memory */
static unsigned long kept_draws, kept_vertices, kept_computed, kept_refused;

/* an entry's values freed: at once where GX has drawn the frame, else once
it has */
static void static_outputs_release_values(struct static_outputs *entry, BOOL drawn)
{
	if (!entry->values)
		return;
	if (drawn)
		kept_free(entry->values, entry->capacity * entry->stride);
	else
		kept_free_later(entry->values, entry->capacity * entry->stride);
	static_outputs_bytes -= entry->capacity * entry->stride;
	entry->values = NULL;
}

static void static_outputs_release(struct static_outputs *entry, BOOL drawn)
{
	static_outputs_release_values(entry, drawn);
	free(entry->computed);
	memset(entry, 0, sizeof(*entry));
}

/* once GX has drawn the frame (Present): the entries not drawn for a while */
static void static_outputs_frame_drawn(void)
{
	unsigned index;

	for (index = 0; index < STATIC_OUTPUTS_ENTRIES; index++)
		if (static_outputs[index].used && device.frames - static_outputs[index].used_frame > STATIC_OUTPUTS_IDLE)
			static_outputs_release(&static_outputs[index], TRUE);
}

/* ---------- the streams GX reads, flushed once while they are static

GX reads a draw's streams from memory, so the processor's cache is flushed
over the range the draw reads (DCFlushRange, a dcbf a line). The models'
and the structure's buffers are written once, and drawn many times a frame,
pass after pass: their ranges were flushed at every draw. A buffer's serial
(its header's Lock, as the kept outputs take it: given at first sight,
cleared by D3DVertexBuffer_Lock) says it has not been written through a
lock since; the range flushed under the serial is kept for each buffer
(STREAM_FLUSHES of them, by the header's address), and a draw within it
flushes nothing. A serial's own first frame is flushed at every draw, as a
buffer the game locks each frame (the dynamic ones) always is. */
#define STREAM_FLUSHES 256

static struct
{
	const D3DVertexBuffer *buffer;
	DWORD data, serial;
	unsigned long frame;
	/* the bytes flushed under the serial since its first frame, from the
	buffer's start: [first, end) */
	unsigned long first, end;
} stream_flushes[STREAM_FLUSHES];
/* in the statistics' window: flushes made and their bytes, and those not
needed */
static unsigned long stream_flushes_made, stream_flush_bytes, stream_flushes_kept;

static void stream_flush(D3DVertexBuffer *buffer, const unsigned char *start, unsigned long bytes)
{
	const unsigned char *base;
	unsigned long first, end;
	unsigned slot;

	if (!buffer || !buffer->Data)
	{
		DCFlushRange((void *)start, bytes);
		stream_flushes_made++;
		stream_flush_bytes += bytes;
		return;
	}
	if (!buffer->Lock)
	{
		if (!++vertex_buffer_serial)
			vertex_buffer_serial++;
		buffer->Lock = vertex_buffer_serial;
	}
	base = (const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(buffer->Data);
	first = (unsigned long)(start - base);
	end = first + bytes;
	slot = ((unsigned long)buffer >> 4) % STREAM_FLUSHES;
	if (stream_flushes[slot].buffer != buffer || stream_flushes[slot].data != buffer->Data ||
		stream_flushes[slot].serial != buffer->Lock)
	{
		stream_flushes[slot].buffer = buffer;
		stream_flushes[slot].data = buffer->Data;
		stream_flushes[slot].serial = buffer->Lock;
		stream_flushes[slot].frame = device.frames;
		stream_flushes[slot].first = stream_flushes[slot].end = 0;
	}
	if (stream_flushes[slot].frame != device.frames && first >= stream_flushes[slot].first &&
		end <= stream_flushes[slot].end)
	{
		stream_flushes_kept++;
		return;
	}
	DCFlushRange((void *)start, bytes);
	stream_flushes_made++;
	stream_flush_bytes += bytes;
	/* (kept from the frame after the serial's first: one range, grown where
	the new one meets it, else the larger) */
	if (stream_flushes[slot].frame == device.frames)
		return;
	if (first <= stream_flushes[slot].end && end >= stream_flushes[slot].first &&
		stream_flushes[slot].end > stream_flushes[slot].first)
	{
		if (first < stream_flushes[slot].first)
			stream_flushes[slot].first = first;
		if (end > stream_flushes[slot].end)
			stream_flushes[slot].end = end;
	}
	else if (end - first > stream_flushes[slot].end - stream_flushes[slot].first)
	{
		stream_flushes[slot].first = first;
		stream_flushes[slot].end = end;
	}
}

/* the stages and colours of what the processor computes for a draw
(spheres, colors) whose outputs read no constant and depend only on
registers of bound streams; the streams in *streams */
static unsigned static_outputs_wanted(const struct vertex_shader_object *shader, const struct stream_fetch *fetch,
	unsigned spheres, BOOL colors, BOOL *colors_kept, unsigned *streams)
{
	const struct nv2a_cpu_dependencies *reads = &shader->constant_reads;
	unsigned long registers = 0, mask;
	unsigned stages = 0, stage;
	int reg, component;

	*colors_kept = FALSE;
	*streams = 0;
	for (stage = 0; stage < 4; stage++)
	{
		if (!(spheres & (1U << stage)))
			continue;
		mask = reads->t[stage][0] | reads->t[stage][1] | reads->t[stage][2];
		if (!(mask & NV2A_CPU_READS_CONSTANT))
		{
			stages |= 1U << stage;
			registers |= mask & 0xFFFF;
		}
	}
	if (colors)
	{
		mask = 0;
		for (component = 0; component < 4; component++)
			mask |= reads->d0[component] | reads->d1[component];
		if (!(mask & NV2A_CPU_READS_CONSTANT))
		{
			*colors_kept = TRUE;
			registers |= mask & 0xFFFF;
		}
	}
	for (reg = 0; reg < NV2A_CPU_INPUT_COUNT; reg++)
	{
		const struct vertex_element *element;
		unsigned stream;

		if (!(registers & (1UL << reg)))
			continue;
		element = element_of(shader, reg);
		stream = element ? element->stream & (MAXIMUM_STREAMS - 1) : 0;
		if (!element || !fetch->streams[stream] || !device.streams[stream].buffer)
		{
			*colors_kept = FALSE;
			return 0;
		}
		*streams |= 1U << stream;
	}
	return stages;
}

/* the kept outputs for a draw (draw_hardware's, of vertices least to most,
none past 16 bits), every vertex it names computed; NULL where they are not
kept (not static yet, or no memory) */
static struct static_outputs *static_outputs_find(struct nv2a_cpu_program *program,
	const struct vertex_shader_object *shader, struct stream_fetch *fetch, UINT count, const WORD *indices,
	UINT first, unsigned long most, unsigned spheres, BOOL colors, float point_size)
{
	static unsigned last;
	struct static_outputs key, *entry = NULL;
	unsigned streams, stream, index, stage, stage_count = 0;
	BOOL colors_kept;
	unsigned long vertex, written_least = 0xFFFFFFFFUL, written_most = 0;

	memset(&key, 0, sizeof(key));
	key.stages = static_outputs_wanted(shader, fetch, spheres, colors, &colors_kept, &streams);
	key.colors = colors_kept;
	if (!key.stages && !key.colors)
		return NULL;
	key.program_hash = program->hash;
	key.shader_id = shader->id;
	for (stream = 0; stream < MAXIMUM_STREAMS; stream++)
	{
		D3DVertexBuffer *buffer = device.streams[stream].buffer;

		if (!(streams & (1U << stream)))
			continue;
		if (key.stream_count == STATIC_OUTPUTS_STREAMS)
			return NULL;
		/* (a buffer's serial: given at first sight, cleared by a lock) */
		if (!buffer->Lock)
		{
			if (!++vertex_buffer_serial)
				vertex_buffer_serial++;
			buffer->Lock = vertex_buffer_serial;
		}
		key.streams[key.stream_count].buffer = buffer;
		key.streams[key.stream_count].data = buffer->Data;
		key.streams[key.stream_count].serial = buffer->Lock;
		key.streams[key.stream_count].stride = device.streams[stream].stride;
		key.stream_count++;
	}
	/* (consecutive draws are mostly of one buffer) */
	for (index = 0; index < STATIC_OUTPUTS_ENTRIES; index++)
	{
		struct static_outputs *candidate = &static_outputs[(last + index) % STATIC_OUTPUTS_ENTRIES];

		if (candidate->used && candidate->program_hash == key.program_hash && candidate->shader_id == key.shader_id &&
			candidate->stages == key.stages && candidate->colors == key.colors &&
			candidate->stream_count == key.stream_count &&
			!memcmp(candidate->streams, key.streams, sizeof(key.streams)))
		{
			entry = candidate;
			last = (last + index) % STATIC_OUTPUTS_ENTRIES;
			break;
		}
	}
	if (!entry)
	{
		/* a new entry, in place of a free one, else one with no values, else
		the least recently drawn */
		struct static_outputs *victim = NULL;

		for (index = 0; index < STATIC_OUTPUTS_ENTRIES; index++)
		{
			struct static_outputs *candidate = &static_outputs[index];

			if (!candidate->used)
			{
				victim = candidate;
				break;
			}
			if (!victim || (!candidate->values && victim->values) ||
				(!candidate->values == !victim->values && candidate->used_frame < victim->used_frame))
				victim = candidate;
		}
		static_outputs_release(victim, FALSE);
		*victim = key;
		victim->used = TRUE;
		victim->generation = ++static_outputs_generation;
		victim->seen_frame = victim->used_frame = device.frames;
		return NULL;
	}
	entry->used_frame = device.frames;
	/* (not static yet: seen this frame only) */
	if (entry->seen_frame == device.frames)
		return NULL;
	if (most >= entry->capacity)
	{
		/* (room for the vertices up to most, in steps of 1024) */
		unsigned long capacity = (most + 1024) & ~1023UL;
		unsigned stride;
		unsigned long bytes;
		unsigned char *values = NULL;
		unsigned long *computed;

		/* (refused, and no room has come since) */
		if (entry->refused == kept_room)
		{
			kept_refused++;
			return NULL;
		}
		for (stage = 0; stage < 4; stage++)
			if (entry->stages & (1U << stage))
				stage_count++;
		stride = 8 * stage_count + (entry->colors ? 8 : 0);
		bytes = capacity * stride;
		if (static_outputs_bytes - entry->capacity * entry->stride + bytes <= STATIC_OUTPUTS_BYTES)
			values = kept_alloc(bytes);
		computed = values ? calloc((capacity + 31) / 32, sizeof(*computed)) : NULL;
		if (!values || !computed)
		{
			if (values)
				kept_free(values, bytes);
			entry->refused = kept_room;
			kept_refused++;
			return NULL;
		}
		if (entry->values)
		{
			/* (what was computed, kept; the old values freed once GX has
			drawn the frame) */
			memcpy(values, entry->values, (size_t)entry->capacity * stride);
			memcpy(computed, entry->computed, (entry->capacity + 31) / 32 * sizeof(*computed));
			written_least = 0;
			written_most = entry->capacity - 1;
			static_outputs_release_values(entry, FALSE);
		}
		free(entry->computed);
		entry->values = values;
		entry->computed = computed;
		entry->capacity = capacity;
		entry->stride = stride;
		static_outputs_bytes += bytes;
	}
	/* (the draw's indices name only vertices computed already: their record
	says so, while the entry and its values are the same) */
	if (draw_range.serial == draw_serial && draw_range.record && draw_range.record->kept_generation == entry->generation &&
		draw_range.record->kept_values == entry->values)
	{
		kept_draws++;
		kept_vertices += count;
		return entry;
	}
	/* the vertices the draw names that are not computed yet */
	for (vertex = 0; vertex < count; vertex++)
	{
		unsigned long whole = indices ? (unsigned long)indices[vertex] + device.base_vertex_index : first + vertex;
		float inputs[NV2A_CPU_INPUT_COUNT][4];
		struct nv2a_cpu_output out;
		unsigned char *values;

		if (entry->computed[whole >> 5] & (1UL << (whole & 31)))
			continue;
		memset(inputs, 0, sizeof(inputs));
		stream_fetch(fetch, vertex, inputs);
		nv2a_cpu_run_live(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size,
			&out, entry->stages * NV2A_CPU_LIVE_T0 | (entry->colors ? NV2A_CPU_LIVE_D0 | NV2A_CPU_LIVE_D1 : 0));
		values = entry->values + whole * entry->stride;
		for (stage = 0; stage < 4; stage++)
			if (entry->stages & (1U << stage))
			{
				float point[2];

				sphere_coordinates(out.t[stage], point);
				memcpy(values, point, sizeof(point));
				values += sizeof(point);
			}
		if (entry->colors)
		{
			int component;

			for (component = 0; component < 4; component++)
			{
				values[component] = color_byte(out.d0[component]);
				values[4 + component] = color_byte(out.d1[component]);
			}
		}
		entry->computed[whole >> 5] |= 1UL << (whole & 31);
		if (whole < written_least)
			written_least = whole;
		if (whole > written_most)
			written_most = whole;
		kept_computed++;
	}
	if (written_least <= written_most)
		DCFlushRange(entry->values + written_least * entry->stride, (written_most - written_least + 1) * entry->stride);
	if (draw_range.serial == draw_serial && draw_range.record)
	{
		draw_range.record->kept_generation = entry->generation;
		draw_range.record->kept_values = entry->values;
	}
	kept_draws++;
	kept_vertices += count;
	return entry;
}

/* ---------- the lit models' normals, decoded once

A draw lit by GX sends each vertex's normal, which the models' buffers hold
packed (11, 11 and 10 bits: D3DVSDT_NORMPACKED3), a form GX does not read:
it was decoded for every index of every lit draw (58,000 a frame in b30).
The models' buffers are static, so each one's normals are decoded once into
three floats a vertex (the same as lit_normal's, to the bit) beside it in
the contiguous window, and GX reads them by index as it reads the
positions. A buffer is keyed as the kept outputs key theirs (its header,
data, stride and serial, the normal's offset), and kept from the frame
after it is first seen, its vertices decoded from 0 to the most a draw
names; at most KEPT_NORMALS_BYTES in all, in the kept buffers' memory (an
entry refused room is not asked for again while kept_room is the same). An
entry not drawn for STATIC_OUTPUTS_IDLE frames is freed once GX has drawn
the frame; where a buffer finds the budget full, entries not drawn in this
frame or the last (models out of view: GX has drawn them) give theirs up at
once, the least recently drawn first. Normals of three floats in the buffer
GX reads where they are. b30 with the Pelican's marines wanted more than
768 KB (555 draws a second sent their normals), and the pool, 2560 KB, held
1584 KB: the budget is 1280 KB, the statistics saying what the level's
draws wanted. */
#define KEPT_NORMALS_ENTRIES 128
#define KEPT_NORMALS_WAYS 4
#define KEPT_NORMALS_BYTES (1280UL * 1024UL)

struct kept_normals
{
	const D3DVertexBuffer *buffer;
	DWORD data, serial;
	UINT stride, offset;
	unsigned long seen_frame, used_frame;
	/* (the kept_room its values were refused in) */
	unsigned long refused;
	/* vertices 0 to decoded - 1, three floats each; room for capacity */
	float *values;
	unsigned long decoded, capacity;
	/* (the capacity it was refused: the statistics' wanted) */
	unsigned long wanted;
};

static struct kept_normals kept_normals[KEPT_NORMALS_ENTRIES];
static unsigned long kept_normals_bytes;
/* in the statistics' window: lit draws whose normals GX read by index (from
the kept, or from their buffer), vertices decoded, draws without room */
static unsigned long kept_normal_draws, kept_normals_decoded, kept_normals_refused;
/* (and entries that gave up their room for another's) */
static unsigned long kept_normals_given;

/* an entry's values freed: once GX has drawn the frame (or now, past room
to wait) */
static void kept_normals_release(struct kept_normals *entry, BOOL drawn)
{
	if (entry->values)
	{
		if (drawn)
			kept_free(entry->values, entry->capacity * 12);
		else
			kept_free_later(entry->values, entry->capacity * 12);
		kept_normals_bytes -= entry->capacity * 12;
	}
	memset(entry, 0, sizeof(*entry));
}

static void kept_normals_frame_drawn(void)
{
	unsigned index;

	for (index = 0; index < KEPT_NORMALS_ENTRIES; index++)
		if (kept_normals[index].buffer && device.frames - kept_normals[index].used_frame > STATIC_OUTPUTS_IDLE)
			kept_normals_release(&kept_normals[index], TRUE);
}

/* the budget's room for bytes more, made by the entries not drawn in this
frame or the last (but keep), the least recently drawn first */
static void kept_normals_make_room(const struct kept_normals *keep, unsigned long bytes)
{
	while (kept_normals_bytes + bytes > KEPT_NORMALS_BYTES)
	{
		struct kept_normals *oldest = NULL;
		unsigned index;

		for (index = 0; index < KEPT_NORMALS_ENTRIES; index++)
		{
			struct kept_normals *candidate = &kept_normals[index];

			if (candidate != keep && candidate->values && candidate->used_frame + 2 <= device.frames &&
				(!oldest || candidate->used_frame < oldest->used_frame))
				oldest = candidate;
		}
		if (!oldest)
			return;
		kept_normals_release(oldest, TRUE);
		kept_normals_given++;
	}
}

/* the array GX reads a lit draw's normals from by index, vertices up to
most (lit.normals_array, lit.normals_array_stride); FALSE where the normals
are sent with each vertex (not static yet, no room) */
static BOOL kept_normals_find(const struct vertex_shader_object *shader, unsigned long least, unsigned long most)
{
	const struct vertex_element *normal = element_of(shader, 1);
	unsigned stream = normal->stream & (MAXIMUM_STREAMS - 1);
	D3DVertexBuffer *buffer = device.streams[stream].buffer;
	UINT stride = device.streams[stream].stride;
	struct kept_normals *entry = NULL, *victim = NULL;
	unsigned set, way;
	unsigned long vertex;

	if (!buffer || !buffer->Data || stride > 255)
		return FALSE;
	if (!lit.normal_packed)
	{
		/* (three floats: GX reads them where they are) */
		lit.normals_array = lit.normals;
		lit.normals_array_stride = stride;
		stream_flush(buffer, lit.normals - normal->offset + least * stride, (most - least + 1) * stride);
		kept_normal_draws++;
		return TRUE;
	}
	if (!buffer->Lock)
	{
		if (!++vertex_buffer_serial)
			vertex_buffer_serial++;
		buffer->Lock = vertex_buffer_serial;
	}
	set = (unsigned)((((unsigned long)buffer >> 4) * 2654435761UL) >> 8) % (KEPT_NORMALS_ENTRIES / KEPT_NORMALS_WAYS) *
		KEPT_NORMALS_WAYS;
	for (way = 0; way < KEPT_NORMALS_WAYS; way++)
	{
		struct kept_normals *candidate = &kept_normals[set + way];

		if (candidate->buffer == buffer && candidate->data == buffer->Data && candidate->serial == buffer->Lock &&
			candidate->stride == stride && candidate->offset == normal->offset)
		{
			entry = candidate;
			break;
		}
		if (!victim || (victim->buffer && (!candidate->buffer || candidate->used_frame < victim->used_frame)))
			victim = candidate;
	}
	if (!entry)
	{
		kept_normals_release(victim, FALSE);
		victim->buffer = buffer;
		victim->data = buffer->Data;
		victim->serial = buffer->Lock;
		victim->stride = stride;
		victim->offset = normal->offset;
		victim->seen_frame = victim->used_frame = device.frames;
		return FALSE;
	}
	entry->used_frame = device.frames;
	/* (not static yet: seen this frame only) */
	if (entry->seen_frame == device.frames)
		return FALSE;
	if (most >= entry->capacity)
	{
		unsigned long capacity = (most + 1024) & ~1023UL;
		float *values = NULL;

		/* (refused, and no room has come since) */
		if (entry->refused == kept_room)
		{
			kept_normals_refused++;
			return FALSE;
		}
		if (kept_normals_bytes - entry->capacity * 12 + capacity * 12 > KEPT_NORMALS_BYTES)
			kept_normals_make_room(entry, (capacity - entry->capacity) * 12);
		if (kept_normals_bytes - entry->capacity * 12 + capacity * 12 <= KEPT_NORMALS_BYTES)
			values = kept_alloc(capacity * 12);
		if (!values)
		{
			entry->refused = kept_room;
			entry->wanted = capacity;
			kept_normals_refused++;
			return FALSE;
		}
		entry->wanted = 0;
		if (entry->values)
		{
			memcpy(values, entry->values, entry->decoded * 12);
			DCFlushRange(values, entry->decoded * 12);
			kept_free_later(entry->values, entry->capacity * 12);
			kept_normals_bytes -= entry->capacity * 12;
		}
		entry->values = values;
		entry->capacity = capacity;
		kept_normals_bytes += capacity * 12;
	}
	if (most >= entry->decoded)
	{
		for (vertex = entry->decoded; vertex <= most; vertex++)
			lit_normal((UINT)vertex, entry->values + 3 * vertex);
		DCFlushRange(entry->values + 3 * entry->decoded, (most + 1 - entry->decoded) * 12);
		kept_normals_decoded += most + 1 - entry->decoded;
		entry->decoded = most + 1;
	}
	lit.normals_array = (const unsigned char *)entry->values;
	lit.normals_array_stride = 12;
	kept_normal_draws++;
	return TRUE;
}

/* ---------- the structure's indices as display lists

The structure's sections are drawn with the same indices each frame, and
their passes but for a lit or keyed one send only those indices: a position,
a normal and texture coordinates each read by index, the same index for
each (emit_indices' first loop). Such a draw's indices, once its index record
has hashed the same in two draws (draw_range.kept), are written once into a
GX display list (GX_BeginDispList: the commands as they would go into the
FIFO, in the kept buffers' memory), and the draws call it: three words in the
FIFO for the draw, the processor's work of its indices gone. GX's state
(the vertex descriptor and formats, the arrays, the matrices) is set
outside the list as for any draw: the list holds only the primitive and its
indices, so it serves any draw of the same primitive and number of indexed
attributes. A record keeps INDEX_LISTS lists (a section's passes differ in
their texture coordinates); they are freed with the record's indices (a
hash that differs, the record's place taken) and when not drawn for
STATIC_OUTPUTS_IDLE frames, once GX has drawn the frame; at most
INDEX_LISTS_BYTES in all. A record refused room for a list does not ask
again while kept_room is the same: its draws send their indices, as they
did before the lists. WiiLibs' wl_gx_dl_* records
its own layouts (one texture coordinate) through wl_gx_draw_indexed, so the
device records with libogc's calls, as WiiLibs does. */
#define INDEX_LISTS_BYTES (768UL * 1024UL)
/* (the lists' memory: draw_bench.c gives its own, outside the window whose
addresses its digests keep) */
#ifndef INDEX_LIST_ALLOCATE
#define INDEX_LIST_ALLOCATE(bytes) kept_alloc(bytes)
#define INDEX_LIST_FREE(data, bytes) kept_free((data), (bytes))
#endif

static unsigned long index_lists_bytes;
static struct
{
	void *data;
	unsigned long bytes;
} index_lists_frees[INDEX_RECORDS];
static unsigned index_lists_free_count;
/* in the statistics' window: draws by a list, lists written, and draws that
found no room for theirs */
static unsigned long index_list_calls, index_lists_made, index_lists_refused;

static void index_list_free(void *data, unsigned long bytes, BOOL drawn)
{
	if (drawn)
		INDEX_LIST_FREE(data, bytes);
	else if (index_lists_free_count < INDEX_RECORDS)
	{
		index_lists_frees[index_lists_free_count].data = data;
		index_lists_frees[index_lists_free_count].bytes = bytes;
		index_lists_free_count++;
	}
	else
	{
		/* (no room to wait: GX finishes first) */
		GX_DrawDone();
		xf_flush_after();
		INDEX_LIST_FREE(data, bytes);
	}
}

static void index_record_lists_release(struct index_record *record, BOOL drawn)
{
	unsigned which;

	for (which = 0; which < INDEX_LISTS; which++)
	{
		struct index_list *list = &record->lists[which];

		if (!list->data)
			continue;
		index_list_free(list->data, list->capacity, drawn);
		index_lists_bytes -= list->capacity;
		memset(list, 0, sizeof(*list));
	}
}

/* once GX has drawn the frame (Present): the lists pushed out, and those of
records not drawn for a while */
static void index_lists_frame_drawn(void)
{
	unsigned index, which;

	while (index_lists_free_count)
	{
		index_lists_free_count--;
		INDEX_LIST_FREE(index_lists_frees[index_lists_free_count].data, index_lists_frees[index_lists_free_count].bytes);
	}
	if (!index_lists_bytes)
		return;
	for (index = 0; index < INDEX_RECORDS; index++)
		if (device.frames - index_records[index].used > STATIC_OUTPUTS_IDLE)
			for (which = 0; which < INDEX_LISTS; which++)
				if (index_records[index].lists[which].data)
				{
					index_record_lists_release(&index_records[index], TRUE);
					break;
				}
}

/* the draw's indices by the record's display list for its primitive and
attributes (written now if it has none): FALSE where it has none and none
can be written (the draw then sends its indices) */
static BOOL index_list_draw(struct index_record *record, u8 primitive, unsigned attributes, const WORD *indices,
	UINT count)
{
	struct index_list *list = NULL, *empty = NULL;
	unsigned which;
	unsigned long capacity;
	void *data;
	u32 size;

	for (which = 0; which < INDEX_LISTS; which++)
	{
		struct index_list *candidate = &record->lists[which];

		if (candidate->data && candidate->primitive == primitive && candidate->attributes == attributes)
		{
			list = candidate;
			break;
		}
		if (!candidate->data && !empty)
			empty = candidate;
	}
	if (list)
	{
		gx_flush_pending(HARDWARE_VERTEX_FORMAT);
		GX_CallDispList(list->data, list->size);
		index_list_calls++;
		return TRUE;
	}
	/* (no room for another, or refused room and none has come since) */
	if (!empty || record->lists_refused == kept_room)
	{
		index_lists_refused++;
		return FALSE;
	}
	/* (the primitive's command and count, the indices, GX_EndDispList's
	padding to 32 bytes, and room for a flush GX_Begin may send first) */
	capacity = (3 + (unsigned long)count * attributes * 2 + 64 + 31) & ~31UL;
	data = index_lists_bytes + capacity <= INDEX_LISTS_BYTES ? INDEX_LIST_ALLOCATE(capacity) : NULL;
	if (!data)
	{
		record->lists_refused = kept_room;
		index_lists_refused++;
		return FALSE;
	}
	/* (the list is written past the processor's cache: no line of it may be
	written back over it) */
	DCInvalidateRange(data, capacity);
	gx_flush_pending(HARDWARE_VERTEX_FORMAT);
	GX_BeginDispList(data, capacity);
	GX_Begin(primitive, HARDWARE_VERTEX_FORMAT, (u16)count);
	emit_indices(indices, 0, count, attributes - 1, FALSE, EMIT_NORMALS_NONE, FALSE, FALSE, FALSE);
	GX_End();
	size = GX_EndDispList();
	if (!size)
	{
		INDEX_LIST_FREE(data, capacity);
		index_lists_refused++;
		return FALSE;
	}
	empty->data = data;
	empty->size = size;
	empty->capacity = (u32)capacity;
	empty->primitive = primitive;
	empty->attributes = (u8)attributes;
	index_lists_bytes += capacity;
	index_lists_made++;
	GX_CallDispList(data, size);
	index_list_calls++;
	return TRUE;
}

/* every kept buffer freed, for a block of the game's or the device's own
that found no room in the window (wii_memory.c calls it): on the device's
thread, and not while a draw reads them (kept_pinned); nonzero where
anything was freed */
static int kept_reclaim(void)
{
	unsigned index;
	unsigned long before = kept_pool_bytes;

	if (kept_pinned || !pthread_equal(pthread_self(), kept_thread) || !kept_pool_bytes)
		return 0;
	GX_DrawDone();
	xf_flush_after();
	while (kept_free_count)
	{
		kept_free_count--;
		kept_free(kept_frees[kept_free_count].data, kept_frees[kept_free_count].bytes);
	}
	for (index = 0; index < STATIC_OUTPUTS_ENTRIES; index++)
		if (static_outputs[index].used)
			static_outputs_release(&static_outputs[index], TRUE);
	for (index = 0; index < KEPT_NORMALS_ENTRIES; index++)
		if (kept_normals[index].buffer)
			kept_normals_release(&kept_normals[index], TRUE);
	while (index_lists_free_count)
	{
		index_lists_free_count--;
		INDEX_LIST_FREE(index_lists_frees[index_lists_free_count].data, index_lists_frees[index_lists_free_count].bytes);
	}
	for (index = 0; index < INDEX_RECORDS; index++)
	{
		index_record_lists_release(&index_records[index], TRUE);
		index_records[index].lists_refused = 0;
		index_records[index].kept_generation = 0;
		index_records[index].kept_values = NULL;
	}
	kept_room++;
	kept_reclaims++;
	return kept_pool_bytes < before;
}

/* (at the device's creation, on its thread) */
static void kept_start(void)
{
	kept_thread = pthread_self();
	wii_memory_window_set_reclaim(kept_reclaim);
}

/* ---------- the vertices of a draw in hardware the processor has a part in

Where the processor computes something of a draw's vertices (a blended
vertex's position, the colours, a sphere map's coordinates, the planar fog's
alpha) or sends what GX cannot read by index, each vertex's part is
computed once however many of the indices name it (a strip names most
twice), into the vertex memo, and each index then sends what its vertex
has. The loop that sends them is made for each common combination of what
is sent (hardware_emit's flags, constants there), so that an index costs
only what it sends: the general one tested each case at every index. */
enum
{
	/* each vertex's matrix indices, by its key (hardware_key_bytes): the
	position's, then one, two or three more */
	HW_KEYED = 1 << 0,
	HW_MATRIX2 = 1 << 1,
	HW_MATRIX3 = 1 << 2,
	HW_MATRIX4 = 1 << 3,
	/* every position sent (a draw with blended vertices): the processor's,
	or the vertex's own */
	HW_POSITIONS = 1 << 4,
	/* a normal, sent (the map's, or a blended vertex's) or by index */
	HW_LIT = 1 << 5,
	HW_NORMALS_INDEXED = 1 << 6,
	/* D0's alpha, the planar fog's (lit.alpha) */
	HW_ALPHA = 1 << 7,
	/* the colours: the processor's, kept (by index), or a pad of 0 */
	HW_CPU_COLORS = 1 << 8,
	HW_KEPT_COLORS = 1 << 9,
	HW_PAD = 1 << 10,
	/* the processor's sphere maps' coordinates */
	HW_SPHERES = 1 << 11
};
/* (what needs the vertex's computed part) */
#define HW_COMPUTED(flags) \
	(((flags) & (HW_POSITIONS | HW_ALPHA | HW_CPU_COLORS | HW_SPHERES)) || \
		((flags) & (HW_LIT | HW_NORMALS_INDEXED)) == HW_LIT)

struct hardware_vertices
{
	unsigned flags;
	struct nv2a_cpu_program *program;
	/* (fetch's elements for what the processor computes of a vertex) */
	struct stream_fetch *fetch;
	float (*inputs)[4];
	float point_size;
	BOOL cpu_colors;
	unsigned cpu_spheres;
	const WORD *indices;
	UINT first;
	unsigned long base, least, memo_size;
	const unsigned char *position_stream;
	UINT position_stride;
	unsigned texture_count, sphere_count;
	int sphere_list[4];
};

/* each key's matrix indices, in the order the vertex sends them: the first
in the word's top byte, so that one store sends two, or four */
static u32 hardware_key_words[256];
static struct hardware_vertices hardware_vertices_draw;
/* in the statistics' window: the draws and indices of each kind (flags),
the first HW_KINDS kinds seen */
#define HW_KINDS 16
static struct
{
	unsigned flags;
	unsigned long draws, indices;
} hardware_kinds[HW_KINDS];

/* a vertex's own position, normal and planar fog (not the processor's
vertex) */
static inline __attribute__((always_inline)) void hardware_vertex_own(const struct hardware_vertices *h,
	unsigned long whole, unsigned key, struct gx_vertex *computed, const unsigned flags)
{
	const unsigned char *position = h->position_stream + whole * h->position_stride;

	if (flags & (HW_POSITIONS | HW_ALPHA))
	{
		word_copy(&computed->position[0], position);
		word_copy(&computed->position[1], position + 4);
		word_copy(&computed->position[2], position + 8);
	}
	if ((flags & (HW_LIT | HW_NORMALS_INDEXED)) == HW_LIT)
		lit_normal(whole, computed->normal);
	if (flags & HW_ALPHA)
	{
		const float (*rows)[4] = (const float (*)[4])lit.world[(flags & HW_KEYED) ? key : 0];
		const float *model = computed->position;
		float world[3];
		int row;

		for (row = 0; row < 3; row++)
			world[row] = rows[row][0] * model[0] + rows[row][1] * model[1] + rows[row][2] * model[2] + rows[row][3];
		computed->alpha = color_byte(gx_lighting_planar_fog(&lit.lighting, world));
	}
}

/* what the processor computes of a vertex (its value whole, at index vertex,
of the key key): out, or the memo's entry, which it returns */
static struct gx_vertex *hardware_vertex_compute(const struct hardware_vertices *h, UINT vertex, unsigned long whole,
	unsigned key, struct gx_vertex *out)
{
	unsigned long slot = whole - h->least;
	struct gx_vertex *computed = slot < h->memo_size ? &vertex_memo[slot] : out;
	BOOL processor = key == KEY_PROCESSOR;
	unsigned flags = h->flags;
	int stage, component;

	if (processor || h->cpu_colors || h->cpu_spheres)
	{
		/* what the processor computes of the vertex: its position (in
		draw_run's (x w, y w, -w), through the identity) where it is the
		processor's, its colours and sphere maps' coordinates where they
		are */
		struct nv2a_cpu_output out_values;
		float (*inputs)[4] = h->inputs;

		stream_fetch(h->fetch, vertex, inputs);
		if (processor || h->cpu_spheres)
		{
			unsigned live = (processor ? NV2A_CPU_LIVE_POSITION : 0) |
				(h->cpu_colors ? NV2A_CPU_LIVE_D0 | NV2A_CPU_LIVE_D1 : 0) | h->cpu_spheres * NV2A_CPU_LIVE_T0;

			nv2a_cpu_run_live(h->program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs,
				h->point_size, &out_values, live);
			if (processor)
			{
				clip_position(out_values.clip, computed->position);
				processor_positions++;
			}
			for (stage = 0; stage < 4; stage++)
				if (h->cpu_spheres & (1U << stage))
					sphere_coordinates(out_values.t[stage], computed->texcoord[stage]);
			if (h->cpu_spheres)
				hardware_spheres_run++;
			if ((flags & HW_LIT) && processor)
			{
				/* (a lit draw's blended vertex: its normal and world position
				by its own blend of the nodes, as the program skins them) */
				const float (*constants)[4] = (const float (*)[4])device.vertex_constants + VERTEX_CONSTANT_BIAS;
				float blend[3][4], world[3];
				int row;

				gx_lighting_skin(constants, inputs[NODE_INDICES], inputs[NODE_WEIGHT], blend);
				for (row = 0; row < 3; row++)
				{
					computed->normal[row] = blend[row][0] * inputs[1][0] + blend[row][1] * inputs[1][1] +
						blend[row][2] * inputs[1][2];
					world[row] = blend[row][0] * inputs[0][0] + blend[row][1] * inputs[0][1] +
						blend[row][2] * inputs[0][2] + blend[row][3];
				}
				if (flags & HW_ALPHA)
					computed->color0[3] = color_byte(gx_lighting_planar_fog(&lit.lighting, world));
			}
		}
		else
			nv2a_cpu_run_colors(h->program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs,
				&out_values);
		if (h->cpu_colors)
		{
			for (component = 0; component < 4; component++)
			{
				computed->color0[component] = color_byte(out_values.d0[component]);
				computed->color1[component] = color_byte(out_values.d1[component]);
			}
			hardware_colors_run++;
		}
	}
	if (processor)
		computed->alpha = computed->color0[3];
	else
		hardware_vertex_own(h, whole, key, computed, flags);
	if (computed != out)
		vertex_memo_stamps[slot] = vertex_memo_draw;
	return computed;
}

/* each index's vertex sent: what flags (a constant where this is inlined)
says it has. What the loop reads of h, the memo and the keys is read once,
into registers: a store to GX's FIFO may alias any memory (the build has no
strict aliasing, and a byte's store aliases anything), so a field read at
each index would be loaded again at each. The FIFO takes bytes in order
whatever the store's width, so the matrix indices go as one store (two
bytes, or four), the planar fog's colour as one word, and the texture
coordinates' indices, all the vertex's own index, two to a word. Where
nothing of a vertex is computed but the processor's blended vertices'
positions (HW_OWN_DIRECT: b30's marines and Pelican, most of the indices
this sends), a vertex of a node's own sends its position and normal from its
buffer at each index, which costs less than the memo's look-up, copy and
stamp: the memo serves the processor's vertices alone. */
#define HW_OWN_DIRECT(flags) (!((flags) & (HW_ALPHA | HW_CPU_COLORS | HW_SPHERES)))
static inline __attribute__((always_inline)) void hardware_emit(const struct hardware_vertices *h, UINT count,
	const unsigned flags)
{
	const unsigned char *const positions = h->position_stream, *const normals = lit.normals;
	const UINT position_stride = h->position_stride, normal_stride = lit.normal_stride;
	const BOOL normal_packed = lit.normal_packed;
	const float *const elevens = packed_elevens, *const tens = packed_tens;
	const WORD *const indices = h->indices;
	const unsigned long base = indices ? h->base : h->first, least = h->least, memo_size = h->memo_size;
	struct gx_vertex *const memo = vertex_memo;
	unsigned long *const stamps = vertex_memo_stamps;
	const unsigned long stamp = vertex_memo_draw;
	const unsigned char *const keys = draw_key_of;
	const u32 *const key_words = hardware_key_words;
	const unsigned texture_pairs = h->texture_count >> 1, texture_odd = h->texture_count & 1;
	const unsigned sphere_count = (flags & HW_SPHERES) ? h->sphere_count : 0;
	UINT vertex;
	unsigned stage;

	for (vertex = 0; vertex < count; vertex++)
	{
		unsigned long whole = (indices ? (unsigned long)indices[vertex] : (unsigned long)vertex) + base;
		u16 index = (u16)whole;
		u32 pair = (u32)index << 16 | index;
		unsigned key = (flags & HW_KEYED) ? keys[vertex] : 0;
		const struct gx_vertex *computed = NULL;
		struct gx_vertex local;

		if (HW_COMPUTED(flags) && !(HW_OWN_DIRECT(flags) && key != KEY_PROCESSOR))
		{
			unsigned long slot = whole - least;

			if (slot < memo_size && stamps[slot] == stamp)
				computed = &memo[slot];
			else if (!(flags & (HW_CPU_COLORS | HW_SPHERES)) && key != KEY_PROCESSOR)
			{
				/* (the vertex's own alone: here) */
				struct gx_vertex *own = slot < memo_size ? &memo[slot] : &local;

				hardware_vertex_own(h, whole, key, own, flags);
				if (own != &local)
					stamps[slot] = stamp;
				computed = own;
			}
			else
				computed = hardware_vertex_compute(h, vertex, whole, key, &local);
		}
		if (flags & HW_KEYED)
		{
			u32 word = key_words[key];

			if (flags & HW_MATRIX4)
				fifo_u32(word);
			else if (flags & HW_MATRIX3)
			{
				fifo_u16((u16)(word >> 16));
				fifo_u8((u8)(word >> 8));
			}
			else if (flags & HW_MATRIX2)
				fifo_u16((u16)(word >> 16));
			else
				fifo_u8((u8)(word >> 24));
		}
		if ((flags & HW_POSITIONS) && HW_OWN_DIRECT(flags) && !computed)
		{
			/* (the vertex's own position, from its buffer) */
			const unsigned char *position = positions + whole * position_stride;
			u32 words[3];

			memcpy(words, position, sizeof(words));
			fifo_u32(words[0]);
			fifo_u32(words[1]);
			fifo_u32(words[2]);
		}
		else if (flags & HW_POSITIONS)
			hardware_words_send(computed->position);
		else if ((flags & (HW_LIT | HW_NORMALS_INDEXED)) == (HW_LIT | HW_NORMALS_INDEXED))
			fifo_u32(pair);
		else
			fifo_u16(index);
		if (flags & HW_LIT)
		{
			if (!(flags & HW_NORMALS_INDEXED) && HW_OWN_DIRECT(flags) && !computed)
			{
				/* (the vertex's own normal, as lit_normal reads it) */
				const unsigned char *source = normals + whole * normal_stride;
				u32 words[3];

				if (normal_packed)
				{
					u32 word;
					float values[3];

					memcpy(&word, source, sizeof(word));
					values[0] = elevens[word & 0x7FF];
					values[1] = elevens[(word >> 11) & 0x7FF];
					values[2] = tens[word >> 22];
					memcpy(words, values, sizeof(words));
				}
				else
					memcpy(words, source, sizeof(words));
				fifo_u32(words[0]);
				fifo_u32(words[1]);
				fifo_u32(words[2]);
			}
			else if (!(flags & HW_NORMALS_INDEXED))
				hardware_words_send(computed->normal);
			else if (flags & HW_POSITIONS)
				fifo_u16(index);
			if (flags & HW_ALPHA)
				fifo_u32(0xFFFFFF00u | computed->alpha);
		}
		if (flags & HW_CPU_COLORS)
		{
			/* (a colour's four bytes as one word: the same bytes in the FIFO) */
			fifo_u32(color_word(computed->color0));
			fifo_u32(color_word(computed->color1));
		}
		else if (flags & HW_KEPT_COLORS)
			fifo_u32(pair);
		else if (flags & HW_PAD)
			fifo_u16(0);
		if (texture_odd)
			fifo_u16(index);
		for (stage = 0; stage < texture_pairs; stage++)
			fifo_u32(pair);
		if (flags & HW_SPHERES)
			for (stage = 0; stage < sphere_count; stage++)
				GX_TexCoord2f32(computed->texcoord[h->sphere_list[stage]][0], computed->texcoord[h->sphere_list[stage]][1]);
	}
}

/* the draw's vertices sent, by the loop made for its flags */
static void hardware_emit_draw(const struct hardware_vertices *h, UINT count)
{
	unsigned kind;

	for (kind = 0; kind < HW_KINDS && hardware_kinds[kind].draws && hardware_kinds[kind].flags != h->flags; kind++)
		;
	if (kind < HW_KINDS)
	{
		hardware_kinds[kind].flags = h->flags;
		hardware_kinds[kind].draws++;
		hardware_kinds[kind].indices += count;
	}
#define HW_EMIT(flags) \
	case (flags): \
		hardware_emit(h, count, (flags)); \
		break;

	switch (h->flags)
	{
	/* (a model's blended vertices, lit: with a reflection, the planar fog) */
	HW_EMIT(HW_KEYED | HW_POSITIONS | HW_LIT)
	HW_EMIT(HW_KEYED | HW_MATRIX2 | HW_POSITIONS | HW_LIT)
	HW_EMIT(HW_KEYED | HW_POSITIONS | HW_LIT | HW_ALPHA)
	HW_EMIT(HW_KEYED | HW_MATRIX2 | HW_POSITIONS | HW_LIT | HW_ALPHA)
	/* (and with two of the reflection, the fog's and the planar fog
	ramp's matrices: b30's marines and Pelican) */
	HW_EMIT(HW_KEYED | HW_MATRIX2 | HW_MATRIX3 | HW_POSITIONS | HW_LIT)
	HW_EMIT(HW_KEYED | HW_MATRIX2 | HW_MATRIX3 | HW_POSITIONS | HW_LIT | HW_ALPHA)
	/* (a model lit, its planar fog the processor's) */
	HW_EMIT(HW_KEYED | HW_LIT | HW_ALPHA)
	HW_EMIT(HW_KEYED | HW_MATRIX2 | HW_LIT | HW_ALPHA)
	HW_EMIT(HW_KEYED | HW_MATRIX2 | HW_MATRIX3 | HW_LIT | HW_ALPHA)
	HW_EMIT(HW_KEYED | HW_LIT | HW_NORMALS_INDEXED | HW_ALPHA)
	HW_EMIT(HW_KEYED | HW_MATRIX2 | HW_LIT | HW_NORMALS_INDEXED | HW_ALPHA)
	HW_EMIT(HW_KEYED | HW_MATRIX2 | HW_MATRIX3 | HW_LIT | HW_NORMALS_INDEXED | HW_ALPHA)
	HW_EMIT(HW_LIT | HW_ALPHA)
	HW_EMIT(HW_LIT | HW_NORMALS_INDEXED | HW_ALPHA)
	/* (unlit: the processor's colours, blended vertices, a pad) */
	HW_EMIT(HW_CPU_COLORS)
	HW_EMIT(HW_KEYED | HW_CPU_COLORS)
	HW_EMIT(HW_KEYED | HW_POSITIONS)
	HW_EMIT(HW_KEYED | HW_POSITIONS | HW_CPU_COLORS)
	HW_EMIT(HW_KEYED | HW_POSITIONS | HW_PAD)
	HW_EMIT(HW_KEYED | HW_PAD)
	HW_EMIT(HW_KEYED | HW_MATRIX2 | HW_PAD)
	HW_EMIT(HW_KEPT_COLORS)
	/* (and with the processor's sphere maps' coordinates) */
	HW_EMIT(HW_SPHERES)
	HW_EMIT(HW_CPU_COLORS | HW_SPHERES)
	HW_EMIT(HW_KEYED | HW_CPU_COLORS | HW_SPHERES)
	HW_EMIT(HW_KEYED | HW_POSITIONS | HW_CPU_COLORS | HW_SPHERES)
	HW_EMIT(HW_KEYED | HW_POSITIONS | HW_SPHERES)
	default:
		hardware_emit(h, count, h->flags);
		break;
	}
#undef HW_EMIT
}

static BOOL draw_hardware(struct nv2a_cpu_program *program, D3DPRIMITIVETYPE primitive_type, UINT count,
	const WORD *indices, UINT first, struct stream_fetch *fetch)
{
	struct vertex_shader_object *shader = (struct vertex_shader_object *)fetch->shader;
	const struct nv2a_cpu_dependencies *dependencies;
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	struct nv2a_cpu_output base, column;
	const struct vertex_element *position_element;
	/* each stage's source: -1 the position, else the GX texture attribute */
	int stage_source[4], texture_registers[4], texture_count = 0, position_register, stage, component;
	BOOL stage_unread[4];
	/* the sphere-mapped stages whose direction the vertices give: their
	coordinates the processor's, sent with each vertex */
	unsigned cpu_spheres = 0, live;
	unsigned long clip_mask, least = 0xFFFFFFFFUL, most = 0, vertex, color_mask[2];
	float stage_rows[5][2][4], point_size;
	struct draw_projection projection;
	/* (position_read: a stage reads a coordinate GX makes from the position,
	which a vertex the processor transforms does not give it) */
	BOOL fogged = FALSE, cpu_colors = FALSE, position_read = FALSE, keyed, lit_on, pad_color = FALSE, plain;
	/* (the fog through each vertex's position matrix: fog_post_matrix) */
	BOOL fog_post = FALSE;
	Mtx fog_post_rows;
	/* (what a static buffer's kept outputs give of what the processor would
	compute: static_outputs_find) */
	struct static_outputs *kept = NULL;
	unsigned kept_spheres = 0, kept_count = 0;
	BOOL kept_colors = FALSE;
	/* (the setup's parts' ends: timing.setup_*) */
	uint64_t keys_done, matrices_done, lit_done, kept_done;
	const unsigned char *position_stream;
	UINT position_stride;
	unsigned long long key;
	struct hardware_matrices *cached;
	/* (fetch's elements for what the processor computes of a vertex) */
	struct stream_fetch vertex_fetch;
	int sphere_list[4], sphere_count = 0;
	uint64_t started = timing_now(), emitting;
	float fog_base = 0.0f, fog_slope = 0.0f;

	if (gpu_transform < 0)
		gpu_transform = config_boolean("debug.gpu_transform") ? 1 : 0;
	inputs_seed(inputs);
	dependencies = draw_dependencies(shader, program, fetch, count);
	if (!gpu_transform || !dependencies->clip_captured || primitive_type == D3DPT_POINTLIST)
		return hardware_rejected(program, "transform off, no clip-space position, or points");
	if (vertex_terms != _gx_psh_vertices_program)
		return hardware_rejected(program, "a pass's own vertex terms (a bumped reflection's, a specular light's)");
	clip_mask = dependencies->clip[0] | dependencies->clip[1] | dependencies->clip[3];
	position_register = single_register(clip_mask);
	if ((clip_mask & NV2A_CPU_NOT_AFFINE) || position_register < 0)
		return hardware_rejected(program, "the clip-space position is not affine in one register");
	position_element = element_of(shader, position_register);
	if (!position_element || position_element->type != D3DVSDT_FLOAT3 ||
		!fetch->streams[position_element->stream & (MAXIMUM_STREAMS - 1)])
		return hardware_rejected(program, "the position is not three floats in a stream");
	for (stage = 0; stage < 4; stage++)
	{
		unsigned long mask = dependencies->t[stage][0] | dependencies->t[stage][1];
		int reg = single_register(mask), index;
		const struct vertex_element *element;

		stage_source[stage] = -1;
		stage_unread[stage] = !(tev_texcoords & (1U << stage));
		if (stage_unread[stage])
			continue;
		if (sphere_stages & (1U << stage))
		{
			/* (a direction the vertices give is the processor's, its sphere
			map's coordinates sent with the vertex as the processor's draws
			send them; one the same for every vertex is one point of the
			sphere map) */
			if ((dependencies->t[stage][0] | dependencies->t[stage][1] | dependencies->t[stage][2]) & 0xFFFF)
			{
				if (!program->native)
					return hardware_rejected(program, "a cube map's direction, on the interpreter");
				cpu_spheres |= 1U << stage;
			}
			continue;
		}
		if (mask & NV2A_CPU_NOT_AFFINE)
			return hardware_rejected(program, "a texture coordinate read is not affine");
		if (!(mask & 0xFFFF))
			continue;
		if (reg == position_register)
		{
			position_read = TRUE;
			continue;
		}
		element = reg >= 0 ? element_of(shader, reg) : NULL;
		if (!element || (element->type != D3DVSDT_FLOAT2 && element->type != D3DVSDT_NORMSHORT2) ||
			!fetch->streams[element->stream & (MAXIMUM_STREAMS - 1)])
			return hardware_rejected(program, "a texture coordinate from a register GX cannot read");
		for (index = 0; index < texture_count && texture_registers[index] != reg; index++)
			;
		if (index == texture_count)
			texture_registers[texture_count++] = reg;
		stage_source[stage] = index;
	}
	for (component = 0; component < 2; component++)
	{
		const unsigned long *color = component ? dependencies->d1 : dependencies->d0;

		color_mask[component] = color[0] | color[1] | color[2] | color[3];
		if (color_mask[component] && (tev_channels & (1U << component)))
		{
			/* (the processor's: the program's colour-only function, a
			vertex at a time) */
			if (!program->native_colors)
				return hardware_rejected(program, "a colour read depends on the vertices, on the interpreter");
			cpu_colors = TRUE;
		}
	}
	if (dependencies->fog && (tev_texcoords & (1U << GX_PSH_FOG_TEXCOORD)))
	{
		DWORD mode = D3D__RenderState[D3DRS_FOGTABLEMODE];

		if ((dependencies->fog & NV2A_CPU_NOT_AFFINE) || single_register(dependencies->fog) != position_register ||
			(mode != D3DFOG_NONE && mode != D3DFOG_LINEAR))
			return hardware_rejected(program, "the fog is not affine in the position, or its table is exponential");
		fogged = TRUE;
	}
	/* (with several values of the inputs, what is read besides the position
	and the colours, which the processor gives, may not depend on them: one
	texture matrix serves every vertex) */
	keyed = draw_keyed();
	if (keyed)
	{
		const struct nv2a_cpu_dependencies *general = &shader->dependencies;

		for (stage = 0; stage < 4; stage++)
			if (!stage_unread[stage] && !(cpu_spheres & (1U << stage)) &&
				((general->t[stage][0] | general->t[stage][1]) & draw_uniform.mask))
				return hardware_rejected(program, "a texture coordinate from a model's nodes");
		/* (the fog from the nodes, or with the processor's vertices: through
		each vertex's position matrix, fog_post_matrix) */
		if (fogged && ((general->fog & draw_uniform.mask) || draw_keys.processor))
			fog_post = TRUE;
		for (component = 0; component < 2; component++)
		{
			const unsigned long *color = component ? general->d1 : general->d0;

			if (((color[0] | color[1] | color[2] | color[3]) & draw_uniform.mask) &&
				(tev_channels & (1U << component)))
			{
				if (!program->native_colors)
					return hardware_rejected(program, "a colour from a model's nodes, on the interpreter");
				cpu_colors = TRUE;
			}
		}
	}
	if (draw_keys.processor && position_read)
		return hardware_rejected(program, "a texture coordinate from the position, with blended vertices");
	/* (the colours and reflection GX's lights and normals can make:
	display.gx_lighting) */
	lit_on = lit_choose(program, shader, fetch, cpu_colors, cpu_spheres);
	/* the vertices' range: GX's indices are 16 bits */
	draw_range_find(fetch, count);
	least = draw_range.least;
	most = draw_range.most;
	if (most > 0xFFFF)
		return hardware_rejected(program, "an index past 16 bits");
	keys_done = timing_now();

	/* the matrices, as an earlier draw of the same program, constants and
	state made them, or else: the outputs for no input, then for each
	component alone */
	memcpy(&point_size, &D3D__RenderState[D3DRS_POINTSIZE], sizeof(point_size));
	key = hardware_key(shader, program);
	cached = &hardware_cache[key % HARDWARE_CACHE_ENTRIES];
	if (cached->key == key)
	{
		memcpy(stage_rows, cached->stage_rows, sizeof(stage_rows));
		memcpy(position_matrix_rows, cached->position, sizeof(position_matrix_rows));
		memcpy(base.d0, cached->d0, sizeof(base.d0));
		memcpy(base.d1, cached->d1, sizeof(base.d1));
		projection = cached->projection;
		hardware_cache_hits++;
	}
	else
	{
	/* (the outputs read of the program's runs alone, which
	nv2a_cpu_run_live gives as nv2a_cpu_run does: the texture coordinates
	the stages read and the fog, and for no input the colours) */
	unsigned columns_live = (tev_texcoords & 15) * NV2A_CPU_LIVE_T0 | NV2A_CPU_LIVE_FOG;

	matrix_inputs(shader, inputs);
	nv2a_cpu_run_live(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size, &base,
		columns_live | NV2A_CPU_LIVE_D0 | NV2A_CPU_LIVE_D1);
	for (component = 0; component < 4; component++)
	{
		if (component < 3)
		{
			inputs[position_register][component] = 1.0f;
			nv2a_cpu_run_live(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size,
				&column, columns_live);
			inputs[position_register][component] = 0.0f;
			for (stage = 0; stage < 4; stage++)
			{
				stage_rows[stage][0][component] = stage_source[stage] < 0 ? column.t[stage][0] - base.t[stage][0] : 0.0f;
				stage_rows[stage][1][component] = stage_source[stage] < 0 ? column.t[stage][1] - base.t[stage][1] : 0.0f;
			}
			stage_rows[4][0][component] = fogged ? column.fog - base.fog : 0.0f;
		}
		else
		{
			for (stage = 0; stage < 4; stage++)
			{
				stage_rows[stage][0][3] = base.t[stage][0];
				stage_rows[stage][1][3] = base.t[stage][1];
			}
			stage_rows[4][0][3] = fogged ? base.fog : 0.0f;
		}
	}
	/* a texture coordinate from a register of its own: (s, t, 1, 1) in */
	for (stage = 0; stage < 4; stage++)
	{
		int s_t;

		if (stage_source[stage] < 0)
			continue;
		int which;

		for (which = 0; which < 2; which++)
		{
			inputs[texture_registers[stage_source[stage]]][which] = 1.0f;
			nv2a_cpu_run_live(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs,
				point_size, &column, columns_live);
			inputs[texture_registers[stage_source[stage]]][which] = 0.0f;
			for (s_t = 0; s_t < 2; s_t++)
			{
				/* (GX reads a normalized short as n / 32768, Direct3D n / 32767) */
				stage_rows[stage][s_t][which] = (column.t[stage][s_t] - base.t[stage][s_t]) *
					(element_of(shader, texture_registers[stage_source[stage]])->type == D3DVSDT_NORMSHORT2 ?
						32768.0f / 32767.0f : 1.0f);
				stage_rows[stage][s_t][2] = 0.0f;
			}
		}
	}
	for (stage = 0; stage < 4; stage++)
	{
		if (stage_unread[stage])
			memset(stage_rows[stage], 0, sizeof(stage_rows[stage]));
		else if (sphere_stages & (1U << stage))
		{
			float point[2];

			sphere_coordinates(base.t[stage], point);
			memset(stage_rows[stage], 0, sizeof(stage_rows[stage]));
			stage_rows[stage][0][3] = point[0];
			stage_rows[stage][1][3] = point[1];
			continue;
		}
		for (component = 0; component < 4; component++)
		{
			stage_rows[stage][0][component] *= texcoord_scale[stage][0];
			stage_rows[stage][1][component] *= texcoord_scale[stage][1];
		}
	}
	if (fogged)
	{
		/* (fog_coordinate is affine in the factor below its clamps, which the
		ramp's edges keep) */
		fog_base = fog_coordinate(0.0f);
		fog_slope = fog_coordinate(1.0f) - fog_base;
		for (component = 0; component < 4; component++)
			stage_rows[4][0][component] *= fog_slope;
		stage_rows[4][0][3] += fog_base;
	}
	else
	{
		memset(stage_rows[4], 0, sizeof(stage_rows[4]));
		stage_rows[4][0][3] = fog_coordinate(base.fog);
	}
	memset(stage_rows[4][1], 0, sizeof(stage_rows[4][1]));

	/* (the position's matrix and depth as draw_run makes them, to the bit) */
	if (!position_matrix(shader, program, position_register, point_size, TRUE))
		return hardware_rejected(program, "no depth to fit (every w the same)");
	projection = position_projection;
	{
		cached->key = key;
		memcpy(cached->stage_rows, stage_rows, sizeof(stage_rows));
		memcpy(cached->position, position_matrix_rows, sizeof(position_matrix_rows));
		memcpy(cached->d0, base.d0, sizeof(base.d0));
		memcpy(cached->d1, base.d1, sizeof(base.d1));
		cached->projection = projection;
	}
	}
	projection_load(&projection);

	/* GX's state for the game's vertices (each value's matrix: the first's
	is the cache's) */
	if (keyed && !position_keys(shader, program, fetch, position_register, point_size))
		return hardware_rejected(program, "no depth to fit for a model's node");
	matrices_done = timing_now();
	if (lit_on && !lit_prepare(shader, fetch, position_register, keyed, 0))
	{
		lit_on = FALSE;
		lit_refused++;
	}
	/* (the normals by index, where no vertex sends its own: the processor's
	blended ones) */
	/* (from here the draw reads the kept buffers: kept_reclaim leaves them) */
	kept_pinned = TRUE;
	lit.normals_indexed = lit_on && !draw_keys.processor && kept_normals_find(shader, least, most);
	lit_done = timing_now();
	if (lit_on)
	{
		if (lit.colors)
			cpu_colors = FALSE;
		if (lit.reflection)
			cpu_spheres &= ~8U;
	}
	if (fog_post)
	{
		const Mtx *key_rows = lit_on ? (const Mtx *)lit.rows : (const Mtx *)draw_keys.rows;

		if (!fog_post_matrix((const float (*)[4])key_rows[0], stage_rows[4][0], fog_post_rows) ||
			!fog_post_check((const float (*)[4])fog_post_rows, key_rows, program, fetch, position_register, point_size))
			return hardware_rejected(program, "the fog from a model's nodes, not one function of its matrices' results");
	}
	if (!keyed && !lit_on && (cpu_spheres || cpu_colors))
	{
		kept = static_outputs_find(program, shader, fetch, count, indices, first, most, cpu_spheres, cpu_colors,
			point_size);
		if (kept)
		{
			kept_spheres = kept->stages;
			kept_colors = kept->colors;
			cpu_spheres &= ~kept_spheres;
			if (kept_colors)
				cpu_colors = FALSE;
			for (stage = 0; stage < 4; stage++)
				if (kept_spheres & (1U << stage))
					kept_count++;
		}
	}
	kept_done = timing_now();
	position_stream = fetch->streams[position_element->stream & (MAXIMUM_STREAMS - 1)] + position_element->offset;
	position_stride = device.streams[position_element->stream & (MAXIMUM_STREAMS - 1)].stride;
	if (position_stride > 255)
		return FALSE;
	GX_ClearVtxDesc();
	position_matrix_load();
	vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	if (draw_keys.processor)
	{
		/* (each vertex's position sent: the processor's, or the vertex's own
		through its node's matrix) */
		GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	}
	else
	{
		GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
		GX_SetArray(GX_VA_POS, (void *)position_stream, (u8)position_stride);
		stream_flush(device.streams[position_element->stream & (MAXIMUM_STREAMS - 1)].buffer,
			position_stream - position_element->offset + least * position_stride, (most - least + 1) * position_stride);
	}
	for (component = 0; component < texture_count; component++)
	{
		const struct vertex_element *element = element_of(shader, texture_registers[component]);
		const unsigned char *stream = fetch->streams[element->stream & (MAXIMUM_STREAMS - 1)];
		UINT stride = device.streams[element->stream & (MAXIMUM_STREAMS - 1)].stride;

		if (stride > 255)
			return FALSE;
		GX_SetVtxDesc(GX_VA_TEX0 + component, GX_INDEX16);
		if (element->type == D3DVSDT_NORMSHORT2)
			vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_TEX0 + component, GX_TEX_ST, GX_S16, 15);
		else
			vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_TEX0 + component, GX_TEX_ST, GX_F32, 0);
		GX_SetArray(GX_VA_TEX0 + component, (void *)(stream + element->offset), (u8)stride);
		stream_flush(device.streams[element->stream & (MAXIMUM_STREAMS - 1)].buffer, stream + least * stride,
			(most - least + 1) * stride);
	}
	/* (the kept sphere map coordinates, read by index from the kept values,
	then the processor's, sent with each vertex) */
	for (stage = 0, component = texture_count; stage < 4; stage++)
	{
		if (!(kept_spheres & (1U << stage)))
			continue;
		GX_SetVtxDesc(GX_VA_TEX0 + component, GX_INDEX16);
		vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_TEX0 + component, GX_TEX_ST, GX_F32, 0);
		GX_SetArray(GX_VA_TEX0 + component, kept->values + 8 * (component - texture_count), (u8)kept->stride);
		component++;
	}
	for (stage = 0; stage < 4; stage++)
	{
		if (!(cpu_spheres & (1U << stage)))
			continue;
		GX_SetVtxDesc(GX_VA_TEX0 + component, GX_DIRECT);
		vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_TEX0 + component, GX_TEX_ST, GX_F32, 0);
		component++;
	}
	GX_InvVtxCache();
	for (stage = 0; stage < 5; stage++)
	{
		Mtx rows;

		memset(rows, 0, sizeof(rows));
		memcpy(rows, stage_rows[stage], sizeof(stage_rows[stage]));
		GX_LoadTexMtxImm(rows, STAGE_TEXMTX(stage), GX_MTX2x4);
		GX_SetTexCoordGen(GX_TEXCOORD0 + stage, GX_TG_MTX2x4,
			stage < 4 && stage_source[stage] >= 0 ? GX_TG_TEX0 + stage_source[stage] : GX_TG_POS, STAGE_TEXMTX(stage));
	}
	/* (as transform_software sends the processor's coordinates; the kept
	ones first, as their attributes are) */
	for (stage = 0, component = texture_count; stage < 4; stage++)
	{
		if (!(kept_spheres & (1U << stage)))
			continue;
		GX_SetTexCoordGen(GX_TEXCOORD0 + stage, GX_TG_MTX2x4, GX_TG_TEX0 + component, GX_IDENTITY);
		component++;
	}
	for (stage = 0; stage < 4; stage++)
	{
		if (!(cpu_spheres & (1U << stage)))
			continue;
		GX_SetTexCoordGen(GX_TEXCOORD0 + stage, GX_TG_MTX2x4, GX_TG_TEX0 + component, GX_IDENTITY);
		component++;
	}
	if (cpu_spheres)
	{
		hardware_sphere_draws++;
		if (lit_on)
			hardware_sphere_lit_draws++;
	}
	/* (only the coordinates and channels the stages read: each coordinate
	generated costs GX's transform a few cycles a vertex; transform_software
	takes all of them back, a lit draw's planar fog its sixth) */
	{
		unsigned coordinates = 0, channels = (tev_channels & 2) ? 2 : (tev_channels & 1) ? 1 : 0;

		while (coordinates < 5 && (tev_texcoords >> coordinates))
			coordinates++;
		GX_SetNumTexGens(coordinates || channels ? coordinates : 1);
		GX_SetNumChans((u8)channels);
	}
	if (lit_on)
		lit_state(keyed, &projection, base.d1);
	if (fog_post)
	{
		/* (texture coordinate 4: the position through the vertex's own matrix,
		then the fog's row; q 1) */
		GX_LoadTexMtxImm(fog_post_rows, GX_DTTMTX1, GX_MTX3x4);
		GX_SetTexCoordGen2(GX_TEXCOORD0 + GX_PSH_FOG_TEXCOORD, GX_TG_MTX3x4, GX_TG_POS, GX_PNMTX1, GX_FALSE, GX_DTTMTX1);
		GX_SetVtxDesc(GX_VA_TEX0MTXIDX + GX_PSH_FOG_TEXCOORD, GX_DIRECT);
		fog_post_draws++;
	}
	if (lit_on && lit.colors)
	{
		/* (the colours: GX's lights, lit_state) */
	}
	else if (cpu_colors)
	{
		/* the colours: the processor's, sent with each vertex */
		GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
		vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
		vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
		GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	}
	else if (kept_colors)
	{
		/* the colours: kept, read by index (after the kept coordinates) */
		GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
		GX_SetVtxDesc(GX_VA_CLR1, GX_INDEX16);
		vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
		vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
		GX_SetArray(GX_VA_CLR0, kept->values + 8 * kept_count, (u8)kept->stride);
		GX_SetArray(GX_VA_CLR1, kept->values + 8 * kept_count + 4, (u8)kept->stride);
		GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	}
	else
	{
		/* the colours: constant (and where the vertex would otherwise be its
		position and matrix indices alone, which GX misreads, a colour of 0
		that neither channel reads goes with it: vertex_shape_check) */
		pad_color = keyed && !texture_count && !cpu_spheres && !kept_spheres && !lit_on;
		if (pad_color)
		{
			GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
			vertex_format(HARDWARE_VERTEX_FORMAT, GX_VA_CLR0, GX_CLR_RGB, GX_RGB565, 0);
		}
		GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){ color_byte(base.d0[0]), color_byte(base.d0[1]),
			color_byte(base.d0[2]), color_byte(base.d0[3]) });
		GX_SetChanMatColor(GX_COLOR1A1, (GXColor){ color_byte(base.d1[0]), color_byte(base.d1[1]),
			color_byte(base.d1[2]), color_byte(base.d1[3]) });
	}
	/* (nothing of the vertices the processor's: emit_indices) */
	plain = !cpu_colors && !cpu_spheres && !draw_keys.processor && !(lit_on && lit.alpha) && !kept_colors && !pad_color;
	if (!plain)
	{
		struct hardware_vertices *h = &hardware_vertices_draw;
		unsigned flags = 0;

		if (keyed)
		{
			unsigned extra = (lit_on && lit.reflection) + fog_post + (lit_on && lit.fog_ramped);

			flags |= HW_KEYED | (extra > 0 ? HW_MATRIX2 : 0) | (extra > 1 ? HW_MATRIX3 : 0) | (extra > 2 ? HW_MATRIX4 : 0);
		}
		if (draw_keys.processor)
			flags |= HW_POSITIONS;
		if (lit_on)
			flags |= HW_LIT | (lit.normals_indexed ? HW_NORMALS_INDEXED : 0) | (lit.alpha ? HW_ALPHA : 0);
		flags |= cpu_colors ? HW_CPU_COLORS : kept_colors ? HW_KEPT_COLORS : pad_color ? HW_PAD : 0;
		if (cpu_spheres)
			flags |= HW_SPHERES;
		if (keyed)
		{
			/* each key's matrix indices, and the processor's vertices' */
			unsigned key;

			for (key = 0; key <= draw_keys.count; key++)
			{
				unsigned which = key < draw_keys.count ? key : KEY_PROCESSOR;
				u8 position = (u8)(which == KEY_PROCESSOR ? GX_PNMTX0 : GX_PNMTX1 + 3 * key), bytes[4] = { 0, 0, 0, 0 };
				unsigned count_bytes = 1;

				bytes[0] = position;
				if (lit_on && lit.reflection)
					bytes[count_bytes++] = lit_sphere_rows[key];
				if (fog_post)
					bytes[count_bytes++] = position;
				if (lit_on && lit.fog_ramped)
					bytes[count_bytes++] = position;
				hardware_key_words[which] = (u32)bytes[0] << 24 | (u32)bytes[1] << 16 | (u32)bytes[2] << 8 | bytes[3];
			}
		}
		h->flags = flags;
		h->program = program;
		h->inputs = inputs;
		h->point_size = point_size;
		h->cpu_colors = cpu_colors;
		h->cpu_spheres = cpu_spheres;
		h->indices = indices;
		h->first = first;
		h->base = device.base_vertex_index;
		h->least = least;
		h->memo_size = 0;
		h->position_stream = position_stream;
		h->position_stride = position_stride;
		h->texture_count = texture_count + kept_count;
		h->sphere_count = 0;
		for (stage = 0; stage < 4; stage++)
			if (cpu_spheres & (1U << stage))
				h->sphere_list[h->sphere_count++] = stage;
		if (HW_COMPUTED(flags))
		{
			/* (what the processor computes of a vertex, once however many of
			the indices name it) */
			if (!vertex_memo)
			{
				vertex_memo = malloc(VERTEX_MEMO_SIZE * sizeof(*vertex_memo));
				vertex_memo_stamps = calloc(VERTEX_MEMO_SIZE, sizeof(*vertex_memo_stamps));
				if (!vertex_memo || !vertex_memo_stamps)
				{
					free(vertex_memo);
					free(vertex_memo_stamps);
					vertex_memo = NULL;
					vertex_memo_stamps = NULL;
				}
			}
			if (vertex_memo)
				h->memo_size = VERTEX_MEMO_SIZE;
			vertex_memo_draw++;
			/* (the registers what the processor computes reads, alone: its
			fetch's elements for the outputs it runs the program for, and a lit
			draw's blended vertices' position, normal, node indices and weight) */
			{
				const struct nv2a_cpu_dependencies *general = &shader->dependencies;
				unsigned long reads = 0;
				unsigned element;

				if (draw_keys.processor)
					reads |= nv2a_cpu_live_inputs(general, NV2A_CPU_LIVE_POSITION) |
						(lit_on ? 1UL << 0 | 1UL << 1 | 1UL << NODE_INDICES | 1UL << NODE_WEIGHT : 0);
				if (cpu_colors)
					reads |= nv2a_cpu_live_inputs(general, NV2A_CPU_LIVE_D0 | NV2A_CPU_LIVE_D1 | NV2A_CPU_LIVE_FOG);
				reads |= nv2a_cpu_live_inputs(general, cpu_spheres * NV2A_CPU_LIVE_T0);
				vertex_fetch = *fetch;
				vertex_fetch.element_count = 0;
				for (element = 0; element < fetch->element_count; element++)
					if (reads & (1UL << fetch->elements[element].reg))
						vertex_fetch.elements[vertex_fetch.element_count++] = fetch->elements[element];
			}
		}
		h->fetch = &vertex_fetch;
	}

	emitting = timing_now();
	if (index_lists < 0)
		index_lists = config_boolean("debug.gx_display_lists") ? 1 : 0;
	if (plain && !keyed && indices && index_lists && draw_range.kept && draw_range.serial == draw_serial &&
		draw_range.record && (!lit_on || lit.normals_indexed) &&
		index_list_draw(draw_range.record, gx_primitive(primitive_type),
			1 + (lit_on ? 1 : 0) + texture_count + kept_count, indices, count))
		;
	else
	{
		gx_begin(gx_primitive(primitive_type), HARDWARE_VERTEX_FORMAT, (u16)count);
		if (plain)
			emit_indices(indices, first, count, texture_count + kept_count, keyed,
				!lit_on ? EMIT_NORMALS_NONE : lit.normals_indexed ? EMIT_NORMALS_INDEXED : EMIT_NORMALS_SENT,
				lit_on && lit.reflection && keyed, fog_post, lit_on && lit.fog_ramped && keyed);
		else
			hardware_emit_draw(&hardware_vertices_draw, count);
		GX_End();
	}
	if (lit_on)
	{
		lit_draws++;
		lit_vertices += count;
		/* (the texture coordinates the other draws make) */
		if (lit.fog_ramped)
			GX_SetNumTexGens(5);
		if (draw_keys.processor)
		{
			/* (GX_PNMTX0 the identity again, as the processor's draws take it) */
			Mtx identity;

			guMtxIdentity(identity);
			GX_LoadPosMtxImm(identity, GX_PNMTX0);
		}
	}
	hardware_draws++;
	hardware_vertices += count;
	if (keyed)
		hardware_keyed_draws++;
	if (draw_keys.processor)
		hardware_blended_draws++;
	timing.hardware_setup += emitting - started;
	timing.hardware_vertices += timing_now() - emitting;
	timing.setup_keys += keys_done - started;
	timing.setup_matrices += matrices_done - keys_done;
	timing.setup_lit += lit_done - matrices_done;
	timing.setup_kept += kept_done - lit_done;
	timing.setup_state += emitting - kept_done;
	timing.setup_draws++;
	return TRUE;
}

static void draw_streams(D3DPRIMITIVETYPE primitive_type, UINT count, const WORD *indices, UINT first)
{
	struct nv2a_cpu_program *program = program_current();
	struct stream_fetch fetch;
	unsigned long element;

	if (!program || !count || count > MAXIMUM_GX_VERTICES || !draw_prepare())
		return;
	draw_serial++;
	fetch.shader = device.vertex_shader;
	fetch.indices = indices;
	fetch.first = first;
	for (element = 0; element < MAXIMUM_STREAMS; element++)
	{
		D3DVertexBuffer *buffer = device.streams[element].buffer;

		fetch.streams[element] = buffer && buffer->Data ? (const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(buffer->Data) :
			NULL;
	}
	stream_fetch_plan(&fetch);
	{
		uint64_t started = timing_now();

		BOOL drawn = draw_hardware(program, primitive_type, count, indices, first, &fetch);

		kept_pinned = FALSE;
		if (drawn)
		{
			timing.draws += timing_now() - started;
			if (draw_logging())
			{
				/* (described as the processor would draw it, then GX's pixels) */
				float point_size;

				memcpy(&point_size, &D3D__RenderState[D3DRS_POINTSIZE], sizeof(point_size));
				draw_log(primitive_type, count, &projection_loaded, stream_fetch, &fetch, program, point_size, TRUE);
				draw_log_pixels();
			}
			return;
		}
	}
	draw_run(program, primitive_type, count, stream_fetch, &fetch, indices, fetch.shader);
}

/* ---------- streams and draws */

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number < MAXIMUM_STREAMS)
	{
		device.streams[stream_number].buffer = stream_data;
		device.streams[stream_number].stride = stride;
	}
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	device.base_vertex_index = base_vertex_index;
	D3D__IndexData = index_data ? (WORD *)index_data->Data : NULL;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	device.statistics.draws++;
	device.statistics.vertices += vertex_count;
	draw_streams(primitive_type, vertex_count, NULL, start_vertex);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	device.statistics.draws++;
	device.statistics.vertices += vertex_count;
	draw_streams(primitive_type, vertex_count, index_data, 0);
}

/* immediate mode: a vertex is complete when its position (register 0) is set */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	device.immediate = TRUE;
	device.immediate_primitive = primitive_type;
	device.immediate_vertices = 0;
}

static void immediate_fetch(void *context, UINT vertex, float (*inputs)[4])
{
	(void)context;
	memcpy(inputs, device.immediate_buffer[vertex], NV2A_CPU_INPUT_COUNT * sizeof(*inputs));
}

void WINAPI D3DDevice_End(void)
{
	struct nv2a_cpu_program *program;

	if (device.immediate && device.immediate_vertices)
	{
		device.statistics.immediate_draws++;
		draw_serial++;
		draw_unstreamed = 0;
		device.statistics.vertices += device.immediate_vertices;
		program = program_current();
		if (program && device.immediate_buffer && draw_prepare())
			draw_run(program, device.immediate_primitive, (UINT)device.immediate_vertices, immediate_fetch, NULL, NULL, NULL);
	}
	device.immediate = FALSE;
}

static void vertex_data(INT reg, float a, float b, float c, float d)
{
	if (reg < 0 || reg >= VERTEX_REGISTER_COUNT)
		return;
	device.vertex_registers[reg][0] = a;
	device.vertex_registers[reg][1] = b;
	device.vertex_registers[reg][2] = c;
	device.vertex_registers[reg][3] = d;
	/* (the position, set last, completes the vertex) */
	if (reg == 0 && device.immediate && device.immediate_buffer &&
		device.immediate_vertices < MAXIMUM_IMMEDIATE_VERTICES)
	{
		memcpy(device.immediate_buffer[device.immediate_vertices++], device.vertex_registers,
			sizeof(device.vertex_registers));
	}
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	vertex_data(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	vertex_data(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	vertex_data(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	vertex_data(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	/* D3DCOLOR is ARGB; the register takes RGBA */
	vertex_data(reg, ((color >> 16) & 0xff) / 255.0f, ((color >> 8) & 0xff) / 255.0f, (color & 0xff) / 255.0f,
		((color >> 24) & 0xff) / 255.0f);
}

/* ---------- clearing and presenting */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	(void)count;
	(void)rectangles;
	(void)z;
	(void)stencil;
	device.statistics.clears++;
	if (!device.gx_ready)
		return;
	/* a frame's first clear of the back buffer is GX's own (frame_begin);
	any other is drawn (efb_clear_quad) */
	if (device.render_target == &device.back_buffer && !device.frame_open)
	{
		if (flags & D3DCLEAR_TARGET)
			device.clear_color = color;
		frame_begin(device.clear_color);
		return;
	}
	frame_begin(device.clear_color);
	efb_take();
	if (device.render_target == &device.back_buffer)
		wl_gx_target_drawn();
	if (flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER))
		efb_clear_quad(flags, color, z);
	if (draw_logging())
	{
		draw_log_state.draws++;
		platform_log("draw %lu: a clear of %s (flags %lx, colour %08lx, z %.3f)", draw_log_state.draws,
			device.render_target == &device.back_buffer ? "the back buffer" : "a texture", (unsigned long)flags,
			(unsigned long)color, z);
		draw_log_pixels();
	}
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	if (device.gx_ready)
	{
		uint64_t started = timing_now();

		frame_begin(device.clear_color);
		/* (the back buffer back, if a render target has the EFB) */
		{
			D3DSurface *target = device.render_target;

			device.render_target = &device.back_buffer;
			efb_take();
			device.render_target = target;
		}
		/* (the device set GX behind WiiLibs' shadow) */
		wl_gx_invalidate_shadow();
		states_shadow_reset();
		/* (GX's draws finished first, timed apart from the copy to the
		display, which wl_gx_end_frame makes: it waits for a free display
		buffer, not for the retrace; the retrace interrupt shows the frame) */
		if (!wl_gx_sync())
			gx_stall_log("the frame's draws");
		timing.present_gx += timing_now() - started;
		/* the display copy's own state, whatever the frame's copies to
		textures and drawing below the back buffer (sphere_bake) left */
		display_copy_restore();
		/* each frame on the screen for as many retraces as the game's
		presentation interval asks (two: 30 Hz), as the Xbox's display does;
		an early frame waits in WiiLibs' display queue, not here. GX still
		finishes each frame before Present returns: D3DResource_IsBusy and
		D3DDevice_IsBusy say FALSE, and what the frame drew from (the game's
		textures and vertex buffers, the palettes and fog ramp rewritten in
		place, the textures evicted during it) is reused straight after */
		{
			static uint32_t interval;
			uint32_t wanted = device.presentation.FullScreen_PresentationInterval == D3DPRESENT_INTERVAL_TWO ? 2 : 1;

			if (wanted != interval)
			{
				wii_video_present_interval(wanted);
				interval = wanted;
			}
		}
		{
			WlGxStats before, after;

			wl_gx_stats(&before);
			wl_gx_end_frame();
			wl_gx_stats(&after);
			if (after.gpu_timeouts != before.gpu_timeouts)
				gx_stall_log("the display copy");
		}
		device.frame_open = FALSE;
		/* (the frame drawn: the textures evicted during it can go) */
		GX_DrawDone();
		xf_flush_after();
		texture_frees_flush();
		static_outputs_frame_drawn();
		kept_normals_frame_drawn();
		index_lists_frame_drawn();
		kept_frees_flush();
		/* (GX's texture cache emptied once a frame, whatever was written) */
		texture_memory_changed = TRUE;
		timing.present += timing_now() - started;
	}
	draw_log_frame_end();
	flip_count++;
	device.frames++;
	{
		static u32 overflows;
		u32 now = GX_GetOverflowCount();

		if (now != overflows)
		{
			platform_log("Direct3D: frame %lu: %lu GX FIFO overflows so far", device.frames, (unsigned long)now);
			overflows = now;
		}
	}
	/* a frame of more than half a second (loading, mostly): where it went */
	{
		static uint64_t last_frame;
		static unsigned slow_frames;
		static struct platform_file_reads last_reads;
		uint64_t now = timing_now();

		if (last_frame && now - last_frame > 500UL * 60750UL && slow_frames < 40)
		{
			slow_frames++;
			platform_log("Direct3D: frame %lu: %lu file reads, %lu KB in %lu ms", device.frames,
				platform_file_reads.calls - last_reads.calls,
				(unsigned long)((platform_file_reads.bytes - last_reads.bytes) / 1024),
				(unsigned long)wl_udiv64_16(platform_file_reads.microseconds - last_reads.microseconds, 1000, NULL));
			platform_log("Direct3D: frame %lu took %lu ms: draws %lu, textures %lu, pixel shaders %lu, the end %lu, "
				"the game %lu", device.frames, timing_ms(now - last_frame),
				timing_ms(timing.draws - last_timing.draws),
				timing_ms(timing.textures - last_timing.textures),
				timing_ms(timing.pixel_shaders - last_timing.pixel_shaders),
				timing_ms(timing.present - last_timing.present),
				timing_ms(now - last_frame - (timing.draws - last_timing.draws) -
					(timing.textures - last_timing.textures) - (timing.pixel_shaders - last_timing.pixel_shaders) -
					(timing.present - last_timing.present)));
		}
		last_frame = now;
		last_timing = timing;
		last_reads = platform_file_reads;
	}
	if (device.frames % STATISTICS_FRAMES == 0)
	{
		platform_log("Direct3D: frame %lu: %lu draws, %lu immediate, %lu vertices (%lu run), %lu clears, %lu drawn "
			"elsewhere, in the last %d", device.frames, device.statistics.draws, device.statistics.immediate_draws,
			device.statistics.vertices, device.statistics.vertices_run, device.statistics.clears, device.draws_elsewhere,
			STATISTICS_FRAMES);
		platform_log("Direct3D: textures %lu bound, %lu failed, %lu without a mode", device.statistics.textures_bound,
			device.statistics.textures_failed, device.statistics.stages_without_mode);
		program_statistics_log();
		/* (lines short of the log's line, some 200 characters) */
		platform_log("Direct3D: in hardware: %lu draws (%lu of their matrices kept, %lu of several nodes, %lu with "
			"blended vertices), %lu vertices", hardware_draws, hardware_cache_hits, hardware_keyed_draws,
			hardware_blended_draws, hardware_vertices);
		platform_log("Direct3D: in hardware, the processor's: colours of %lu vertices, positions of %lu in models' draws",
			hardware_colors_run, processor_positions);
		platform_log("Direct3D: position matrices' program runs: %lu of four points at once, %lu of one",
			position_lanes_runs, position_single_runs);
		position_lanes_runs = position_single_runs = 0;
		hardware_draws = hardware_vertices = hardware_colors_run = hardware_cache_hits = hardware_keyed_draws = 0;
		hardware_blended_draws = processor_positions = 0;
		platform_log("Direct3D: draws' index ranges: %lu taken as kept, %lu found; models' node keys: %lu taken as "
			"kept, %lu found (%lu KB kept)", ranges_kept, ranges_found, key_records_kept, key_records_made,
			index_record_bytes / 1024);
		ranges_kept = ranges_found = key_records_kept = key_records_made = 0;
		platform_log("Direct3D: the structure's indices as display lists: %lu draws called one, %lu written (%lu KB "
			"kept), %lu without room", index_list_calls, index_lists_made, index_lists_bytes / 1024, index_lists_refused);
		index_list_calls = index_lists_made = index_lists_refused = 0;
		{
			unsigned index, slabs = 0;

			for (index = 0; index < KEPT_SLABS; index++)
				if (kept_slabs[index].base)
					slabs++;
			platform_log("Direct3D: the kept buffers' memory: %lu KB of the window (%u slabs, %lu KB of them in use; "
				"%lu blocks of their own); %lu blocks refused, given back %lu times", kept_pool_bytes / 1024, slabs,
				kept_slab_granules * KEPT_SLAB_GRANULE / 1024, kept_blocks, kept_window_refused, kept_reclaims);
			kept_window_refused = 0;
		}
		platform_log("Direct3D: in hardware with the processor's sphere map coordinates: %lu draws (%lu lit by GX), "
			"%lu vertices run", hardware_sphere_draws, hardware_sphere_lit_draws, hardware_spheres_run);
		hardware_sphere_draws = hardware_sphere_lit_draws = hardware_spheres_run = 0;
		{
			/* (the kinds of hardware_emit's draws, most indices first) */
			char line[320];
			unsigned kind, shown, length;

			length = (unsigned)snprintf(line, sizeof(line), "Direct3D: the processor's part in hardware's vertices "
				"(kind: draws/indices):");
			for (shown = 0; shown < 8 && length < sizeof(line) - 32; shown++)
			{
				unsigned best = HW_KINDS;

				for (kind = 0; kind < HW_KINDS; kind++)
					if (hardware_kinds[kind].indices &&
						(best == HW_KINDS || hardware_kinds[kind].indices > hardware_kinds[best].indices))
						best = kind;
				if (best == HW_KINDS)
					break;
				length += (unsigned)snprintf(line + length, sizeof(line) - length, " %x: %lu/%lu",
					hardware_kinds[best].flags, hardware_kinds[best].draws, hardware_kinds[best].indices);
				hardware_kinds[best].indices = 0;
			}
			platform_log("%s", line);
			memset(hardware_kinds, 0, sizeof(hardware_kinds));
		}
		{
			unsigned index, entries = 0, kept_entries = 0;

			for (index = 0; index < STATIC_OUTPUTS_ENTRIES; index++)
			{
				entries += static_outputs[index].used ? 1 : 0;
				kept_entries += static_outputs[index].values ? 1 : 0;
			}
			platform_log("Direct3D: static buffers' outputs kept: %lu draws read them, %lu vertices, %lu computed; "
				"%u buffers seen, %u kept, %lu KB; %lu draws without room", kept_draws, kept_vertices, kept_computed,
				entries, kept_entries, static_outputs_bytes / 1024, kept_refused);
			kept_draws = kept_vertices = kept_computed = kept_refused = 0;
		}
		platform_log("Direct3D: the streams GX reads: %lu ranges flushed (%lu KB), %lu flushed before and kept",
			stream_flushes_made, stream_flush_bytes / 1024, stream_flushes_kept);
		stream_flushes_made = stream_flush_bytes = stream_flushes_kept = 0;
		platform_log("Direct3D: lit by GX (display.gx_lighting): %lu draws, %lu vertices, %lu refused; view sphere maps: "
			"%lu drawn, %lu draws took a near one", lit_draws, lit_vertices, lit_refused, sphere_bakes_made,
			sphere_bakes_borrowed);
		platform_log("Direct3D: lit by GX, the planar fog by GX's ramp in %lu draws (%lu its product with the "
			"atmospheric fog; %lu ramps written), the processor's in %lu; a model's fog through its nodes' matrices in "
			"%lu draws", lit_fog_draws, lit_fog_products, fog_ramps_made, lit_fog_refused, fog_post_draws);
		{
			unsigned index, entries = 0;
			unsigned long wanted = 0;

			/* (wanted: what the entries hold, and what those refused asked for) */
			for (index = 0; index < KEPT_NORMALS_ENTRIES; index++)
			{
				const struct kept_normals *entry = &kept_normals[index];

				entries += entry->values ? 1 : 0;
				wanted += (entry->wanted > entry->capacity ? entry->wanted : entry->capacity) * 12;
			}
			platform_log("Direct3D: lit by GX, the normals read by index in %lu draws; %lu decoded, %u buffers' kept "
				"(%lu KB of %lu, %lu KB wanted), %lu draws without room, %lu buffers gave theirs up", kept_normal_draws,
				kept_normals_decoded, entries, kept_normals_bytes / 1024, KEPT_NORMALS_BYTES / 1024, wanted / 1024,
				kept_normals_refused, kept_normals_given);
			kept_normal_draws = kept_normals_decoded = kept_normals_refused = kept_normals_given = 0;
		}
		lit_draws = lit_vertices = lit_refused = sphere_bakes_made = sphere_bakes_borrowed = fog_post_draws = 0;
		lit_fog_draws = lit_fog_products = fog_ramps_made = lit_fog_refused = 0;
		platform_log("Direct3D: textures not drawn: %lu without data, %lu of a shape, %lu of a format, "
			"%lu without memory, %lu not decoded; %lu converted bitmaps found", texture_failures[_failure_no_data],
			texture_failures[_failure_shape],
			texture_failures[_failure_format], texture_failures[_failure_memory], texture_failures[_failure_decode],
			textures_native);
		platform_log("Direct3D: textures found %lu by their mark, %lu looked up, %lu new; %lu objects built, "
			"TMEM emptied %lu times", texture_counts.marked, texture_counts.looked_up, texture_counts.created,
			texture_counts.objects, texture_counts.invalidations);
		platform_log("Direct3D: textures' pixels: %lu hashed (%lu KB), %lu decoded, %lu flushed (%lu KB)",
			texture_counts.hashed, texture_counts.hashed_bytes / 1024, texture_counts.decoded, texture_counts.flushed,
			texture_counts.flushed_bytes / 1024);
		memset(&texture_counts, 0, sizeof(texture_counts));
		{
			static struct gx_psh_cache_counts shaders_last;
			struct gx_psh_cache_counts shaders;
			WlGxTargetStats targets;

			gx_psh_cache_counts(&shaders);
			wl_gx_target_stats(&targets, true);
			platform_log("Direct3D: pixel shaders: %lu translated, %lu beyond GX; GX FIFO overflows %lu; "
				"render targets: %lu parks, %lu copies, %lu not copied", shaders.translated, shaders.unusable,
				(unsigned long)GX_GetOverflowCount(), (unsigned long)targets.parks, (unsigned long)targets.resolves,
				(unsigned long)targets.skipped);
			platform_log("Direct3D: pixel shader programs, the last %d frames: %lu translated, %lu kept, %lu with new "
				"constants; tests %lu (%lu otherwise, %lu generic), audits %lu (%lu differ)", STATISTICS_FRAMES,
				shaders.translated - shaders_last.translated, shaders.found - shaders_last.found,
				shaders.patched - shaders_last.patched, shaders.tests - shaders_last.tests,
				shaders.tests_differ - shaders_last.tests_differ, shaders.generic - shaders_last.generic,
				shaders.audited - shaders_last.audited, shaders.audits_differ - shaders_last.audits_differ);
			platform_log("Direct3D: pixel shader structures pushed out of the cache: %lu, translated again: %lu",
				shaders.evicted - shaders_last.evicted, shaders.again - shaders_last.again);
			shaders_last = shaders;
		}
		{
			uint64_t now = timing_now();

			if (timing.window_start)
			{
				platform_log("Direct3D: a frame's draws: GX's transform %lu ms (matrices %lu, vertices %lu), the "
					"processor's %lu, immediate %lu", timing_frame(timing.hardware_setup + timing.hardware_vertices, 60750),
					timing_frame(timing.hardware_setup, 60750),
					timing_frame(timing.hardware_vertices, 60750),
					timing_frame(timing.processor, 60750),
					timing_frame(timing.immediate, 60750));
				platform_log("Direct3D: GX's transform's setup, a frame (tenths of a ms): what the draw depends on %lu, "
					"the matrices %lu, the lights and sphere maps %lu",
					timing_frame(timing.setup_keys, 6075),
					timing_frame(timing.setup_matrices, 6075),
					timing_frame(timing.setup_lit, 6075));
				platform_log("Direct3D: GX's transform's setup, a frame (tenths of a ms): the kept outputs %lu, GX's "
					"state %lu; %lu draws", timing_frame(timing.setup_kept, 6075),
					timing_frame(timing.setup_state, 6075),
					(unsigned long)wl_udiv64_16(timing.setup_draws, STATISTICS_FRAMES, NULL));
				platform_log("Direct3D: the processor's indexed vertices: fetch %lu ms, program %lu, store %lu, emit %lu",
					timing_frame(timing.fetch_ticks, 60750),
					timing_frame(timing.program_ticks, 60750),
					timing_frame(timing.store_ticks, 60750),
					timing_frame(timing.emit_ticks, 60750));
				platform_log("Direct3D: a frame takes %lu ms: draws %lu, textures %lu, pixel shaders %lu, the end of "
					"the frame %lu (waiting for GX), the game %lu",
					timing_frame(now - timing.window_start, 60750),
					timing_frame(timing.draws, 60750),
					timing_frame(timing.textures, 60750),
					timing_frame(timing.pixel_shaders, 60750),
					timing_frame(timing.present, 60750),
					timing_frame(now - timing.window_start - timing.draws - timing.textures - timing.pixel_shaders -
						timing.present, 60750));
				platform_log("Direct3D: the end of the frame: GX's draws %lu ms, the copy and a free display buffer %lu",
					timing_frame(timing.present_gx, 60750),
					timing_frame(timing.present - timing.present_gx, 60750));
				{
					static WlVideoStats last;
					WlVideoStats video;

					wl_platform_video_stats(&video);
					platform_log("Direct3D: the display: %lu frames shown (%lu late), every %lu retraces at the "
						"last; %lu waits for a free buffer, %lu ms in all, the longest %lu",
						(unsigned long)(video.shown - last.shown), (unsigned long)(video.late - last.late),
						(unsigned long)video.last_gap, (unsigned long)(video.buffer_waits - last.buffer_waits),
						(unsigned long)wl_udiv64_16(video.buffer_wait_us - last.buffer_wait_us, 1000, NULL),
						(unsigned long)wl_udiv64_16(video.buffer_wait_us_max, 1000, NULL));
					last = video;
				}
			}
			memset(&timing, 0, sizeof(timing));
			timing.window_start = now;
			last_timing = timing;
		}
		memset(texture_failures, 0, sizeof(texture_failures));
		memset(&device.statistics, 0, sizeof(device.statistics));
		device.draws_elsewhere = 0;
		if (device.frames % (STATISTICS_FRAMES * 4) == 0)
		{
			/* the heaps: MEM1's (newlib's), and WiiLibs' in MEM2 */
			unsigned long used, free_bytes;

			extern void dsound_wii_audio_report(void);

			dsound_wii_audio_report();
			wii_heap_usage(&used, &free_bytes);
			platform_log("memory: MEM1 heap %lu KB used, %lu KB free", used / 1024, free_bytes / 1024);
			{
				unsigned long index, decoded = 0;

				for (index = 0; index < TEXTURE_CACHE_ENTRIES; index++)
					decoded += texture_cache[index].texels && !texture_cache[index].native;
				platform_log("memory: decoded textures %lu KB of %lu in %lu, %lu given up for others (%lu KB)",
					texture_cache_bytes / 1024, TEXTURE_CACHE_BYTES / 1024, decoded, textures_reclaimed,
					texture_reclaim_bytes / 1024);
			}
			{
				extern unsigned long wii_game_stack_used(unsigned long *size);
				unsigned long size, deepest = wii_game_stack_used(&size);

				platform_log("memory: the game's stack at most %lu KB deep of %lu", deepest / 1024, size / 1024);
			}
			{
				unsigned long base, size, largest;

				wii_memory_window_range(&base, &size);
				wii_memory_window_usage(&used, &free_bytes, &largest);
				platform_log("memory: MEM2 window (tag, texture and sound caches, Direct3D) at %08lx, %lu KB: "
					"%lu KB used, %lu KB free (%lu KB together)", base, size / 1024, used / 1024, free_bytes / 1024,
					largest / 1024);
			}
			wl_mem_log_usage();
#if defined(__has_include)
#if __has_include("wl/platform/heap_sites.h")
			/* who holds the heaps (port/wii/src/wii_boot.c turns it on) */
			wl_heap_overflow_log_sites(16);
#endif
#endif
		}
	}
}

/* ---------- the port's screen (port/linux/include/halo_linux_source_fixups.h):
the Wii's is the Xbox's 640 by 480 */

long halo_screen_width(void)
{
	return SCREEN_WIDTH;
}

long halo_screen_commit(void)
{
	return SCREEN_WIDTH;
}

/* (the screen's targets draw a pixel for each of the Xbox's) */
float halo_screen_scale(void)
{
	return 1.0f;
}

long halo_shadow_map_scale(void)
{
	return 1;
}

void halo_screen_ui_offset(unsigned char centered)
{
	(void)centered;
}

void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
}
