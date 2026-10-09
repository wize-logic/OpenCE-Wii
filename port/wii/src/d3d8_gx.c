/*
D3D8_GX.C

The Xbox Direct3D 8 device for the Wii, over GX through WiiLibs, in place of
port/linux/src/d3d8_gl.c.

The game drives the device through the XDK's inline functions, which keep
the "simple" render states in D3D__RenderState (port/linux/src/d3d8_state.c,
shared with the OpenGL device) and call into this file for everything else.
Resources - textures, surfaces, vertex and index buffers - are the shared
d3d8_resources.c's, in the contiguous window, identified by the physical
address in their Data field.

This first device keeps every state the game sets and reads, with the same
semantics as the OpenGL device: transforms, viewport, render targets,
vertex shaders and their constants, streams, immediate-mode vertices,
visibility tests, callbacks and the vertical blank. It draws nothing yet:
each Present shows a frame cleared to the colour of the frame's last
back-buffer Clear. The draws are counted (logged every few seconds), so the
game's progress through its frames can be followed in the log.
*/

#include "platform.h"

#include <pthread.h>
#include <string.h>

#include <ogc/video.h>

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
/* frames between the draw counts in the log */
#define STATISTICS_FRAMES 300

/* ---------- structures */

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long id;
	DWORD *instructions;
	unsigned long instruction_count;
	const DWORD *declaration;
};

struct stream_source
{
	D3DVertexBuffer *buffer;
	UINT stride;
};

struct draw_statistics
{
	unsigned long draws;
	unsigned long vertices;
	unsigned long clears;
	unsigned long immediate_draws;
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

		/* the video mode and GX (the text console of the start-up goes) */
		device.gx_ready = wl_platform_video_init() == WL_OK && wl_gx_init() == WL_OK;
		if (!device.gx_ready)
			platform_log("Direct3D: GX did not start (nothing is displayed)");
		device.created = TRUE;
		platform_log("Direct3D: device created, %dx%d (draws are not shown yet)", SCREEN_WIDTH, SCREEN_HEIGHT);
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
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
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

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	object->id = device.next_vertex_shader_id++;
	object->declaration = declaration;
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
	(void)primitive_type;
	(void)start_vertex;
	device.statistics.draws++;
	device.statistics.vertices += vertex_count;
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	(void)primitive_type;
	(void)index_data;
	device.statistics.draws++;
	device.statistics.vertices += vertex_count;
}

/* immediate mode: a vertex is complete when its position (register 0) is set */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	device.immediate = TRUE;
	device.immediate_primitive = primitive_type;
	device.immediate_vertices = 0;
}

void WINAPI D3DDevice_End(void)
{
	if (device.immediate && device.immediate_vertices)
	{
		device.statistics.immediate_draws++;
		device.statistics.vertices += device.immediate_vertices;
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
	if (reg == 0 && device.immediate)
		device.immediate_vertices++;
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
	if ((flags & D3DCLEAR_TARGET) && device.render_target == &device.back_buffer)
		device.clear_color = color;
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
		/* ARGB to the RGBA WiiLibs takes */
		D3DCOLOR color = device.clear_color;

		wl_gx_begin_frame((color << 8) | 0xff);
		wl_gx_end_frame();
	}
	flip_count++;
	device.frames++;
	if (device.frames % STATISTICS_FRAMES == 0)
	{
		platform_log("Direct3D: frame %lu: %lu draws, %lu immediate, %lu vertices, %lu clears in the last %d",
			device.frames, device.statistics.draws, device.statistics.immediate_draws, device.statistics.vertices,
			device.statistics.clears, STATISTICS_FRAMES);
		memset(&device.statistics, 0, sizeof(device.statistics));
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
