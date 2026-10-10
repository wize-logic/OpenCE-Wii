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
#include "port_config.h"
#include "nv2a_cpu.h"
#include "wii_memory.h"
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
/* (the game's own vertices, indexed: draw_hardware) */
#define HARDWARE_VERTEX_FORMAT GX_VTXFMT6
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
	/* what its outputs depend on (nv2a_cpu_analyze), with the program */
	struct nv2a_cpu_dependencies dependencies;
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

/* where a frame's time goes, in microseconds (logged with the statistics) */
struct frame_timing
{
	uint64_t window_start;
	uint64_t draws, textures, pixel_shaders, present;
	/* the draws' time by path: GX's transform (its matrices, then its
	vertices), the processor's, immediate mode */
	uint64_t hardware_setup, hardware_vertices, processor, immediate;
	/* the processor's vertices' steps, in timebase ticks (60.75 a microsecond) */
	unsigned long long fetch_ticks, program_ticks, store_ticks, emit_ticks;
};

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

/* a surface's size: its Size word's (linear), or its format word's
(swizzled, as a texture's level 0) */
static void surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	*width = description.width;
	*height = description.height;
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
static void efb_frame_begun(void);
static void fog_ramp_bind(void);
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
a draw says fewer */
#define EMIT_ALL 0x7F
static unsigned emit_attributes = EMIT_ALL;

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
}

static void frame_begin(D3DCOLOR clear_color)
{
	Mtx identity;

	if (device.frame_open || !device.gx_ready)
		return;
	/* ARGB to the RGBA WiiLibs takes */
	wl_gx_begin_frame((clear_color << 8) | (clear_color >> 24));
	device.frame_open = TRUE;
	efb_frame_begun();

	/* (x and y in pixels from the top left, the depth given as z - 1: GX's
	clip-space depth runs from -1, near, to 0, far; draw_run loads it) */
	projection_valid = FALSE;
	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);

	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_TEX2, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_TEX3, GX_TEX_ST, GX_F32, 0);
	/* (the fog factor, on the fog ramp: fog_coordinate) */
	GX_SetVtxAttrFmt(DEVICE_VERTEX_FORMAT, GX_VA_TEX4, GX_TEX_S, GX_F32, 0);
	gx_psh_initialize();
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

		GX_Begin(GX_TRIANGLES, DEVICE_VERTEX_FORMAT, 3);
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
	GX_DrawDone();
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
	/* (no depth buffer: no depth test, as Direct3D) */
	if (device.depth_stencil)
		GX_SetZMode(D3D__RenderState[D3DRS_ZENABLE] ? GX_TRUE : GX_FALSE, gx_compare(D3D__RenderState[D3DRS_ZFUNC]),
			D3D__RenderState[D3DRS_ZWRITEENABLE] ? GX_TRUE : GX_FALSE);
	else
		GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
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
	/* the cull mode names the screen winding to discard (as the OpenGL
	port reads it); the vertices keep their screen winding on both paths */
	{
		static int cull = -1;

		if (cull < 0)
			cull = config_boolean("debug.gpu_cull") ? 1 : 0;
		if (!cull)
		{
			GX_SetCullMode(GX_CULL_NONE);
			return;
		}
	}
	switch (D3D__RenderState[D3DRS_CULLMODE])
	{
	case D3DCULL_CW: GX_SetCullMode(gx_cull_clockwise); break;
	case D3DCULL_CCW: GX_SetCullMode(gx_cull_clockwise == GX_CULL_FRONT ? GX_CULL_BACK : GX_CULL_FRONT); break;
	default: GX_SetCullMode(GX_CULL_NONE); break;
	}
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
	return hash;
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
	if (entry->texels && !entry->native)
	{
		if (device.frame_open)
		{
			if (pending_texture_free_count == PENDING_TEXTURE_FREES)
			{
				GX_DrawDone();
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

/* ---------- textures drawn by the device

A render target's pixels are copied out of the EFB into its own memory in
GX's format (efb_resolve, below), the same size as the Xbox's: its texture
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
	entry = render_target_texture(texture);
	if (entry)
		return entry;
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
	/* (a texture the game writes as it draws, outside the map's texture
	cache: the glyph cache, whose strings bring their glyphs in just before
	they are drawn, checked at each bind; the others once a frame) */
	if (entry && !entry->native &&
		(entry->used_frame != device.frames || !texture_in_texture_cache(texture)) &&
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
			DCFlushRange(entry->texels, entry->bytes);
			entry->used_frame = device.frames;
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
			DCFlushRange(entry->texels, entry->bytes);
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
/* the stages bound to a cube map the converter made a sphere map: their
texture coordinates are a direction, which sphere_coordinates maps */
static unsigned sphere_stages;

static unsigned textures_bind(void)
{
	unsigned stages = 0;
	DWORD stage;

	GX_InvalidateTexAll();
	sphere_stages = 0;
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
		wrap_s = repeatable && !entry->sphere ? gx_wrap(D3D__TextureState[stage][D3DTSS_ADDRESSU]) : GX_CLAMP;
		wrap_t = repeatable && !entry->sphere ? gx_wrap(D3D__TextureState[stage][D3DTSS_ADDRESSV]) : GX_CLAMP;
		if (entry->sphere)
			sphere_stages |= 1U << stage;
		minification = gx_minification(stage, mipmapped);
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
			GX_InitTexObjCI(&object, entry->texels, (u16)entry->width, (u16)entry->height, GX_TF_CI8, wrap_s, wrap_t,
				mipmapped ? GX_TRUE : GX_FALSE, GX_TLUT0 + stage);
		}
		else
			GX_InitTexObj(&object, entry->texels, (u16)entry->width, (u16)entry->height, entry->gx_format, wrap_s,
				wrap_t, mipmapped ? GX_TRUE : GX_FALSE);
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

/* what the stages set up last read of the vertices: the texture
coordinates (bit n: GX_TEXCOORDn) and the colour channels (bit 0 COLOR0A0,
bit 1 COLOR1A1). draw_hardware leaves an output nothing reads alone */
static unsigned tev_texcoords = 0x1F, tev_channels = 3;

static void tev_setup(unsigned stages)
{
	static int debug = -1;
	struct gx_psh_textures textures;

	/* (all of the vertices' values, until a translation says less) */
	tev_texcoords = 0x1F;
	tev_channels = 3;
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
		fog_ramp_bind();
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
	tev_texcoords = 0x1F;
	tev_channels = 3;
	if (entry->usable)
	{
		gx_psh_apply(&entry->program);
		tev_texcoords = tev_channels = 0;
		for (index = 0; index < entry->program.stage_count; index++)
		{
			const struct gx_psh_stage *tev = &entry->program.stages[index];

			if (tev->texture_map == GX_PSH_FOG_MAP)
				tev_texcoords |= 1U << GX_PSH_FOG_TEXCOORD;
			else if (tev->texture_map < GX_PSH_FOG_MAP)
				tev_texcoords |= 1U << (tev->texture_map & 3);
			if (tev->ras_channel < 2)
				tev_channels |= 1U << tev->ras_channel;
		}
	}
	else
		tev_fallback(stages);
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
	/* (for the draw log: whether this draw's vertices gave the fit) */
	BOOL fitted;
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

/* ---------- fog

The pixel shaders read the fog register: its colour is D3DRS_FOGCOLOR, its
alpha the vertex's fog factor, from the program's oFog through the fog
table (D3DRS_FOGTABLEMODE; Halo's own programs compute the factor, table
none). GX has no such value, so it is sent as a fifth texture coordinate,
which reads the fog ramp (texture map 7): an intensity texture whose value
is its coordinate. The translation (gx_psh.c) reads its alpha. */

static unsigned char fog_ramp[256 * 4] __attribute__((aligned(32)));
static BOOL fog_ramp_made;

static void fog_ramp_bind(void)
{
	GXTexObj ramp;

	if (!fog_ramp_made)
	{
		unsigned x, y;

		/* (I8 in 8x4 tiles: a 256x4 texture, each row the same) */
		for (y = 0; y < 4; y++)
			for (x = 0; x < 256; x++)
				fog_ramp[(x / 8) * 32 + y * 8 + (x % 8)] = (unsigned char)x;
		DCFlushRange(fog_ramp, sizeof(fog_ramp));
		fog_ramp_made = TRUE;
	}
	GX_InitTexObj(&ramp, fog_ramp, 256, 4, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&ramp, GX_NEAR, GX_NEAR);
	GX_LoadTexObj(&ramp, GX_TEXMAP7);
}

static float as_float(DWORD value)
{
	float result;

	memcpy(&result, &value, sizeof(result));
	return result;
}

/* the vertex's fog factor (1 no fog, 0 all fog) as its coordinate on the ramp */
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
	if (!(factor >= 0.0f))
		factor = 0.0f;
	if (factor > 1.0f)
		factor = 1.0f;
	/* (a power of two: the same as the division) */
	return (factor * 255.0f + 0.5f) * (1.0f / 256.0f);
}

/* a cube map's direction (s, t, r) on its sphere map: the mirror ball's
mapping, whose inverse made the map (tools/wii_gx_textures.py's
sphere_directions); only straight behind (0, 0, -1) has no place, and
reads the disc's edge */
static void sphere_coordinates(const float direction[4], float out[2])
{
	float x = direction[0], y = direction[1], z = direction[2];
	float length = sqrtf(x * x + y * y + z * z), m;

	if (!(length > 1.0e-12f))
	{
		out[0] = out[1] = 0.5f;
		return;
	}
	x /= length;
	y /= length;
	z /= length;
	m = 2.0f * sqrtf(x * x + y * y + (z + 1.0f) * (z + 1.0f));
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
	float position[3];
	u8 color0[4], color1[4];
	float texcoord[4][2];
	float fog;
};

static void vertex_store(const struct nv2a_cpu_output *out, const struct draw_projection *projection,
	const float *raw_position, struct gx_vertex *vertex)
{
	/* the program's screen position (pixel centres on integers, as Direct3D
	puts them), its depth scaled back to 0..1 */
	float depth = out->position[2] / 16777215.0f;
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
	{
		const float *scale = device.vertex_constants[VERTEX_CONSTANT_BIAS - 38];
		const float *offset = device.vertex_constants[VERTEX_CONSTANT_BIAS - 37];
		float w = out->clip[3];

		vertex->position[0] = out->clip[0] * scale[0] + (offset[0] + 0.5f) * w;
		vertex->position[1] = out->clip[1] * scale[1] + (offset[1] + 0.5f) * w;
		vertex->position[2] = -w;
	}
	else
	{
		if (!(depth >= 0.0f))
			depth = 0.0f;
		if (depth > 1.0f)
			depth = 1.0f;
		vertex->position[0] = out->position[0] + 0.5f;
		vertex->position[1] = out->position[1] + 0.5f;
		vertex->position[2] = depth - 1.0f;
	}
	for (stage = 0; stage < 4; stage++)
	{
		vertex->color0[stage] = color_byte(out->d0[stage]);
		vertex->color1[stage] = color_byte(out->d1[stage]);
		if (sphere_stages & (1U << stage))
			sphere_coordinates(out->t[stage], vertex->texcoord[stage]);
		else
		{
			vertex->texcoord[stage][0] = out->t[stage][0] * texcoord_scale[stage][0];
			vertex->texcoord[stage][1] = out->t[stage][1] * texcoord_scale[stage][1];
		}
	}
	vertex->fog = fog_coordinate(out->fog);
}

static inline void vertex_emit(const struct gx_vertex *vertex)
{
	int stage;

	if (emit_matrix_index)
		GX_MatrixIndex1x8(vertex->matrix);
	GX_Position3f32(vertex->position[0], vertex->position[1], vertex->position[2]);
	if (emit_attributes == EMIT_ALL)
	{
		GX_Color4u8(vertex->color0[0], vertex->color0[1], vertex->color0[2], vertex->color0[3]);
		GX_Color4u8(vertex->color1[0], vertex->color1[1], vertex->color1[2], vertex->color1[3]);
		for (stage = 0; stage < 4; stage++)
			GX_TexCoord2f32(vertex->texcoord[stage][0], vertex->texcoord[stage][1]);
		GX_TexCoord1f32(vertex->fog);
		return;
	}
	if (emit_attributes & 1)
		GX_Color4u8(vertex->color0[0], vertex->color0[1], vertex->color0[2], vertex->color0[3]);
	if (emit_attributes & 2)
		GX_Color4u8(vertex->color1[0], vertex->color1[1], vertex->color1[2], vertex->color1[3]);
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
	for (index = 0; index < 7; index++)
		if (!(wanted & (1U << index)))
			GX_SetVtxDesc(attributes[index], GX_NONE);
	emit_attributes = wanted;
	transform_hardware = TRUE;
}

static void vertex_send(const struct nv2a_cpu_output *out, const struct draw_projection *projection)
{
	struct gx_vertex vertex;

	vertex_store(out, projection, NULL, &vertex);
	vertex_emit(&vertex);
}

/* an indexed draw's vertices, each run through its program once however
many of its indices name it (a strip names most twice): kept by index less
the draw's least, for draws whose indices span no more than this */
#define VERTEX_MEMO_SIZE 4096
static struct gx_vertex *vertex_memo;
static unsigned long *vertex_memo_stamps;
static unsigned long vertex_memo_draw;

static u8 gx_primitive(D3DPRIMITIVETYPE type);

/* one frame's draws described in the log (DRAW_LOG_FRAME; 0: none) */
#ifndef DRAW_LOG_FRAME
#define DRAW_LOG_FRAME 0
#endif
/* the pixel the logged frame reads back after each draw */
#ifndef DRAW_LOG_X
#define DRAW_LOG_X 320
#define DRAW_LOG_Y 260
#endif

static void draw_log(D3DPRIMITIVETYPE primitive_type, UINT count, const struct draw_projection *projection,
	void (*fetch)(void *, UINT, float (*)[4]), void *context, struct nv2a_cpu_program *program, float point_size)
{
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
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
	platform_log("draw %lu: prim %d x%u %s screen %.0f,%.0f-%.0f,%.0f d0 %.2f %.2f %.2f %.2f | blend %lu %lu>%lu atest %lu %lu/%lu z %lu/%lu/%lu wr %lx",
		device.statistics.draws + device.statistics.immediate_draws, (int)primitive_type, count,
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
		const struct pixel_shader_entry *shader = pixel_shader_last < pixel_shader_cache_count ?
			&pixel_shader_cache[pixel_shader_last] : NULL;
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
			!shader ? "?" : !shader->usable ? "beyond GX (texture times colour)" :
				shader->program.approximate ? "approximate: " : "exact",
			shader && shader->usable && shader->program.approximate && shader->program.approximation ?
				shader->program.approximation : "",
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
identity of the draws in screen space) */
#define MATRIX_KEYS 9
static struct
{
	unsigned count;
	/* a vertex of each value, of the draw's */
	UINT representative[MATRIX_KEYS];
	Mtx rows[MATRIX_KEYS];
} draw_keys;
static unsigned char vertex_keys[65536];

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
(as element_fetch gives it) */
static void matrix_inputs(const struct vertex_shader_object *shader, float (*inputs)[4])
{
	unsigned long index;

	memset(inputs, 0, NV2A_CPU_INPUT_COUNT * sizeof(*inputs));
	for (index = 0; index < shader->element_count; index++)
		inputs[shader->elements[index].reg & 15][3] = 1.0f;
	for (index = 0; index < NV2A_CPU_INPUT_COUNT; index++)
		if (draw_uniform.mask & (1UL << index))
			memcpy(inputs[index], draw_uniform.values[index], sizeof(inputs[index]));
}

static Mtx position_matrix_rows;
/* the depth's a + b / w for the same constants (position_matrix): from the
program's outputs for the origin and the axes, not from a draw's vertices,
so every draw of these constants has the same, to the bit */
static struct draw_projection position_projection;
/* (config.toml's debug.gpu_transform; -1 not read yet) */
static int gpu_transform = -1;

/* the matrices and depths of recent constants, by the program, its vertex
declaration, the constants it reads and the viewport's */
#define POSITION_CACHE_ENTRIES 64
static struct
{
	unsigned long long key;
	Mtx rows;
	struct draw_projection projection;
} position_cache[POSITION_CACHE_ENTRIES];

static unsigned long long constants_key(const struct vertex_shader_object *shader, const struct nv2a_cpu_program *program)
{
	const struct nv2a_cpu_dependencies *dependencies = &shader->dependencies;
	unsigned long long hash = 14695981039346656037ULL ^ (unsigned long)program;
	unsigned long row, word;

	hash = (hash ^ (unsigned long)shader) * 1099511628211ULL;
	hash = (hash ^ draw_uniform.mask) * 1099511628211ULL;
	for (row = 0; row < NV2A_CPU_INPUT_COUNT; row++)
		if (draw_uniform.mask & (1UL << row))
		{
			const unsigned long *words = (const unsigned long *)draw_uniform.values[row];

			for (word = 0; word < 4; word++)
				hash = (hash ^ words[word]) * 1099511628211ULL;
		}
	for (row = 0; row < NV2A_CPU_CONSTANT_COUNT; row++)
		if (dependencies->relative || (dependencies->constants[row / 32] & (1UL << (row % 32))) ||
			row == VERTEX_CONSTANT_BIAS - 38 || row == VERTEX_CONSTANT_BIAS - 37)
		{
			const unsigned long *words = (const unsigned long *)device.vertex_constants[row];

			hash = (hash ^ row) * 1099511628211ULL;
			for (word = 0; word < 4; word++)
				hash = (hash ^ words[word]) * 1099511628211ULL;
		}
	return hash ? hash : 1;
}

static BOOL position_matrix_compute(const struct vertex_shader_object *shader, struct nv2a_cpu_program *program,
	int reg, float point_size);

static BOOL position_matrix(const struct vertex_shader_object *shader, struct nv2a_cpu_program *program, int reg,
	float point_size)
{
	unsigned long long key = constants_key(shader, program);
	unsigned slot = (unsigned)(key % POSITION_CACHE_ENTRIES);

	if (position_cache[slot].key == key)
	{
		memcpy(position_matrix_rows, position_cache[slot].rows, sizeof(position_matrix_rows));
		position_projection = position_cache[slot].projection;
		return TRUE;
	}
	if (!position_matrix_compute(shader, program, reg, point_size))
		return FALSE;
	position_cache[slot].key = key;
	memcpy(position_cache[slot].rows, position_matrix_rows, sizeof(position_matrix_rows));
	position_cache[slot].projection = position_projection;
	return TRUE;
}

static BOOL position_matrix_compute(const struct vertex_shader_object *shader, struct nv2a_cpu_program *program,
	int reg, float point_size)
{
	float ws[4];
	const float *scale = device.vertex_constants[VERTEX_CONSTANT_BIAS - 38];
	const float *offset = device.vertex_constants[VERTEX_CONSTANT_BIAS - 37];
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	struct nv2a_cpu_output base, column;
	int component;

	matrix_inputs(shader, inputs);
	nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size, &base);
	for (component = 0; component < 4; component++)
	{
		float x = base.clip[0], y = base.clip[1], w = base.clip[3];

		if (component < 3)
		{
			inputs[reg][component] = 1.0f;
			nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size,
				&column);
			inputs[reg][component] = 0.0f;
			x = column.clip[0] - base.clip[0];
			y = column.clip[1] - base.clip[1];
			w = column.clip[3] - base.clip[3];
		}
		position_matrix_rows[0][component] = x * scale[0] + (offset[0] + 0.5f) * w;
		position_matrix_rows[1][component] = y * scale[1] + (offset[1] + 0.5f) * w;
		position_matrix_rows[2][component] = -w;
		ws[component] = component < 3 ? column.clip[3] : base.clip[3];
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
		if (!(length > 1.0e-12f))
			return FALSE;
		for (point = 0; point < 2; point++)
		{
			float t = (targets[point] - ws[3]) / length;

			for (component = 0; component < 3; component++)
				inputs[reg][component] = t * gradient[component];
			nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size,
				&column);
			d[point] = column.position[2] / 16777215.0f;
			v[point] = column.clip[3];
		}
		for (component = 0; component < 3; component++)
			inputs[reg][component] = 0.0f;
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
	if (draw_keys.count > 1)
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
in position_matrix_rows and position_projection, and in draw_uniform */
static BOOL position_keys(const struct vertex_shader_object *shader, struct nv2a_cpu_program *program, void *fetch,
	int reg, float point_size)
{
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	unsigned key;
	int index;

	for (key = draw_keys.count; key-- > 0; )
	{
		stream_fetch(fetch, draw_keys.representative[key], inputs);
		for (index = 0; index < NV2A_CPU_INPUT_COUNT; index++)
			if (draw_uniform.mask & (1UL << index))
				memcpy(draw_uniform.values[index], inputs[index], sizeof(inputs[index]));
		if (!position_matrix(shader, program, reg, point_size))
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
	uint64_t started = wl_platform_time_us();
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	struct nv2a_cpu_output out;
	struct draw_projection projection = { FALSE, 0.0f, 0.0f };
	float point_size;
	UINT vertex;

	memcpy(&point_size, &D3D__RenderState[D3DRS_POINTSIZE], sizeof(point_size));
	if (gpu_transform < 0)
		gpu_transform = config_boolean("debug.gpu_transform") ? 1 : 0;
	if (shader && gpu_transform)
	{
		position_register = position_register_of(shader,
			draw_dependencies((struct vertex_shader_object *)shader, program, (struct stream_fetch *)context, count));
		if (position_register >= 0 && (draw_keys.count > 1 ?
			position_keys(shader, program, context, position_register, point_size) :
			position_matrix(shader, program, position_register, point_size)))
			projection = position_projection;
		else
			position_register = -1;
	}
	if (position_register < 0)
		projection = projection_fit(program, count, fetch, context, point_size);
	projection_load(&projection);
	if (DRAW_LOG_FRAME && device.frames == DRAW_LOG_FRAME)
		draw_log(primitive_type, count, &projection, fetch, context, program, point_size);
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
				emit_matrix_index = draw_keys.count > 1;
			}
			if (shader)
				emit_attributes_set();

			GX_Begin(gx_primitive(primitive_type), DEVICE_VERTEX_FORMAT, (u16)count);
			for (vertex = 0; vertex < count; vertex++)
			{
				unsigned slot = indices[vertex] - least;

				if (vertex_memo_stamps[slot] != vertex_memo_draw)
				{
					u32 t0 = PPCMftb(), t1, t2;

					fetch(context, vertex, inputs);
					t1 = PPCMftb();
					nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs,
						point_size, &out);
					t2 = PPCMftb();
					vertex_store(&out, &projection, position_register >= 0 ? inputs[position_register] : NULL,
						&vertex_memo[slot]);
					vertex_memo[slot].matrix = (u8)(GX_PNMTX1 + 3 * vertex_keys[vertex]);
					vertex_memo_stamps[slot] = vertex_memo_draw;
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
			emit_matrix_index = draw_keys.count > 1;
		}
		if (shader)
			emit_attributes_set();

		GX_Begin(gx_primitive(primitive_type), DEVICE_VERTEX_FORMAT, (u16)count);
		for (vertex = 0; vertex < count; vertex++)
		{
			struct gx_vertex sent;

			sent.matrix = (u8)(GX_PNMTX1 + 3 * vertex_keys[vertex]);
			fetch(context, vertex, inputs);
			nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size,
				&out);
			vertex_store(&out, &projection, position_register >= 0 ? inputs[position_register] : NULL, &sent);
			vertex_emit(&sent);
		}
		device.statistics.vertices_run += count;
		GX_End();
	}
	/* (the logged frame: a pixel read back after each draw, to see which
	draw makes it what it is) */
	if (DRAW_LOG_FRAME && device.frames == DRAW_LOG_FRAME && device.render_target == &device.back_buffer)
	{
		GXColor pixel;

		GX_DrawDone();
		GX_PeekARGB(DRAW_LOG_X, DRAW_LOG_Y, &pixel);
		platform_log("  pixel (%d, %d) after: %u %u %u %u", DRAW_LOG_X, DRAW_LOG_Y, pixel.r, pixel.g, pixel.b, pixel.a);
	}
	program_statistics_add(program, count);
	{
		uint64_t spent = wl_platform_time_us() - started;

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
	}
	return shader->program;
}

/* ---------- the EFB's owner

GX draws into one frame buffer, the EFB. The back buffer has it for the
frame; a draw or clear into another render target takes it over:

- the back buffer, if anything was drawn into it, is parked first: its
  colour and depth copied out (efb_park);
- the target is drawn at the EFB's origin, within a scissor of its size;
- when another surface takes the EFB, the target's pixels are copied into
  its own memory in GX's format and recorded (efb_resolve), so that its
  texture is bound from there;
- the back buffer is put back (efb_restore) by a quad that writes the
  parked colour, and the parked depth through a Z texture.

A clear once the frame has begun is a quad of its colour and depth
(efb_clear_quad). */

static struct
{
	/* the surface the EFB holds: the back buffer's, or a render target's */
	D3DSurface *owner;
	/* the back buffer has had something drawn or cleared this frame */
	BOOL back_buffer_drawn;
	/* the back buffer's colour and depth are parked */
	BOOL parked;
	void *parked_color, *parked_depth;
	unsigned long parks, resolves, skipped;
} efb;

static unsigned long efb_buffer_bytes(void)
{
	return (unsigned long)SCREEN_WIDTH * SCREEN_HEIGHT * 4;
}

/* texture copies unfiltered (the display copy's deflicker blurs), and the
display copy's filter back after */
static void efb_copy_filter(BOOL display)
{
	GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);

	if (display && mode)
		GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
	else
		GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);
}

/* a rectangle of the EFB copied out as a texture (colour, or depth) */
static void efb_copy(void *destination, unsigned long width, unsigned long height, u8 format)
{
	unsigned long bytes = wl_gxc_tex_bytes(format == GX_TF_RGB565 ? WL_GXC_RGB565 : WL_GXC_RGBA8, (uint32_t)width,
		(uint32_t)height);

	DCInvalidateRange(destination, bytes);
	GX_SetTexCopySrc(0, 0, (u16)width, (u16)height);
	GX_SetTexCopyDst((u16)width, (u16)height, format, GX_FALSE);
	GX_CopyTex(destination, GX_FALSE);
	GX_PixModeSync();
}

static void efb_park(void)
{
	if (!efb.parked_color)
	{
		/* (in the contiguous window, where there is room; else the heap) */
		efb.parked_color = platform_contiguous_alloc(efb_buffer_bytes(), 32, PLATFORM_ANY_PHYSICAL_ADDRESS, 0);
		efb.parked_depth = platform_contiguous_alloc(efb_buffer_bytes(), 32, PLATFORM_ANY_PHYSICAL_ADDRESS, 0);
		if (!efb.parked_color || !efb.parked_depth)
		{
			if (efb.parked_color)
				platform_contiguous_free(efb.parked_color);
			if (efb.parked_depth)
				platform_contiguous_free(efb.parked_depth);
			efb.parked_color = aligned_alloc(32, efb_buffer_bytes());
			efb.parked_depth = aligned_alloc(32, efb_buffer_bytes());
		}
		if (!efb.parked_color || !efb.parked_depth)
		{
			free(efb.parked_color);
			free(efb.parked_depth);
			efb.parked_color = efb.parked_depth = NULL;
			platform_log("Direct3D: no memory to park the back buffer: render targets draw over it");
			return;
		}
	}
	efb_copy_filter(FALSE);
	efb_copy(efb.parked_color, SCREEN_WIDTH, SCREEN_HEIGHT, GX_TF_RGBA8);
	efb_copy(efb.parked_depth, SCREEN_WIDTH, SCREEN_HEIGHT, GX_TF_Z24X8);
	efb_copy_filter(TRUE);
	efb.parked = TRUE;
	efb.parks++;
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

static void efb_resolve(D3DSurface *surface)
{
	struct render_target_record *record;
	unsigned long width, height;
	u8 format = render_target_gx_format(surface);

	surface_dimensions(surface, &width, &height);
	if (!format || !surface->Data || width > SCREEN_WIDTH || height > SCREEN_HEIGHT || (width & 3) || (height & 3))
	{
		efb.skipped++;
		return;
	}
	efb_copy_filter(FALSE);
	efb_copy(PLATFORM_PHYSICAL_TO_VIRTUAL(surface->Data), width, height, format);
	efb_copy_filter(TRUE);
	record = render_target_record_find(surface->Data);
	if (!record)
	{
		record = &render_target_records[render_target_record_next];
		render_target_record_next = (render_target_record_next + 1) % RENDER_TARGET_RECORDS;
	}
	record->data = surface->Data;
	record->width = width;
	record->height = height;
	record->gx_format = format;
	efb.resolves++;
}

/* a quad over a rectangle of the EFB: its colour and alpha from texture
map 0 or a constant colour, its depth from texture map 1 (a Z texture) or a
constant depth */
static void efb_quad(float x0, float y0, float x1, float y1, const GXColor *color, float depth, BOOL textured)
{
	Mtx44 matrix;
	int corner;
	static const float corners[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

	memset(matrix, 0, sizeof(matrix));
	matrix[0][0] = 2.0f / SCREEN_WIDTH;
	matrix[0][3] = -1.0f;
	matrix[1][1] = -2.0f / SCREEN_HEIGHT;
	matrix[1][3] = 1.0f;
	matrix[2][2] = 1.0f;
	matrix[3][3] = 1.0f;
	GX_LoadProjectionMtx(matrix, GX_ORTHOGRAPHIC);
	projection_valid = FALSE;
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetNumTevStages(textured ? 2 : 1);
	if (textured)
	{
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
		GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
		/* (the Z texture is the last stage's texture; the colour passes) */
		GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
		GX_SetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
		GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
		GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetZTexture(GX_ZT_REPLACE, GX_TF_Z24X8, 0);
		GX_SetZCompLoc(GX_FALSE);
	}
	else
	{
		GX_SetTevColor(GX_TEVREG0, *color);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
		GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	}
	transform_software();

	GX_Begin(GX_QUADS, DEVICE_VERTEX_FORMAT, 4);
	for (corner = 0; corner < 4; corner++)
	{
		int stage;

		GX_Position3f32(x0 + (x1 - x0) * corners[corner][0], y0 + (y1 - y0) * corners[corner][1], depth - 1.0f);
		GX_Color4u8(255, 255, 255, 255);
		GX_Color4u8(255, 255, 255, 255);
		for (stage = 0; stage < 4; stage++)
			GX_TexCoord2f32(corners[corner][0], corners[corner][1]);
		GX_TexCoord1f32(1.0f);
	}
	GX_End();
	if (textured)
	{
		GX_SetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
		GX_SetZCompLoc(GX_TRUE);
	}
}

static void efb_restore(void)
{
	GXTexObj color, depth;

	GX_SetScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	if (!efb.parked)
		return;
	GX_InvalidateTexAll();
	GX_InitTexObj(&color, efb.parked_color, SCREEN_WIDTH, SCREEN_HEIGHT, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&color, GX_NEAR, GX_NEAR);
	GX_LoadTexObj(&color, GX_TEXMAP0);
	GX_InitTexObj(&depth, efb.parked_depth, SCREEN_WIDTH, SCREEN_HEIGHT, GX_TF_Z24X8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&depth, GX_NEAR, GX_NEAR);
	GX_LoadTexObj(&depth, GX_TEXMAP1);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
	efb_quad(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, NULL, 0.0f, TRUE);
	efb.parked = FALSE;
}

/* the EFB for the current render target */
static void efb_take(void)
{
	D3DSurface *target = device.render_target;

	if (!efb.owner)
		efb.owner = &device.back_buffer;
	if (target == efb.owner)
		return;
	/* the one leaving */
	if (efb.owner == &device.back_buffer)
	{
		if (efb.back_buffer_drawn && !efb.parked)
			efb_park();
	}
	else
	{
		efb_resolve(efb.owner);
	}
	/* the one coming */
	if (target == &device.back_buffer)
	{
		efb_restore();
	}
	else
	{
		unsigned long width, height;

		surface_dimensions(target, &width, &height);
		GX_SetScissor(0, 0, (u32)(width < SCREEN_WIDTH ? width : SCREEN_WIDTH),
			(u32)(height < SCREEN_HEIGHT ? height : SCREEN_HEIGHT));
	}
	efb.owner = target;
}

/* a new frame: the back buffer has the EFB, and nothing drawn */
static void efb_frame_begun(void)
{
	efb.owner = &device.back_buffer;
	efb.back_buffer_drawn = FALSE;
	efb.parked = FALSE;
}

/* a clear of the current target's viewport, once the frame has begun */
static void efb_clear_quad(DWORD flags, D3DCOLOR color, float z)
{
	GXColor gx_color = { (u8)(color >> 16), (u8)(color >> 8), (u8)color, (u8)(color >> 24) };
	float x0 = (float)device.viewport.X, y0 = (float)device.viewport.Y;

	GX_SetColorUpdate((flags & D3DCLEAR_TARGET) ? GX_TRUE : GX_FALSE);
	GX_SetAlphaUpdate((flags & D3DCLEAR_TARGET) ? GX_TRUE : GX_FALSE);
	GX_SetZMode((flags & D3DCLEAR_ZBUFFER) ? GX_TRUE : GX_FALSE, GX_ALWAYS, (flags & D3DCLEAR_ZBUFFER) ? GX_TRUE : GX_FALSE);
	GX_SetZCompLoc(GX_TRUE);
	efb_quad(x0, y0, x0 + (float)device.viewport.Width, y0 + (float)device.viewport.Height, &gx_color,
		z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z, FALSE);
}

/* whether a draw goes to the screen now (the frame begun, its state set) */
static BOOL draw_prepare(void)
{
	if (!device.gx_ready)
		return FALSE;
	frame_begin(device.clear_color);
	efb_take();
	if (device.render_target == &device.back_buffer)
		efb.back_buffer_drawn = TRUE;
	else
		device.draws_elsewhere++;
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

	/* (each element sets its register's four components; the registers no
	element names stay as the caller cleared them, once a draw) */
	for (element = 0; element < fetch->shader->element_count; element++)
	{
		const struct vertex_element *source = &fetch->shader->elements[element];
		const unsigned char *stream = fetch->streams[source->stream & (MAXIMUM_STREAMS - 1)];

		if (stream)
			element_fetch(stream + index * device.streams[source->stream & (MAXIMUM_STREAMS - 1)].stride +
				source->offset, source->type, inputs[source->reg & 15]);
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

static unsigned long hardware_draws, hardware_vertices, hardware_colors_run, hardware_keyed_draws;

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

/* what a draw's program depends on: its analysis, or, where its clip-space
position is not affine but the inputs it is not affine in (other than a
position of three floats) hold the same values in every vertex the draw
names (a skinned model's part bound to one node: its node indices and
weights), its analysis with those as constants, whose values draw_uniform
keeps for the matrices */
static const struct nv2a_cpu_dependencies *draw_dependencies(struct vertex_shader_object *shader,
	struct nv2a_cpu_program *program, struct stream_fetch *fetch, UINT count)
{
	const struct nv2a_cpu_dependencies *general = &shader->dependencies;
	unsigned long clip = general->clip[0] | general->clip[1] | general->clip[3], candidates = 0;
	float inputs[NV2A_CPU_INPUT_COUNT][4] = { { 0.0f } };
	unsigned index;
	int reg;

	draw_uniform.mask = 0;
	draw_keys.count = 0;
	if (!fetch || !count || !general->clip_captured || !(clip & NV2A_CPU_NOT_AFFINE))
		return general;
	for (reg = 0; reg < NV2A_CPU_INPUT_COUNT; reg++)
	{
		const struct vertex_element *element;

		if (!(clip & (1UL << reg)))
			continue;
		element = element_of(shader, reg);
		if (!element || !fetch->streams[element->stream & (MAXIMUM_STREAMS - 1)] || !element->bytes)
			return general;
		if (element->type != D3DVSDT_FLOAT3)
			candidates |= 1UL << reg;
	}
	if (!candidates)
		return general;
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
					return general;
				}
				wholes[key] = whole;
				draw_keys.representative[key] = vertex;
				draw_keys.count++;
			}
			vertex_keys[vertex] = (unsigned char)key;
		}
		(void)regs;
	}
	stream_fetch(fetch, 0, inputs);
	for (reg = 0; reg < NV2A_CPU_INPUT_COUNT; reg++)
		if (candidates & (1UL << reg))
			memcpy(draw_uniform.values[reg], inputs[reg], sizeof(inputs[reg]));
	draw_uniform.mask = candidates;
	for (index = 0; index < shader->specialized_count; index++)
		if (shader->specialized[index].uniform == candidates)
			return &shader->specialized[index].dependencies;
	index = shader->specialized_count < 2 ? shader->specialized_count++ : 1;
	shader->specialized[index].uniform = candidates;
	nv2a_cpu_analyze_uniform(program, candidates, &shader->specialized[index].dependencies);
	return &shader->specialized[index].dependencies;
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

static unsigned long long hash_words(unsigned long long hash, const void *data, unsigned long bytes)
{
	const unsigned long *words = data;
	unsigned long index;

	for (index = 0; index < bytes / 4; index++)
		hash = (hash ^ words[index]) * 1099511628211ULL;
	return hash;
}

static unsigned long long hardware_key(const struct vertex_shader_object *shader, const struct nv2a_cpu_program *program)
{
	const struct nv2a_cpu_dependencies *dependencies = &shader->dependencies;
	unsigned long long hash = 14695981039346656037ULL;
	unsigned long state[6], row;

	state[0] = (unsigned long)program;
	state[1] = (unsigned long)shader;
	state[2] = tev_texcoords | sphere_stages << 8;
	state[3] = D3D__RenderState[D3DRS_FOGTABLEMODE];
	state[4] = D3D__RenderState[D3DRS_FOGSTART];
	state[5] = D3D__RenderState[D3DRS_FOGEND];
	hash = hash_words(hash, state, sizeof(state));
	hash = hash_words(hash, texcoord_scale, sizeof(texcoord_scale));
	hash = hash_words(hash, &draw_uniform.mask, sizeof(draw_uniform.mask));
	for (row = 0; row < NV2A_CPU_INPUT_COUNT; row++)
		if (draw_uniform.mask & (1UL << row))
			hash = hash_words(hash, draw_uniform.values[row], sizeof(draw_uniform.values[row]));
	for (row = 0; row < NV2A_CPU_CONSTANT_COUNT; row++)
		if (dependencies->relative || (dependencies->constants[row / 32] & (1UL << (row % 32))) ||
			row == VERTEX_CONSTANT_BIAS - 38 || row == VERTEX_CONSTANT_BIAS - 37)
			hash = hash_words(hash ^ row, device.vertex_constants[row], sizeof(device.vertex_constants[row]));
	/* (0 is an empty entry's) */
	return hash ? hash : 1;
}

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
	unsigned long clip_mask, least = 0xFFFFFFFFUL, most = 0, vertex, color_mask[2];
	float stage_rows[5][2][4], point_size;
	struct draw_projection projection;
	BOOL fogged = FALSE, cpu_colors = FALSE;
	unsigned long long key;
	struct hardware_matrices *cached;
	uint64_t started = wl_platform_time_us(), emitting;
	float fog_base = 0.0f, fog_slope = 0.0f;

	if (gpu_transform < 0)
		gpu_transform = config_boolean("debug.gpu_transform") ? 1 : 0;
	dependencies = draw_dependencies(shader, program, fetch, count);
	if (!gpu_transform || !dependencies->clip_captured || primitive_type == D3DPT_POINTLIST)
		return hardware_rejected(program, "transform off, no clip-space position, or points");
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
			/* (a direction the vertices give is the processor's; one the
			same for every vertex is one point of the sphere map) */
			if ((dependencies->t[stage][0] | dependencies->t[stage][1] | dependencies->t[stage][2]) & 0xFFFF)
				return hardware_rejected(program, "a cube map's direction (a sphere map's coordinates)");
			continue;
		}
		if (mask & NV2A_CPU_NOT_AFFINE)
			return hardware_rejected(program, "a texture coordinate read is not affine");
		if (!(mask & 0xFFFF) || reg == position_register)
			continue;
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
	if (draw_keys.count > 1)
	{
		const struct nv2a_cpu_dependencies *general = &shader->dependencies;

		for (stage = 0; stage < 4; stage++)
			if (!stage_unread[stage] && ((general->t[stage][0] | general->t[stage][1]) & draw_uniform.mask))
				return hardware_rejected(program, "a texture coordinate from a model's nodes");
		if (fogged && (general->fog & draw_uniform.mask))
			return hardware_rejected(program, "the fog from a model's nodes");
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
	/* the vertices' range: GX's indices are 16 bits */
	for (vertex = 0; vertex < count; vertex++)
	{
		unsigned long index = indices ? (unsigned long)indices[vertex] + device.base_vertex_index : first + vertex;

		if (index < least)
			least = index;
		if (index > most)
			most = index;
	}
	if (most > 0xFFFF)
		return hardware_rejected(program, "an index past 16 bits");

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
	matrix_inputs(shader, inputs);
	nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size, &base);
	for (component = 0; component < 4; component++)
	{
		if (component < 3)
		{
			inputs[position_register][component] = 1.0f;
			nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs, point_size,
				&column);
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
			nv2a_cpu_run(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs,
				point_size, &column);
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
	if (!position_matrix(shader, program, position_register, point_size))
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
	if (draw_keys.count > 1 && !position_keys(shader, program, fetch, position_register, point_size))
		return hardware_rejected(program, "no depth to fit for a model's node");
	GX_ClearVtxDesc();
	position_matrix_load();
	GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
	GX_SetVtxAttrFmt(HARDWARE_VERTEX_FORMAT, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	{
		const unsigned char *stream = fetch->streams[position_element->stream & (MAXIMUM_STREAMS - 1)];
		UINT stride = device.streams[position_element->stream & (MAXIMUM_STREAMS - 1)].stride;

		if (stride > 255)
			return FALSE;
		GX_SetArray(GX_VA_POS, (void *)(stream + position_element->offset), (u8)stride);
		DCFlushRange((void *)(stream + least * stride), (most - least + 1) * stride);
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
			GX_SetVtxAttrFmt(HARDWARE_VERTEX_FORMAT, GX_VA_TEX0 + component, GX_TEX_ST, GX_S16, 15);
		else
			GX_SetVtxAttrFmt(HARDWARE_VERTEX_FORMAT, GX_VA_TEX0 + component, GX_TEX_ST, GX_F32, 0);
		GX_SetArray(GX_VA_TEX0 + component, (void *)(stream + element->offset), (u8)stride);
		DCFlushRange((void *)(stream + least * stride), (most - least + 1) * stride);
	}
	GX_InvVtxCache();
	for (stage = 0; stage < 5; stage++)
	{
		Mtx rows;

		memset(rows, 0, sizeof(rows));
		memcpy(rows, stage_rows[stage], sizeof(stage_rows[stage]));
		GX_LoadTexMtxImm(rows, GX_TEXMTX0 + 3 * stage, GX_MTX2x4);
		GX_SetTexCoordGen(GX_TEXCOORD0 + stage, GX_TG_MTX2x4,
			stage < 4 && stage_source[stage] >= 0 ? GX_TG_TEX0 + stage_source[stage] : GX_TG_POS, GX_TEXMTX0 + 3 * stage);
	}
	if (cpu_colors)
	{
		/* the colours: the processor's, sent with each vertex */
		GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
		GX_SetVtxAttrFmt(HARDWARE_VERTEX_FORMAT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
		GX_SetVtxAttrFmt(HARDWARE_VERTEX_FORMAT, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
		GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
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
		vertex_memo_draw++;
	}
	else
	{
		/* the colours: constant */
		GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){ color_byte(base.d0[0]), color_byte(base.d0[1]),
			color_byte(base.d0[2]), color_byte(base.d0[3]) });
		GX_SetChanMatColor(GX_COLOR1A1, (GXColor){ color_byte(base.d1[0]), color_byte(base.d1[1]),
			color_byte(base.d1[2]), color_byte(base.d1[3]) });
	}

	emitting = wl_platform_time_us();
	GX_Begin(gx_primitive(primitive_type), HARDWARE_VERTEX_FORMAT, (u16)count);
	for (vertex = 0; vertex < count; vertex++)
	{
		unsigned long whole = indices ? (unsigned long)indices[vertex] + device.base_vertex_index : first + vertex;
		u16 index = (u16)whole;

		if (draw_keys.count > 1)
			GX_MatrixIndex1x8((u8)(GX_PNMTX1 + 3 * vertex_keys[vertex]));
		GX_Position1x16(index);
		if (cpu_colors)
		{
			struct gx_vertex local, *colors = &local;
			unsigned long slot = whole - least;

			if (vertex_memo && slot < VERTEX_MEMO_SIZE)
				colors = &vertex_memo[slot];
			if (colors == &local || vertex_memo_stamps[slot] != vertex_memo_draw)
			{
				struct nv2a_cpu_output out;

				stream_fetch(fetch, vertex, inputs);
				nv2a_cpu_run_colors(program, (const float (*)[4])device.vertex_constants, (const float (*)[4])inputs,
					&out);
				for (component = 0; component < 4; component++)
				{
					colors->color0[component] = color_byte(out.d0[component]);
					colors->color1[component] = color_byte(out.d1[component]);
				}
				if (colors != &local)
					vertex_memo_stamps[slot] = vertex_memo_draw;
				hardware_colors_run++;
			}
			GX_Color4u8(colors->color0[0], colors->color0[1], colors->color0[2], colors->color0[3]);
			GX_Color4u8(colors->color1[0], colors->color1[1], colors->color1[2], colors->color1[3]);
		}
		for (component = 0; component < texture_count; component++)
			GX_TexCoord1x16(index);
	}
	GX_End();
	hardware_draws++;
	hardware_vertices += count;
	if (draw_keys.count > 1)
		hardware_keyed_draws++;
	timing.hardware_setup += emitting - started;
	timing.hardware_vertices += wl_platform_time_us() - emitting;
	return TRUE;
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
	{
		uint64_t started = wl_platform_time_us();

		if (draw_hardware(program, primitive_type, count, indices, first, &fetch))
		{
			timing.draws += wl_platform_time_us() - started;
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
		efb.back_buffer_drawn = TRUE;
	if (flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER))
		efb_clear_quad(flags, color, z);
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
		/* (the back buffer back, if a render target has the EFB) */
		{
			D3DSurface *target = device.render_target;

			device.render_target = &device.back_buffer;
			efb_take();
			device.render_target = target;
		}
		/* (the device set GX behind WiiLibs' shadow) */
		wl_gx_invalidate_shadow();
		wl_gx_end_frame();
		device.frame_open = FALSE;
		/* (the frame drawn: the textures evicted during it can go) */
		GX_DrawDone();
		texture_frees_flush();
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
	/* a frame of more than half a second (loading, mostly): where it went */
	{
		static uint64_t last_frame;
		static unsigned slow_frames;
		static struct platform_file_reads last_reads;
		uint64_t now = wl_platform_time_us();

		if (last_frame && now - last_frame > 500000 && slow_frames < 40)
		{
			slow_frames++;
			platform_log("Direct3D: frame %lu: %lu file reads, %lu KB in %lu ms", device.frames,
				platform_file_reads.calls - last_reads.calls,
				(unsigned long)((platform_file_reads.bytes - last_reads.bytes) / 1024),
				(unsigned long)((platform_file_reads.microseconds - last_reads.microseconds) / 1000));
			platform_log("Direct3D: frame %lu took %lu ms: draws %lu, textures %lu, pixel shaders %lu, the end %lu, "
				"the game %lu", device.frames, (unsigned long)((now - last_frame) / 1000),
				(unsigned long)((timing.draws - last_timing.draws) / 1000),
				(unsigned long)((timing.textures - last_timing.textures) / 1000),
				(unsigned long)((timing.pixel_shaders - last_timing.pixel_shaders) / 1000),
				(unsigned long)((timing.present - last_timing.present) / 1000),
				(unsigned long)((now - last_frame - (timing.draws - last_timing.draws) -
					(timing.textures - last_timing.textures) - (timing.pixel_shaders - last_timing.pixel_shaders) -
					(timing.present - last_timing.present)) / 1000));
		}
		last_frame = now;
		last_timing = timing;
		last_reads = platform_file_reads;
	}
	if (device.frames % STATISTICS_FRAMES == 0)
	{
		platform_log("Direct3D: frame %lu: %lu draws, %lu immediate, %lu vertices (%lu run), %lu clears, %lu drawn "
			"elsewhere, textures %lu bound %lu failed %lu without a mode, in the last %d", device.frames,
			device.statistics.draws, device.statistics.immediate_draws, device.statistics.vertices,
			device.statistics.vertices_run, device.statistics.clears,
			device.draws_elsewhere, device.statistics.textures_bound, device.statistics.textures_failed,
			device.statistics.stages_without_mode, STATISTICS_FRAMES);
		program_statistics_log();
		platform_log("Direct3D: in hardware: %lu draws (%lu of their matrices kept, %lu of several nodes), %lu vertices "
			"(%lu with the processor's colours)", hardware_draws, hardware_cache_hits, hardware_keyed_draws,
			hardware_vertices, hardware_colors_run);
		hardware_draws = hardware_vertices = hardware_colors_run = hardware_cache_hits = hardware_keyed_draws = 0;
		platform_log("Direct3D: textures not drawn: %lu without data, %lu of a shape, %lu of a format, "
			"%lu without memory, %lu not decoded; %lu converted bitmaps found", texture_failures[_failure_no_data],
			texture_failures[_failure_shape],
			texture_failures[_failure_format], texture_failures[_failure_memory], texture_failures[_failure_decode],
			textures_native);
		platform_log("Direct3D: pixel shaders: %lu translated, %lu beyond GX; GX FIFO overflows %lu; "
			"render targets: %lu parks, %lu copies, %lu not copied", pixel_shaders_translated, pixel_shaders_unusable,
			(unsigned long)GX_GetOverflowCount(), efb.parks, efb.resolves, efb.skipped);
		efb.parks = efb.resolves = efb.skipped = 0;
		{
			uint64_t now = wl_platform_time_us();

			if (timing.window_start)
			{
				platform_log("Direct3D: a frame's draws: GX's transform %lu ms (matrices %lu, vertices %lu), the "
					"processor's %lu, immediate %lu", (unsigned long)((timing.hardware_setup + timing.hardware_vertices) /
					1000 / STATISTICS_FRAMES), (unsigned long)(timing.hardware_setup / 1000 / STATISTICS_FRAMES),
					(unsigned long)(timing.hardware_vertices / 1000 / STATISTICS_FRAMES),
					(unsigned long)(timing.processor / 1000 / STATISTICS_FRAMES),
					(unsigned long)(timing.immediate / 1000 / STATISTICS_FRAMES));
				platform_log("Direct3D: the processor's indexed vertices: fetch %lu ms, program %lu, store %lu, emit %lu",
					(unsigned long)(timing.fetch_ticks / 60750 / STATISTICS_FRAMES),
					(unsigned long)(timing.program_ticks / 60750 / STATISTICS_FRAMES),
					(unsigned long)(timing.store_ticks / 60750 / STATISTICS_FRAMES),
					(unsigned long)(timing.emit_ticks / 60750 / STATISTICS_FRAMES));
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
				extern unsigned long wii_game_stack_used(unsigned long *size);
				unsigned long size, deepest = wii_game_stack_used(&size);

				platform_log("memory: the game's stack at most %lu KB deep of %lu", deepest / 1024, size / 1024);
			}
			wii_memory_window_usage(&used, &free_bytes);
			platform_log("memory: MEM2 window (tag, texture and sound caches, Direct3D) %lu KB used, %lu KB free",
				used / 1024, free_bytes / 1024);
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
