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
#include "port_config.h"
#include "nv2a_cpu.h"
#include "xbox_texture_formats.h"
#include "wl/codec/gx_tex.h"

#include "wl/platform/mem.h"
#include "wl/platform/platform.h"

void wii_heap_usage(unsigned long *used, unsigned long *free_bytes);

void d3d8_state_defaults(void);

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
/* bytes of decoded textures kept (in the game's heap) */
#define TEXTURE_CACHE_BYTES (1536UL * 1024UL)
#define TEXTURE_CACHE_ENTRIES 128
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
};

struct stream_source
{
	D3DVertexBuffer *buffer;
	UINT stride;
};

/* where a frame's time goes, in microseconds (logged with the statistics) */
struct frame_timing
{
	uint64_t window_start;
	uint64_t draws, textures, pixel_shaders, present;
};

struct draw_statistics
{
	unsigned long draws;
	unsigned long vertices;
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
		device.gx_ready = wl_platform_video_init() == WL_OK && wl_gx_init() == WL_OK;
		if (!device.gx_ready)
			platform_log("Direct3D: GX did not start (nothing is displayed)");
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

static void surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height)
{
	*width = (surface->Size & D3DSIZE_WIDTH_MASK) + 1;
	*height = ((surface->Size & D3DSIZE_HEIGHT_MASK) >> D3DSIZE_HEIGHT_SHIFT) + 1;
}

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
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
}

/* ---------- the frame on GX

A frame is begun on GX by the first clear or draw after a Present (cleared
to the clear's colour), and ended, copied and shown by the Present. The
device sets GX itself, around WiiLibs' frame (wl_gx_invalidate_shadow): an
orthographic projection over the screen's pixels, the vertex format below,
the colours as the vertices give them. */

static BOOL projection_valid;

static void frame_begin(D3DCOLOR clear_color)
{
	Mtx identity;

	if (device.frame_open || !device.gx_ready)
		return;
	/* ARGB to the RGBA WiiLibs takes */
	wl_gx_begin_frame((clear_color << 8) | (clear_color >> 24));
	device.frame_open = TRUE;

	/* (x and y in pixels from the top left, the depth given as z - 1: GX's
	clip-space depth runs from -1, near, to 0, far; draw_run loads it) */
	projection_valid = FALSE;
	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX2, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX3, GX_DIRECT);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_TEX2, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_TEX3, GX_TEX_ST, GX_F32, 0);
	gx_psh_initialize();
	GX_SetNumChans(2);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(4);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX2, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD3, GX_TG_MTX2x4, GX_TG_TEX3, GX_IDENTITY);
	GX_SetViewport(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 1.0f);
	GX_SetScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
}

/* ---------- render states to GX */

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
	DWORD write = D3D__RenderState[D3DRS_COLORWRITEENABLE];

	if (D3D__RenderState[D3DRS_ALPHABLENDENABLE])
	{
		DWORD operation = D3D__RenderState[D3DRS_BLENDOP];

		GX_SetBlendMode(operation == D3DBLENDOP_REVSUBTRACT ? GX_BM_SUBTRACT : GX_BM_BLEND,
			gx_blend_factor(D3D__RenderState[D3DRS_SRCBLEND], TRUE),
			gx_blend_factor(D3D__RenderState[D3DRS_DESTBLEND], FALSE), GX_LO_CLEAR);
	}
	else
	{
		GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	}
	GX_SetColorUpdate((write & (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE)) ?
		GX_TRUE : GX_FALSE);
	GX_SetAlphaUpdate((write & D3DCOLORWRITEENABLE_ALPHA) ? GX_TRUE : GX_FALSE);
	GX_SetZMode(D3D__RenderState[D3DRS_ZENABLE] ? GX_TRUE : GX_FALSE, gx_compare(D3D__RenderState[D3DRS_ZFUNC]),
		D3D__RenderState[D3DRS_ZWRITEENABLE] ? GX_TRUE : GX_FALSE);
	if (D3D__RenderState[D3DRS_ALPHATESTENABLE])
	{
		GX_SetAlphaCompare(gx_compare(D3D__RenderState[D3DRS_ALPHAFUNC]), (u8)D3D__RenderState[D3DRS_ALPHAREF],
			GX_AOP_AND, GX_ALWAYS, 0);
		/* (the test before the depth write) */
		GX_SetZCompLoc(GX_FALSE);
	}
	else
	{
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_TRUE);
	}
	/* (the device draws the vertices as the program leaves them, in screen
	space; their winding is not kept yet: nothing is culled) */
	GX_SetCullMode(GX_CULL_NONE);
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
	/* a decoded texture's pixels when it was decoded, hashed: the game
	rewrites some (its text's glyph cache) */
	unsigned long hash, source_bytes;
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
/* why textures were not drawn (logged with the frame statistics) */
static unsigned long texture_failures[5];
enum { _failure_no_data, _failure_shape, _failure_memory, _failure_decode, _failure_format };
static unsigned long textures_native;

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
	return hash;
}

static void texture_evict(struct texture_entry *entry)
{
	if (entry->texels && !entry->native)
	{
		free(entry->texels);
		texture_cache_bytes -= entry->bytes;
	}
	memset(entry, 0, sizeof(*entry));
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
	entry->used_frame = device.frames - 1;
	textures_native++;
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
	unsigned long index, texel_count;

	if (!texture || !texture->Data)
	{
		texture_failures[_failure_no_data]++;
		return NULL;
	}
	for (index = 0; index < TEXTURE_CACHE_ENTRIES; index++)
	{
		struct texture_entry *candidate = &texture_cache[index];

		if (candidate->texels && candidate->data == texture->Data && candidate->format == texture->Format &&
			candidate->size == texture->Size)
		{
			entry = candidate;
			break;
		}
		if (!oldest || candidate->used_frame < oldest->used_frame || !candidate->texels)
			oldest = candidate;
	}
	if (entry && !entry->native && entry->used_frame != device.frames &&
		texture_hash(texture, entry->source_bytes) != entry->hash)
	{
		/* (rewritten since it was decoded: again) */
		texture_evict(entry);
		oldest = entry;
		entry = NULL;
	}
	if (entry)
	{
		/* (the processor wrote a converted bitmap's pixels as it read them:
		they reach memory before GX reads them, once a frame) */
		if (entry->native && entry->used_frame != device.frames)
			DCFlushRange(entry->texels, entry->bytes);
		entry->used_frame = device.frames;
		return entry;
	}

	xgpu_texture_describe(texture->Format, texture->Size, &description);
	if (!description.cube_map && description.depth <= 1 &&
		((texture->Format & D3DFORMAT_DIMENSION_MASK) >> D3DFORMAT_DIMENSION_SHIFT) == 2 &&
		gx_native_format(description.format, &native) && texture_in_texture_cache(texture))
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
			DCFlushRange(entry->texels, entry->bytes);
			entry->used_frame = device.frames;
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
	if (description.compressed)
		xgpu_texture_dxt_decode_level(information.kind, source, description.width, description.height, 1, argb);
	else if (!xgpu_texture_decode_level(&description, 0, source,
		palette && palette->Data ? (const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(palette->Data) : NULL, argb))
	{
		texture_failures[_failure_decode]++;
		free(argb);
		return NULL;
	}
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
		entry->used_frame = device.frames;
		texture_cache_bytes += bytes;
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

/* what each stage's texture is, for the pixel stage */
static struct texture_entry *bound_entries[D3DTSS_MAXSTAGES];
/* a linear texture is addressed in texels, GX's in fractions of it */
static float texcoord_scale[D3DTSS_MAXSTAGES][2] = { { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 } };

/* the stages' textures bound to GX's texture maps (a DXT3 or DXT5 texture's
alpha to the map four after); which stages have one */
static unsigned textures_bind(void)
{
	unsigned stages = 0;
	DWORD stage;

	GX_InvalidateTexAll();
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		struct texture_entry *entry;
		GXTexObj object;
		BOOL mipmapped, repeatable;
		u8 wrap_s, wrap_t, minification;

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
		wrap_s = repeatable ? gx_wrap(D3D__TextureState[stage][D3DTSS_ADDRESSU]) : GX_CLAMP;
		wrap_t = repeatable ? gx_wrap(D3D__TextureState[stage][D3DTSS_ADDRESSV]) : GX_CLAMP;
		minification = gx_minification(stage, mipmapped);
		GX_InitTexObj(&object, entry->texels, (u16)entry->width, (u16)entry->height, entry->gx_format, wrap_s, wrap_t,
			mipmapped ? GX_TRUE : GX_FALSE);
		GX_InitTexObjLOD(&object, minification,
			D3D__TextureState[stage][D3DTSS_MAGFILTER] == D3DTEXF_POINT ? GX_NEAR : GX_LINEAR,
			0.0f, mipmapped ? (f32)(entry->levels - 1) : 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
		GX_LoadTexObj(&object, (u8)(GX_TEXMAP0 + stage));
		if (entry->alpha_texels)
		{
			GX_InitTexObj(&object, entry->alpha_texels, (u16)entry->width, (u16)entry->height, GX_TF_I4, wrap_s, wrap_t,
				mipmapped ? GX_TRUE : GX_FALSE);
			GX_InitTexObjLOD(&object, minification,
				D3D__TextureState[stage][D3DTSS_MAGFILTER] == D3DTEXF_POINT ? GX_NEAR : GX_LINEAR,
				0.0f, mipmapped ? (f32)(entry->levels - 1) : 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
			GX_LoadTexObj(&object, (u8)(GX_TEXMAP4 + stage));
		}
		bound_entries[stage] = entry;
		if (entry->linear)
		{
			texcoord_scale[stage][0] = 1.0f / (float)entry->width;
			texcoord_scale[stage][1] = 1.0f / (float)entry->height;
		}
		stages |= 1U << stage;
	}
	return stages;
}

/* ---------- the pixel stage

The pixel shader's register combiners are translated to TEV stages
(gx_psh.c), kept by the pixel shader render states and the textures' kinds
they were translated for. A shader GX cannot hold takes the first stage's
texture, modulated by the diffuse colour. */

#define PIXEL_SHADER_CACHE_ENTRIES 48

struct pixel_shader_entry
{
	DWORD state[D3DRS_PS_MAX];
	struct gx_psh_textures textures;
	BOOL usable;
	struct gx_psh_program program;
};

static struct pixel_shader_entry pixel_shader_cache[PIXEL_SHADER_CACHE_ENTRIES];
static unsigned pixel_shader_cache_count, pixel_shader_cache_next, pixel_shader_last;
static unsigned long pixel_shaders_translated, pixel_shaders_unusable;

static void tev_fallback(unsigned stages)
{
	struct texture_entry *entry = bound_entries[0];

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

static void tev_setup(unsigned stages)
{
	static int debug = -1;
	struct gx_psh_textures textures;
	struct pixel_shader_entry *entry = NULL;
	DWORD stage;
	unsigned index;

	/* the desktop ports' debug views (port_config.c): the diffuse colour
	alone, or the first texture alone, opaque */
	if (debug < 0)
		debug = config_boolean("debug.gpu_debug_flat") ? 1 : config_boolean("debug.gpu_debug_texture0") ? 2 : 0;
	if (debug)
	{
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
	for (stage = 0; stage < GX_PSH_TEXTURES; stage++)
	{
		DWORD mode = (D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;

		/* (2D and projected lookups: the rest, cube maps, dot products and
		bump maps, read as nothing) */
		if (!(stages & (1U << stage)) || !bound_entries[stage] || (mode != 1 && mode != 2))
			continue;
		textures.present |= 1U << stage;
		if (bound_entries[stage]->alpha_texels)
			textures.separate_alpha |= 1U << stage;
		textures.contents[stage] = bound_entries[stage]->channels;
	}
	if (pixel_shader_last < pixel_shader_cache_count)
	{
		struct pixel_shader_entry *last = &pixel_shader_cache[pixel_shader_last];

		if (!memcmp(&last->textures, &textures, sizeof(textures)) &&
			!memcmp(last->state, D3D__RenderState, sizeof(last->state)))
			entry = last;
	}
	for (index = 0; !entry && index < pixel_shader_cache_count; index++)
	{
		struct pixel_shader_entry *candidate = &pixel_shader_cache[index];

		if (!memcmp(&candidate->textures, &textures, sizeof(textures)) &&
			!memcmp(candidate->state, D3D__RenderState, sizeof(candidate->state)))
		{
			entry = candidate;
			pixel_shader_last = index;
		}
	}
	if (!entry)
	{
		index = pixel_shader_cache_next;
		pixel_shader_cache_next = (pixel_shader_cache_next + 1) % PIXEL_SHADER_CACHE_ENTRIES;
		if (pixel_shader_cache_count < PIXEL_SHADER_CACHE_ENTRIES)
			pixel_shader_cache_count++;
		entry = &pixel_shader_cache[index];
		memcpy(entry->state, D3D__RenderState, sizeof(entry->state));
		entry->textures = textures;
		entry->usable = gx_psh_translate(D3D__RenderState, &textures, &entry->program);
		pixel_shaders_translated++;
		if (!entry->usable)
		{
			pixel_shaders_unusable++;
			if (pixel_shaders_unusable <= 8)
			{
				const DWORD *state = D3D__RenderState;
				unsigned long count = state[D3DRS_PSCOMBINERCOUNT] & 0xff, combiner;

				platform_log("Direct3D: pixel shader beyond GX (%s): %lu combiners (%08lx), final %08lx %08lx, modes %08lx",
					entry->program.reason ? entry->program.reason : "no stages", count,
					(unsigned long)state[D3DRS_PSCOMBINERCOUNT], (unsigned long)state[D3DRS_PSFINALCOMBINERINPUTSABCD],
					(unsigned long)state[D3DRS_PSFINALCOMBINERINPUTSEFG], (unsigned long)state[D3DRS_PSTEXTUREMODES]);
				for (combiner = 0; combiner < count && combiner < 8; combiner++)
					platform_log("  %lu: rgb %08lx -> %08lx, alpha %08lx -> %08lx", combiner,
						(unsigned long)state[D3DRS_PSRGBINPUTS0 + combiner], (unsigned long)state[D3DRS_PSRGBOUTPUTS0 + combiner],
						(unsigned long)state[D3DRS_PSALPHAINPUTS0 + combiner], (unsigned long)state[D3DRS_PSALPHAOUTPUTS0 + combiner]);
			}
		}
		pixel_shader_last = index;
	}
	if (entry->usable)
		gx_psh_apply(&entry->program);
	else
		tev_fallback(stages);
}

/* ---------- vertices

Each draw's vertices are the program's: fetched from the streams (or the
immediate registers), run, then sent to GX as they come out. */

static float normalized(long value, float maximum)
{
	float result = (float)value / maximum;

	return result < -1.0f ? -1.0f : result;
}

static void element_fetch(const unsigned char *source, unsigned type, float out[4])
{
	float f[4];
	short s[4];
	unsigned long word;
	int component;

	out[0] = out[1] = out[2] = 0.0f;
	out[3] = 1.0f;
	switch (type)
	{
	case D3DVSDT_FLOAT1: case D3DVSDT_FLOAT2: case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT4:
		memcpy(f, source, (type >> 4) * sizeof(float));
		for (component = 0; component < (int)(type >> 4); component++)
			out[component] = f[component];
		break;
	case D3DVSDT_FLOAT2H:
		memcpy(f, source, 3 * sizeof(float));
		out[0] = f[0];
		out[1] = f[1];
		out[3] = f[2];
		break;
	case D3DVSDT_D3DCOLOR:
		memcpy(&word, source, sizeof(word));
		out[0] = ((word >> 16) & 0xff) / 255.0f;
		out[1] = ((word >> 8) & 0xff) / 255.0f;
		out[2] = (word & 0xff) / 255.0f;
		out[3] = ((word >> 24) & 0xff) / 255.0f;
		break;
	case D3DVSDT_SHORT1: case D3DVSDT_SHORT2: case D3DVSDT_SHORT3: case D3DVSDT_SHORT4:
		memcpy(s, source, (type >> 4) * sizeof(short));
		for (component = 0; component < (int)(type >> 4); component++)
			out[component] = (float)s[component];
		break;
	case D3DVSDT_NORMSHORT1: case D3DVSDT_NORMSHORT2: case D3DVSDT_NORMSHORT3: case D3DVSDT_NORMSHORT4:
		memcpy(s, source, (type >> 4) * sizeof(short));
		for (component = 0; component < (int)(type >> 4); component++)
			out[component] = normalized(s[component], 32767.0f);
		break;
	case D3DVSDT_NORMPACKED3:
		memcpy(&word, source, sizeof(word));
		out[0] = normalized(((long)(word << 21)) >> 21, 1023.0f);
		out[1] = normalized(((long)(word << 10)) >> 21, 1023.0f);
		out[2] = normalized(((long)word) >> 22, 511.0f);
		break;
	case D3DVSDT_PBYTE1: case D3DVSDT_PBYTE2: case D3DVSDT_PBYTE3: case D3DVSDT_PBYTE4:
		for (component = 0; component < (int)(type >> 4); component++)
			out[component] = source[component] / 255.0f;
		break;
	default:
		break;
	}
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
};

static struct draw_projection projection_loaded;
/* the last fit, for a draw whose vertices are all at one w */
static float projection_a, projection_b = 0.0f;

static void projection_load(const struct draw_projection *projection)
{
	Mtx44 matrix;

	if (projection_valid && projection_loaded.perspective == projection->perspective &&
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
		matrix[2][3] = projection->b;
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
		matrix[3][3] = 1.0f;
		GX_LoadProjectionMtx(matrix, GX_ORTHOGRAPHIC);
	}
	projection_loaded = *projection;
	projection_valid = TRUE;
}

static void vertex_send(const struct nv2a_cpu_output *out, const struct draw_projection *projection)
{
	/* the program's screen position (pixel centres on integers, as Direct3D
	puts them), its depth scaled back to 0..1 */
	float depth = out->position[2] / 16777215.0f;
	int stage;

	if (projection->perspective)
	{
		const float *scale = device.vertex_constants[VERTEX_CONSTANT_BIAS - 38];
		const float *offset = device.vertex_constants[VERTEX_CONSTANT_BIAS - 37];
		float w = out->clip[3];

		GX_Position3f32(out->clip[0] * scale[0] + (offset[0] + 0.5f) * w, out->clip[1] * scale[1] + (offset[1] + 0.5f) * w,
			-w);
	}
	else
	{
		if (!(depth >= 0.0f))
			depth = 0.0f;
		if (depth > 1.0f)
			depth = 1.0f;
		GX_Position3f32(out->position[0] + 0.5f, out->position[1] + 0.5f, depth - 1.0f);
	}
	GX_Color4u8(color_byte(out->d0[0]), color_byte(out->d0[1]), color_byte(out->d0[2]), color_byte(out->d0[3]));
	GX_Color4u8(color_byte(out->d1[0]), color_byte(out->d1[1]), color_byte(out->d1[2]), color_byte(out->d1[3]));
	for (stage = 0; stage < 4; stage++)
		GX_TexCoord2f32(out->t[stage][0] * texcoord_scale[stage][0], out->t[stage][1] * texcoord_scale[stage][1]);
}

static u8 gx_primitive(D3DPRIMITIVETYPE type);

/* one frame's draws described in the log (DRAW_LOG_FRAME; 0: none) */
#ifndef DRAW_LOG_FRAME
#define DRAW_LOG_FRAME 330
#endif

static void draw_log(D3DPRIMITIVETYPE primitive_type, UINT count, const struct draw_projection *projection,
	void (*fetch)(void *, UINT, float (*)[4]), void *context, struct nv2a_cpu_program *program, float point_size)
{
	float inputs[NV2A_CPU_INPUT_COUNT][4];
	struct nv2a_cpu_output out;
	float minimum[2] = { 1.0e9f, 1.0e9f }, maximum[2] = { -1.0e9f, -1.0e9f };
	char textures[160];
	int length = 0, stage;
	UINT vertex;

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

		if (entry && length < (int)sizeof(textures) - 40)
			length += snprintf(textures + length, sizeof(textures) - (size_t)length, " t%d %lux%lu/%u%s%s", stage,
				entry->width, entry->height, entry->gx_format, entry->native ? "n" : "d", entry->alpha_texels ? "+a" : "");
	}
	platform_log("draw %lu: prim %d x%u %s screen %.0f,%.0f-%.0f,%.0f d0 %.2f %.2f %.2f %.2f | blend %lu %lu>%lu atest %lu %lu/%lu z %lu/%lu/%lu wr %lx | ps %lx fin %08lx %08lx modes %05lx |%s",
		device.statistics.draws + device.statistics.immediate_draws, (int)primitive_type, count,
		projection->perspective ? "persp" : "ortho", minimum[0], minimum[1], maximum[0], maximum[1],
		out.d0[0], out.d0[1], out.d0[2], out.d0[3],
		(unsigned long)D3D__RenderState[D3DRS_ALPHABLENDENABLE], (unsigned long)D3D__RenderState[D3DRS_SRCBLEND],
		(unsigned long)D3D__RenderState[D3DRS_DESTBLEND], (unsigned long)D3D__RenderState[D3DRS_ALPHATESTENABLE],
		(unsigned long)D3D__RenderState[D3DRS_ALPHAFUNC], (unsigned long)D3D__RenderState[D3DRS_ALPHAREF],
		(unsigned long)D3D__RenderState[D3DRS_ZENABLE], (unsigned long)D3D__RenderState[D3DRS_ZFUNC],
		(unsigned long)D3D__RenderState[D3DRS_ZWRITEENABLE], (unsigned long)D3D__RenderState[D3DRS_COLORWRITEENABLE],
		(unsigned long)D3D__RenderState[D3DRS_PSCOMBINERCOUNT], (unsigned long)D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD],
		(unsigned long)D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG], (unsigned long)D3D__RenderState[D3DRS_PSTEXTUREMODES],
		textures);
}

/* a draw's vertices: each fetched (its inputs), run and sent */
typedef void (*vertex_fetch_proc)(void *context, UINT vertex, float (*inputs)[4]);

static struct frame_timing timing;

static void draw_run(struct nv2a_cpu_program *program, D3DPRIMITIVETYPE primitive_type, UINT count,
	vertex_fetch_proc fetch, void *context)
{
	uint64_t started = wl_platform_time_us();
	float inputs[NV2A_CPU_INPUT_COUNT][4];
	struct nv2a_cpu_output out;
	struct draw_projection projection = { FALSE, 0.0f, 0.0f };
	float point_size;
	UINT vertex;

	memcpy(&point_size, &D3D__RenderState[D3DRS_POINTSIZE], sizeof(point_size));
	/* the projection: a few vertices, for their w and depth */
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
		/* (one w for all the vertices looked at: screen space is exact, as
		for the interface's quads, whose w is 1) */
		if (!fitted && have_first)
			projection.perspective = FALSE;
		projection.a = projection_a;
		projection.b = projection_b;
	}
	projection_load(&projection);
	if (device.frames == DRAW_LOG_FRAME)
		draw_log(primitive_type, count, &projection, fetch, context, program, point_size);
	GX_Begin(gx_primitive(primitive_type), DEVICE_VERTEX_FORMAT, (u16)count);
	for (vertex = 0; vertex < count; vertex++)
	{
		fetch(context, vertex, inputs);
		nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size, &out);
		vertex_send(&out, &projection);
	}
	GX_End();
	timing.draws += wl_platform_time_us() - started;
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
	}
	return shader->program;
}

/* whether a draw goes to the screen now (the frame begun, its state set) */
static BOOL draw_prepare(void)
{
	if (!device.gx_ready)
		return FALSE;
	if (device.render_target != &device.back_buffer)
	{
		device.draws_elsewhere++;
		return FALSE;
	}
	frame_begin(device.clear_color);
	states_apply();
	{
		uint64_t started = wl_platform_time_us();
		unsigned stages = textures_bind();
		uint64_t bound = wl_platform_time_us();

		tev_setup(stages);
		timing.textures += bound - started;
		timing.pixel_shaders += wl_platform_time_us() - bound;
	}
	return TRUE;
}

/* vertices [first, first + count) of the streams, or those the indices
name, through the program */
struct stream_fetch
{
	const struct vertex_shader_object *shader;
	const unsigned char *streams[MAXIMUM_STREAMS];
	const WORD *indices;
	UINT first;
};

static void stream_fetch(void *context, UINT vertex, float (*inputs)[4])
{
	const struct stream_fetch *fetch = context;
	UINT index = fetch->indices ? fetch->indices[vertex] + device.base_vertex_index : fetch->first + vertex;
	unsigned long element;

	memset(inputs, 0, NV2A_CPU_INPUT_COUNT * sizeof(*inputs));
	for (element = 0; element < fetch->shader->element_count; element++)
	{
		const struct vertex_element *source = &fetch->shader->elements[element];
		const unsigned char *stream = fetch->streams[source->stream & (MAXIMUM_STREAMS - 1)];

		if (stream)
			element_fetch(stream + index * device.streams[source->stream & (MAXIMUM_STREAMS - 1)].stride +
				source->offset, source->type, inputs[source->reg & 15]);
	}
}

static void draw_streams(D3DPRIMITIVETYPE primitive_type, UINT count, const WORD *indices, UINT first)
{
	struct nv2a_cpu_program *program = program_current();
	struct stream_fetch fetch;
	unsigned long element;

	if (!program || !count || count > MAXIMUM_GX_VERTICES || !draw_prepare())
		return;
	fetch.shader = device.vertex_shader;
	fetch.indices = indices;
	fetch.first = first;
	for (element = 0; element < MAXIMUM_STREAMS; element++)
	{
		D3DVertexBuffer *buffer = device.streams[element].buffer;

		fetch.streams[element] = buffer && buffer->Data ? (const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(buffer->Data) :
			NULL;
	}
	draw_run(program, primitive_type, count, stream_fetch, &fetch);
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
		device.statistics.vertices += device.immediate_vertices;
		program = program_current();
		if (program && device.immediate_buffer && draw_prepare())
			draw_run(program, device.immediate_primitive, (UINT)device.immediate_vertices, immediate_fetch, NULL);
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
	if (device.render_target != &device.back_buffer)
		return;
	/* a frame's first clear is GX's (frame_begin); a later one would be
	drawn over the frame, which the game's own clears do not need yet */
	if ((flags & D3DCLEAR_TARGET) && !device.frame_open)
		device.clear_color = color;
	frame_begin(device.clear_color);
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
		uint64_t started = wl_platform_time_us();

		frame_begin(device.clear_color);
		/* (the device set GX behind WiiLibs' shadow) */
		wl_gx_invalidate_shadow();
		wl_gx_end_frame();
		device.frame_open = FALSE;
		timing.present += wl_platform_time_us() - started;
	}
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
	if (device.frames % STATISTICS_FRAMES == 0)
	{
		platform_log("Direct3D: frame %lu: %lu draws, %lu immediate, %lu vertices, %lu clears, %lu drawn elsewhere, "
			"textures %lu bound %lu failed %lu without a mode, in the last %d", device.frames, device.statistics.draws,
			device.statistics.immediate_draws, device.statistics.vertices, device.statistics.clears,
			device.draws_elsewhere, device.statistics.textures_bound, device.statistics.textures_failed,
			device.statistics.stages_without_mode, STATISTICS_FRAMES);
		platform_log("Direct3D: textures not drawn: %lu without data, %lu of a shape, %lu of a format, "
			"%lu without memory, %lu not decoded; %lu converted bitmaps found", texture_failures[_failure_no_data],
			texture_failures[_failure_shape],
			texture_failures[_failure_format], texture_failures[_failure_memory], texture_failures[_failure_decode],
			textures_native);
		platform_log("Direct3D: pixel shaders: %lu translated, %lu beyond GX; GX FIFO overflows %lu",
			pixel_shaders_translated, pixel_shaders_unusable, (unsigned long)GX_GetOverflowCount());
		{
			uint64_t now = wl_platform_time_us();

			if (timing.window_start)
			{
				platform_log("Direct3D: a frame takes %lu ms: draws %lu (vertex programs and GX), textures %lu, "
					"pixel shaders %lu, the end of the frame %lu (waiting for GX), the rest (the game) %lu",
					(unsigned long)((now - timing.window_start) / 1000 / STATISTICS_FRAMES),
					(unsigned long)(timing.draws / 1000 / STATISTICS_FRAMES),
					(unsigned long)(timing.textures / 1000 / STATISTICS_FRAMES),
					(unsigned long)(timing.pixel_shaders / 1000 / STATISTICS_FRAMES),
					(unsigned long)(timing.present / 1000 / STATISTICS_FRAMES),
					(unsigned long)((now - timing.window_start - timing.draws - timing.textures - timing.pixel_shaders -
						timing.present) / 1000 / STATISTICS_FRAMES));
			}
			memset(&timing, 0, sizeof(timing));
			timing.window_start = now;
		}
		memset(texture_failures, 0, sizeof(texture_failures));
		memset(&device.statistics, 0, sizeof(device.statistics));
		device.draws_elsewhere = 0;
		if (device.frames % (STATISTICS_FRAMES * 4) == 0)
		{
			/* the heaps: MEM1's (newlib's), and WiiLibs' in MEM2 */
			unsigned long used, free_bytes;

			wii_heap_usage(&used, &free_bytes);
			platform_log("memory: MEM1 heap %lu KB used, %lu KB free", used / 1024, free_bytes / 1024);
			wl_mem_log_usage();
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
