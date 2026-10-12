/*
GX_STAND_IN.C

libogc's GX, VIDEO and cache functions the Direct3D device calls, for the
host harness (gx_stand_in.h says what it records). Compiled with the
device's own flags; with GX_STAND_IN_RECORD, against the copy of <ogc/gx.h>
tools/wii_draw_bench.py writes, whose vertex writes call wg_record_*.
*/

#include <stdint.h>
#include <string.h>

#define BOOL GCBOOL
#include <ogc/cache.h>
#include <ogc/gx.h>
#include <ogc/video.h>
#undef BOOL

#include "gx_stand_in.h"

static unsigned long calls;

unsigned long gx_stand_in_calls(void)
{
	return calls;
}

#ifndef GX_STAND_IN_RECORD

/* ---------- counting: GX does nothing, the vertex writes go to one word */

static volatile WGPipe pipe_sink;
WGPipe *const wgPipe = (WGPipe *)&pipe_sink;

#define CALL(name, selector, ...) ((void)(selector), calls++)
#define CANONICAL(pointer) (pointer)
#define CALL_DATA(name, selector, data, bytes) ((void)(selector), (void)(data), calls++)
#define BEGIN(primitive, format, count) (void)(primitive), (void)(format), (void)(count), calls++

unsigned long gx_stand_in_errors(void)
{
	return 0;
}

unsigned long gx_stand_in_take(struct gx_stand_in_draw *draws, unsigned long capacity)
{
	(void)draws;
	(void)capacity;
	calls = 0;
	return 0;
}

void gx_stand_in_states(void (*each)(const char *name, uint32_t selector, uint64_t hash))
{
	(void)each;
}

void gx_stand_in_trace(unsigned gen, void (*each)(float s))
{
	(void)gen;
	(void)each;
}

#else

/* ---------- recording */

enum { ATTRIBUTES = 26, FORMATS = 8, MATRIX_ROWS = 64, NORMAL_MATRICES = 64, STATES = 512, FIFO_BYTES = 1 << 20,
	DRAWS = 1 << 16 };

static uint64_t hash_bytes(uint64_t hash, const void *data, unsigned long bytes)
{
	const unsigned char *p = data;
	unsigned long index;

	for (index = 0; index < bytes; index++)
		hash = (hash ^ p[index]) * 1099511628211ULL;
	return hash;
}

/* what the vertices are read by (consumed by the decoding, not digested
as state) */
struct vertex_state
{
	unsigned char desc[ATTRIBUTES];
	unsigned char comp_count[FORMATS][ATTRIBUTES], comp_type[FORMATS][ATTRIBUTES], frac[FORMATS][ATTRIBUTES];
	const unsigned char *array[ATTRIBUTES];
	unsigned array_stride[ATTRIBUTES];
	uint32_t current_matrix;
	unsigned texgens;
	struct { uint32_t type, source, matrix, normalize, post; } texgen[8];
	float rows[MATRIX_ROWS][4];
	/* the post-transform matrices (GX_DTTMTX0 on) */
	float post_rows[MATRIX_ROWS][4];
	float normal[NORMAL_MATRICES][3][3];
};

static struct vertex_state vs;

/* every other call's last arguments, by its name and selector */
static struct
{
	const char *name;
	uint32_t selector;
	uint64_t hash;
} states[STATES];
static unsigned state_count;

static struct gx_stand_in_draw recorded[DRAWS];
static unsigned long recorded_count;

/* the primitive being written: its state when it began, and its bytes */
static struct
{
	int open;
	unsigned char primitive, format;
	unsigned count;
	unsigned long need;
	uint64_t state;
	struct vertex_state vs;
} pending;
static unsigned char fifo[FIFO_BYTES];
static unsigned long fifo_length;
/* vertex bytes outside a primitive, or a primitive left short */
static unsigned long errors;
/* a display list being written (GX_BeginDispList) */
static unsigned char *list_base;
static unsigned long list_size, list_length;

/* a pointer as the digests take it: one into the window (the Wii's, at its
address in every build) as it is; any other (the device's own arrays and
heap, whose addresses move from build to build) by the order it was first
seen in */
static uint32_t canonical(uint32_t pointer)
{
	static uint32_t seen[1024];
	static unsigned seen_count;
	unsigned index;

	if (pointer >= 0x90000000U && pointer < 0xA0000000U)
		return pointer;
	for (index = 0; index < seen_count; index++)
		if (seen[index] == pointer)
			return 0xC0000000U | index;
	if (seen_count < 1024)
		seen[seen_count++] = pointer;
	return 0xC0000000U | index;
}

static void state_set(const char *name, uint32_t selector, const void *words, unsigned long bytes)
{
	unsigned index;
	uint64_t hash = hash_bytes(14695981039346656037ULL, words, bytes);

	for (index = 0; index < state_count; index++)
		if (states[index].name == name && states[index].selector == selector)
			break;
	if (index == state_count)
	{
		if (state_count == STATES)
			return;
		state_count++;
	}
	states[index].name = name;
	states[index].selector = selector;
	states[index].hash = hash;
}

/* the state's digest, whatever order it was set in */
static uint64_t state_digest(void)
{
	uint64_t sum = 0;
	unsigned index;

	for (index = 0; index < state_count; index++)
	{
		uint64_t hash = hash_bytes(14695981039346656037ULL, states[index].name, strlen(states[index].name));

		hash = hash_bytes(hash, &states[index].selector, sizeof(states[index].selector));
		hash = hash_bytes(hash, &states[index].hash, sizeof(states[index].hash));
		sum += hash * 0x9E3779B97F4A7C15ULL;
	}
	return sum;
}

#define CALL(name, selector, ...) do { uint32_t call_words_[] = { __VA_ARGS__ }; calls++; \
	state_set(name, (uint32_t)(selector), call_words_, sizeof(call_words_)); } while (0)
#define CALL_DATA(name, selector, data, bytes) do { calls++; state_set(name, (uint32_t)(selector), data, bytes); } while (0)
#define CANONICAL(pointer) canonical(pointer)

static unsigned component_bytes(unsigned type)
{
	return type == GX_F32 ? 4 : type == GX_U16 || type == GX_S16 ? 2 : 1;
}

static unsigned color_bytes(unsigned type)
{
	switch (type)
	{
	case GX_RGB565: case GX_RGBA4: return 2;
	case GX_RGB8: case GX_RGBA6: return 3;
	default: return 4;
	}
}

static unsigned attribute_components(unsigned attribute, unsigned count)
{
	if (attribute == GX_VA_POS)
		return count == GX_POS_XY ? 2 : 3;
	if (attribute == GX_VA_NRM)
		return 3;
	return count == GX_TEX_S ? 1 : 2;
}

/* an attribute's bytes as a vertex holds it (directly) */
static unsigned attribute_bytes(const struct vertex_state *state, unsigned format, unsigned attribute)
{
	if (attribute < GX_VA_POS)
		return 1;
	if (attribute == GX_VA_CLR0 || attribute == GX_VA_CLR1)
		return color_bytes(state->comp_type[format][attribute]);
	return attribute_components(attribute, state->comp_count[format][attribute]) *
		component_bytes(state->comp_type[format][attribute]);
}

static unsigned long vertex_bytes(const struct vertex_state *state, unsigned format)
{
	unsigned long bytes = 0;
	unsigned attribute;

	for (attribute = 0; attribute <= GX_VA_TEX7; attribute++)
	{
		unsigned desc = state->desc[attribute];

		if (desc == GX_DIRECT)
			bytes += attribute_bytes(state, format, attribute);
		else if (desc == GX_INDEX8)
			bytes += 1;
		else if (desc == GX_INDEX16)
			bytes += 2;
	}
	return bytes;
}

static float component(const unsigned char *data, unsigned type, unsigned frac)
{
	float value;
	int16_t s16;
	uint16_t u16;

	switch (type)
	{
	case GX_F32: memcpy(&value, data, 4); return value;
	case GX_S16: memcpy(&s16, data, 2); return (float)s16 / (float)(1 << frac);
	case GX_U16: memcpy(&u16, data, 2); return (float)u16 / (float)(1 << frac);
	case GX_S8: return (float)(int8_t)data[0] / (float)(1 << frac);
	default: return (float)data[0] / (float)(1 << frac);
	}
}

/* the traced texture coordinate (gx_stand_in_trace) */
static unsigned trace_gen = 99;
static void (*trace_each)(float s);

void gx_stand_in_trace(unsigned gen, void (*each)(float s))
{
	trace_gen = gen;
	trace_each = each;
}

/* one vertex read from data as state describes it, digested */
static uint64_t vertex_digest(uint64_t hash, const struct vertex_state *state, unsigned format, const unsigned char **data)
{
	unsigned char indices[9];
	float values[ATTRIBUTES][3];
	unsigned char colors[2][4];
	unsigned attribute, gen, row;

	memset(indices, 0, sizeof(indices));
	memset(values, 0, sizeof(values));
	memset(colors, 0, sizeof(colors));
	for (attribute = 0; attribute <= GX_VA_TEX7; attribute++)
	{
		unsigned desc = state->desc[attribute], components, type, frac, which;
		const unsigned char *source;

		if (desc == GX_NONE)
			continue;
		if (attribute < GX_VA_POS)
		{
			indices[attribute] = *(*data)++;
			continue;
		}
		if (desc == GX_DIRECT)
		{
			source = *data;
			*data += attribute_bytes(state, format, attribute);
		}
		else
		{
			unsigned index;

			if (desc == GX_INDEX8)
				index = *(*data)++;
			else
			{
				index = (unsigned)(*data)[0] << 8 | (*data)[1];
				*data += 2;
			}
			source = state->array[attribute] + (unsigned long)index * state->array_stride[attribute];
		}
		if (attribute == GX_VA_CLR0 || attribute == GX_VA_CLR1)
		{
			memcpy(colors[attribute - GX_VA_CLR0], source, color_bytes(state->comp_type[format][attribute]));
			continue;
		}
		components = attribute_components(attribute, state->comp_count[format][attribute]);
		type = state->comp_type[format][attribute];
		frac = state->frac[format][attribute];
		for (which = 0; which < components; which++)
			values[attribute][which] = component(source + which * component_bytes(type), type, frac);
	}
	/* the position through its matrix (the vertex's, else the current one) */
	{
		unsigned matrix = state->desc[GX_VA_PTNMTXIDX] ? indices[0] : state->current_matrix & 0x3F;

		hash = hash_bytes(hash, values[GX_VA_POS], sizeof(values[GX_VA_POS]));
		for (row = 0; row < 3 && matrix + row < MATRIX_ROWS; row++)
			hash = hash_bytes(hash, state->rows[matrix + row], sizeof(state->rows[0]));
		if (state->desc[GX_VA_NRM])
		{
			hash = hash_bytes(hash, values[GX_VA_NRM], sizeof(values[GX_VA_NRM]));
			hash = hash_bytes(hash, state->normal[matrix], sizeof(state->normal[0]));
		}
	}
	if (state->desc[GX_VA_CLR0])
		hash = hash_bytes(hash, colors[0], 4);
	if (state->desc[GX_VA_CLR1])
		hash = hash_bytes(hash, colors[1], 4);
	/* each texture coordinate generated: its source's values through its
	matrix (the vertex's, else the generation's) */
	for (gen = 0; gen < state->texgens && gen < 8; gen++)
	{
		uint32_t source = state->texgen[gen].source, matrix = state->texgen[gen].matrix;
		const float *input;
		uint32_t kind[2] = { state->texgen[gen].type, source };

		if (state->desc[GX_VA_TEX0MTXIDX + gen])
			matrix = indices[1 + gen];
		input = source == GX_TG_POS ? values[GX_VA_POS] : source == GX_TG_NRM ? values[GX_VA_NRM] :
			source >= GX_TG_TEX0 && source <= GX_TG_TEX7 ? values[GX_VA_TEX0 + source - GX_TG_TEX0] : values[0];
		if (gen == trace_gen && trace_each)
		{
			float in[4] = { input[0], input[1], input[2], 1.0f }, out[3] = { input[0], input[1], 1.0f };
			unsigned rows_used = state->texgen[gen].type == GX_TG_MTX2x4 ? 2u : 3u, column;

			if (source != GX_TG_POS && source != GX_TG_NRM)
				in[2] = 1.0f;
			if (matrix != GX_IDENTITY)
				for (row = 0; row < rows_used && matrix + row < MATRIX_ROWS; row++)
				{
					out[row] = 0.0f;
					for (column = 0; column < 4; column++)
						out[row] += state->rows[matrix + row][column] * in[column];
				}
			if (rows_used == 2)
				out[2] = 1.0f;
			if (state->texgen[gen].post != GX_DTTIDENTITY)
			{
				uint32_t post = state->texgen[gen].post - GX_DTTMTX0;
				float again[4] = { out[0], out[1], out[2], 1.0f };

				for (row = 0; row < 3 && post + row < MATRIX_ROWS; row++)
				{
					out[row] = 0.0f;
					for (column = 0; column < 4; column++)
						out[row] += state->post_rows[post + row][column] * again[column];
				}
			}
			trace_each(rows_used == 3 && out[2] != 0.0f ? out[0] / out[2] : out[0]);
		}
		hash = hash_bytes(hash, kind, sizeof(kind));
		hash = hash_bytes(hash, input, 3 * sizeof(float));
		if (matrix == GX_IDENTITY)
			hash = hash_bytes(hash, "identity", 8);
		else
			for (row = 0; row < (state->texgen[gen].type == GX_TG_MTX2x4 ? 2u : 3u) && matrix + row < MATRIX_ROWS; row++)
				hash = hash_bytes(hash, state->rows[matrix + row], sizeof(state->rows[0]));
		/* (and its post-transform matrix, where it has one) */
		if (state->texgen[gen].post != GX_DTTIDENTITY)
		{
			uint32_t post = state->texgen[gen].post - GX_DTTMTX0;

			hash = hash_bytes(hash, &state->texgen[gen].normalize, sizeof(state->texgen[gen].normalize));
			for (row = 0; row < 3 && post + row < MATRIX_ROWS; row++)
				hash = hash_bytes(hash, state->post_rows[post + row], sizeof(state->post_rows[0]));
		}
	}
	return hash;
}

/* a primitive's vertices, from its bytes */
static void primitive_digest(const struct vertex_state *state, uint64_t state_hash, unsigned primitive, unsigned format,
	unsigned count, const unsigned char *data)
{
	uint64_t hash = 14695981039346656037ULL ^ state_hash;
	uint32_t head[2] = { primitive, count };
	unsigned vertex;

	hash = hash_bytes(hash, head, sizeof(head));
	for (vertex = 0; vertex < count; vertex++)
		hash = vertex_digest(hash, state, format, &data);
	if (recorded_count < DRAWS)
	{
		recorded[recorded_count].digest = hash;
		recorded[recorded_count].vertices = count;
	}
	recorded_count++;
}

static void pending_close(void)
{
	if (pending.open && fifo_length >= pending.need)
	{
		primitive_digest(&pending.vs, pending.state, pending.primitive, pending.format, pending.count, fifo);
		pending.open = 0;
		fifo_length = 0;
	}
}

static void fifo_write(const void *data, unsigned long bytes)
{
	if (list_base)
	{
		if (list_length + bytes <= list_size)
			memcpy(list_base + list_length, data, bytes);
		list_length += bytes;
		return;
	}
	if (!pending.open)
	{
		errors++;
		return;
	}
	if (fifo_length + bytes <= FIFO_BYTES)
		memcpy(fifo + fifo_length, data, bytes);
	fifo_length += bytes;
	pending_close();
}

void wg_record_U8(u8 value) { fifo_write(&value, 1); }
void wg_record_S8(s8 value) { fifo_write(&value, 1); }
void wg_record_U16(u16 value) { fifo_write(&value, 2); }
void wg_record_S16(s16 value) { fifo_write(&value, 2); }
void wg_record_U32(u32 value) { fifo_write(&value, 4); }
void wg_record_S32(s32 value) { fifo_write(&value, 4); }
void wg_record_F32(f32 value) { fifo_write(&value, 4); }

#define BEGIN(primitive, format, count) primitive_begin(primitive, format, count)

static void primitive_begin(u8 primitive, u8 format, u16 count)
{
	calls++;
	if (list_base)
	{
		u8 command = (u8)(primitive | (format & 7));

		fifo_write(&command, 1);
		fifo_write(&count, 2);
		return;
	}
	pending_close();
	if (pending.open)
		errors++;
	pending.open = 1;
	pending.primitive = primitive;
	pending.format = format & 7;
	pending.count = count;
	pending.vs = vs;
	pending.state = state_digest();
	pending.need = (unsigned long)count * vertex_bytes(&vs, pending.format);
	fifo_length = 0;
	pending_close();
}

unsigned long gx_stand_in_errors(void)
{
	return errors;
}

void gx_stand_in_states(void (*each)(const char *name, uint32_t selector, uint64_t hash))
{
	unsigned index;

	for (index = 0; index < state_count; index++)
		each(states[index].name, states[index].selector, states[index].hash);
	/* (and the vertex state's parts) */
	each("vertex descriptor", 0, hash_bytes(14695981039346656037ULL, vs.desc, sizeof(vs.desc)));
	each("current matrix", 0, vs.current_matrix);
	each("texgens", 0, vs.texgens);
	for (index = 0; index < 8; index++)
		each("texgen", index, hash_bytes(14695981039346656037ULL, &vs.texgen[index], sizeof(vs.texgen[index])));
}

unsigned long gx_stand_in_take(struct gx_stand_in_draw *draws, unsigned long capacity)
{
	unsigned long count = recorded_count;

	pending_close();
	memcpy(draws, recorded, (count < capacity ? count : capacity) * sizeof(*draws));
	recorded_count = 0;
	calls = 0;
	return count;
}

void GX_BeginDispList(void *list, u32 size)
{
	calls++;
	list_base = list;
	list_size = size;
	list_length = 0;
}

u32 GX_EndDispList(void)
{
	unsigned long length = (list_length + 31) & ~31UL;

	calls++;
	if (list_length <= list_size)
		memset(list_base + list_length, 0, (length < list_size ? length : list_size) - list_length);
	list_base = NULL;
	return length <= list_size ? (u32)length : 0;
}

void GX_CallDispList(void *list, u32 size)
{
	const unsigned char *data = list, *end = data + size;
	uint64_t state = state_digest();

	calls++;
	pending_close();
	while (data < end)
	{
		unsigned char command = *data++;
		unsigned count;

		if (!command)
			continue;
		count = (unsigned)data[0] << 8 | data[1];
		data += 2;
		primitive_digest(&vs, state, command & 0xF8, command & 7, count, data);
		data += (unsigned long)count * vertex_bytes(&vs, command & 7);
	}
}

#endif

/* ---------- the functions */

#define FLOAT_WORD(value) float_word(value)
static inline uint32_t float_word(float value)
{
	uint32_t word;

	memcpy(&word, &value, sizeof(word));
	return word;
}

#ifndef GX_STAND_IN_RECORD
void GX_BeginDispList(void *list, u32 size) { (void)list; (void)size; calls++; }
u32 GX_EndDispList(void) { calls++; return 32; }
void GX_CallDispList(void *list, u32 size) { (void)list; (void)size; calls++; }
#endif

void GX_Begin(u8 primitive, u8 format, u16 count)
{
	BEGIN(primitive, format, count);
}

void GX_DrawDone(void) { calls++; }
void GX_InvVtxCache(void) { calls++; }
void GX_PixModeSync(void) { calls++; }
void GX_InvalidateTexAll(void) { calls++; }
u32 GX_GetOverflowCount(void) { calls++; return 0; }
void GX_SetMisc(u32 token, u32 value) { CALL("misc", token, value); }
void GX_GetGPStatus(u8 *overhi, u8 *underlow, u8 *read_idle, u8 *command_idle, u8 *breakpoint)
{
	calls++;
	*overhi = *underlow = *breakpoint = 0;
	*read_idle = *command_idle = 1;
}
void GX_GetGPFifo(GXFifoObj *fifo) { calls++; memset(fifo, 0, sizeof(*fifo)); }
void GX_GetCPUFifo(GXFifoObj *fifo) { calls++; memset(fifo, 0, sizeof(*fifo)); }
void GX_GetFifoPtrs(const GXFifoObj *fifo, void **read, void **write) { (void)fifo; calls++; *read = *write = NULL; }
u32 GX_GetFifoCount(const GXFifoObj *fifo) { (void)fifo; calls++; return 0; }
u16 GX_GetDrawSync(void) { calls++; return 0; }

/* (the descriptor as GX_GetVtxDescv lists it, recording or not) */
static unsigned char descriptor_types[GX_VA_TEX7 + 1];

void GX_GetVtxDescv(GXVtxDesc *attr_list)
{
	unsigned attribute;

	calls++;
	for (attribute = 0; attribute <= GX_VA_TEX7; attribute++)
	{
		attr_list[attribute].attr = attribute;
		attr_list[attribute].type = descriptor_types[attribute];
	}
	attr_list[attribute].attr = GX_VA_NULL;
	attr_list[attribute].type = 0;
}

void GX_ClearVtxDesc(void)
{
	calls++;
	memset(descriptor_types, 0, sizeof(descriptor_types));
#ifdef GX_STAND_IN_RECORD
	memset(vs.desc, 0, sizeof(vs.desc));
#endif
}

void GX_SetVtxDesc(u8 attribute, u8 type)
{
	calls++;
	if (attribute <= GX_VA_TEX7)
		descriptor_types[attribute] = type;
#ifdef GX_STAND_IN_RECORD
	if (attribute < ATTRIBUTES)
		vs.desc[attribute] = type;
#else
	(void)attribute;
	(void)type;
#endif
}

void GX_SetVtxAttrFmt(u8 format, u32 attribute, u32 count, u32 type, u32 frac)
{
	calls++;
#ifdef GX_STAND_IN_RECORD
	if (format < FORMATS && attribute < ATTRIBUTES)
	{
		vs.comp_count[format][attribute] = (unsigned char)count;
		vs.comp_type[format][attribute] = (unsigned char)type;
		vs.frac[format][attribute] = (unsigned char)frac;
	}
#else
	(void)format;
	(void)attribute;
	(void)count;
	(void)type;
	(void)frac;
#endif
}

void GX_SetArray(u32 attribute, void *pointer, u8 stride)
{
	calls++;
#ifdef GX_STAND_IN_RECORD
	if (attribute < ATTRIBUTES)
	{
		vs.array[attribute] = pointer;
		vs.array_stride[attribute] = stride;
	}
#else
	(void)attribute;
	(void)pointer;
	(void)stride;
#endif
}

void GX_SetCurrentMtx(u32 matrix)
{
	calls++;
#ifdef GX_STAND_IN_RECORD
	vs.current_matrix = matrix;
#else
	(void)matrix;
#endif
}

void GX_SetNumTexGens(u32 count)
{
	calls++;
#ifdef GX_STAND_IN_RECORD
	vs.texgens = count;
#else
	(void)count;
#endif
}

void GX_SetTexCoordGen2(u16 texcoord, u32 type, u32 source, u32 matrix, u32 normalize, u32 post)
{
	calls++;
#ifdef GX_STAND_IN_RECORD
	if (texcoord < 8)
	{
		vs.texgen[texcoord].type = type;
		vs.texgen[texcoord].source = source;
		vs.texgen[texcoord].matrix = matrix;
		vs.texgen[texcoord].normalize = normalize;
		vs.texgen[texcoord].post = post;
	}
#else
	(void)texcoord;
	(void)type;
	(void)source;
	(void)matrix;
	(void)normalize;
	(void)post;
#endif
}

void GX_SetTexCoordGen(u16 texcoord, u32 type, u32 source, u32 matrix)
{
	GX_SetTexCoordGen2(texcoord, type, source, matrix, GX_FALSE, GX_DTTIDENTITY);
}

static void rows_load(const float (*matrix)[4], u32 first, unsigned count)
{
#ifdef GX_STAND_IN_RECORD
	unsigned row;

	if (first >= GX_DTTMTX0)
	{
		for (row = 0; row < count && first - GX_DTTMTX0 + row < MATRIX_ROWS; row++)
			memcpy(vs.post_rows[first - GX_DTTMTX0 + row], matrix[row], sizeof(vs.post_rows[0]));
		return;
	}
	for (row = 0; row < count && first + row < MATRIX_ROWS; row++)
		memcpy(vs.rows[first + row], matrix[row], sizeof(vs.rows[0]));
#else
	(void)matrix;
	(void)first;
	(void)count;
#endif
}

void GX_LoadPosMtxImm(Mtx matrix, u32 index) { calls++; rows_load((const float (*)[4])matrix, index, 3); }
void GX_LoadTexMtxImm(Mtx matrix, u32 index, u8 type)
{
	calls++;
	rows_load((const float (*)[4])matrix, index, type == GX_MTX2x4 ? 2 : 3);
}

void GX_LoadNrmMtxImm(Mtx matrix, u32 index)
{
	calls++;
#ifdef GX_STAND_IN_RECORD
	if (index < NORMAL_MATRICES)
	{
		unsigned row, column;

		for (row = 0; row < 3; row++)
			for (column = 0; column < 3; column++)
				vs.normal[index][row][column] = matrix[row][column];
	}
#else
	(void)matrix;
	(void)index;
#endif
}

void GX_LoadProjectionMtx(Mtx44 matrix, u8 type)
{
	CALL_DATA("projection", type, matrix, sizeof(Mtx44));
}

#define F FLOAT_WORD
void GX_SetViewport(f32 x, f32 y, f32 width, f32 height, f32 near_z, f32 far_z)
{
	CALL("viewport", 0, F(x), F(y), F(width), F(height), F(near_z), F(far_z));
}

void GX_SetScissor(u32 x, u32 y, u32 width, u32 height) { CALL("scissor", 0, x, y, width, height); }
void GX_SetChanCtrl(s32 channel, u8 enable, u8 ambient_source, u8 material_source, u8 lights, u8 diffuse, u8 attenuation)
{
	CALL("chan", channel, enable, ambient_source, material_source, lights, diffuse, attenuation);
}
static uint32_t color_value(GXColor color)
{
	return (uint32_t)color.r << 24 | (uint32_t)color.g << 16 | (uint32_t)color.b << 8 | color.a;
}
void GX_SetChanAmbColor(s32 channel, GXColor color) { CALL("ambient", channel, color_value(color)); }
void GX_SetChanMatColor(s32 channel, GXColor color) { CALL("material", channel, color_value(color)); }
void GX_SetNumChans(u8 count) { CALL("chans", 0, count); }
void GX_SetTevOp(u8 stage, u8 mode) { CALL("tev op", stage, mode); }
void GX_SetTevColor(u8 reg, GXColor color) { CALL("tev color", reg, color_value(color)); }
void GX_SetTevKColor(u8 reg, GXColor color) { CALL("tev konst", reg, color_value(color)); }
void GX_SetTevColorIn(u8 stage, u8 a, u8 b, u8 c, u8 d) { CALL("tev color in", stage, a, b, c, d); }
void GX_SetTevAlphaIn(u8 stage, u8 a, u8 b, u8 c, u8 d) { CALL("tev alpha in", stage, a, b, c, d); }
void GX_SetTevColorOp(u8 stage, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg)
{
	CALL("tev color op", stage, op, bias, scale, clamp, reg);
}
void GX_SetTevAlphaOp(u8 stage, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg)
{
	CALL("tev alpha op", stage, op, bias, scale, clamp, reg);
}
void GX_SetTevOrder(u8 stage, u8 texcoord, u32 texmap, u8 color) { CALL("tev order", stage, texcoord, texmap, color); }
void GX_SetNumTevStages(u8 count) { CALL("tev stages", 0, count); }
void GX_SetTevKColorSel(u8 stage, u8 select) { CALL("tev kcsel", stage, select); }
void GX_SetTevKAlphaSel(u8 stage, u8 select) { CALL("tev kasel", stage, select); }
void GX_SetTevSwapMode(u8 stage, u8 rasterized, u8 texture) { CALL("tev swap", stage, rasterized, texture); }
void GX_SetTevSwapModeTable(u8 table, u8 r, u8 g, u8 b, u8 a) { CALL("tev swap table", table, r, g, b, a); }
void GX_SetTevIndirect(u8 stage, u8 indirect, u8 format, u8 bias, u8 matrix, u8 wrap_s, u8 wrap_t, u8 add, u8 lod, u8 a)
{
	CALL("tev indirect", stage, indirect, format, bias, matrix, wrap_s, wrap_t, add, lod, a);
}
void GX_SetTevDirect(u8 stage) { CALL("tev indirect", stage, 0, 0, 0, 0, 0, 0, 0, 0, 0); }
void GX_SetTevIndRepeat(u8 stage) { CALL("tev indirect", stage, 0xFFFFFFFFU); }
void GX_SetNumIndStages(u8 count) { CALL("ind stages", 0, count); }
void GX_SetIndTexOrder(u8 stage, u8 texcoord, u8 texmap) { CALL("ind order", stage, texcoord, texmap); }
void GX_SetIndTexCoordScale(u8 stage, u8 s, u8 t) { CALL("ind scale", stage, s, t); }
void GX_SetIndTexMatrix(u8 matrix, f32 offsets[2][3], s8 scale)
{
	CALL("ind matrix", matrix, F(offsets[0][0]), F(offsets[0][1]), F(offsets[0][2]), F(offsets[1][0]), F(offsets[1][1]),
		F(offsets[1][2]), (uint32_t)scale);
}
void GX_SetZTexture(u8 op, u8 format, u32 bias) { CALL("z texture", 0, op, format, bias); }
void GX_SetZMode(u8 enable, u8 function, u8 update) { CALL("z mode", 0, enable, function, update); }
void GX_SetZCompLoc(u8 before) { CALL("z comp loc", 0, before); }
void GX_SetBlendMode(u8 type, u8 source, u8 destination, u8 op) { CALL("blend", 0, type, source, destination, op); }
void GX_SetCullMode(u8 mode) { CALL("cull", 0, mode); }
void GX_SetAlphaCompare(u8 comp0, u8 ref0, u8 op, u8 comp1, u8 ref1) { CALL("alpha compare", 0, comp0, ref0, op, comp1, ref1); }
void GX_SetColorUpdate(u8 enable) { CALL("color update", 0, enable); }
void GX_SetAlphaUpdate(u8 enable) { CALL("alpha update", 0, enable); }
void GX_SetCopyFilter(u8 aa, u8 pattern[12][2], u8 vertical, u8 filter[7])
{
	CALL("copy filter", 0, aa, vertical);
	(void)pattern;
	(void)filter;
}
void GX_SetTexCopySrc(u16 left, u16 top, u16 width, u16 height) { CALL("copy source", 0, left, top, width, height); }
void GX_SetTexCopyDst(u16 width, u16 height, u32 format, u8 mipmap) { CALL("copy destination", 0, width, height, format, mipmap); }
void GX_SetDispCopySrc(u16 left, u16 top, u16 width, u16 height) { CALL("display copy source", 0, left, top, width, height); }
void GX_SetDispCopyDst(u16 width, u16 height) { CALL("display copy destination", 0, width, height); }
u32 GX_SetDispCopyYScale(f32 scale) { CALL("display copy scale", 0, F(scale)); return 480; }
f32 GX_GetYScaleFactor(u16 efb_height, u16 xfb_height) { calls++; return (f32)xfb_height / (f32)efb_height; }
void GX_CopyTex(void *destination, u8 clear) { CALL("copy", 0, CANONICAL((uint32_t)destination), clear); }
void GX_PeekARGB(u16 x, u16 y, GXColor *color)
{
	calls++;
	(void)x;
	(void)y;
	memset(color, 0, sizeof(*color));
}

/* a texture object: its arguments, as GX_LoadTexObj digests them */
void GX_InitTexObj(GXTexObj *object, void *image, u16 width, u16 height, u8 format, u8 wrap_s, u8 wrap_t, u8 mipmap)
{
	calls++;
	memset(object, 0, sizeof(*object));
	object->val[0] = (u32)image;
	object->val[1] = (u32)width << 16 | height;
	object->val[2] = (u32)format | (u32)wrap_s << 8 | (u32)wrap_t << 16 | (u32)mipmap << 24;
	object->val[4] = GX_LINEAR << 8 | (mipmap ? GX_LIN_MIP_LIN : GX_LINEAR);
}
void GX_InitTexObjCI(GXTexObj *object, void *image, u16 width, u16 height, u8 format, u8 wrap_s, u8 wrap_t, u8 mipmap,
	u32 tlut)
{
	GX_InitTexObj(object, image, width, height, format, wrap_s, wrap_t, mipmap);
	object->val[3] = tlut;
}
void GX_InitTexObjTlut(GXTexObj *object, u32 tlut) { calls++; object->val[3] = tlut; }
void GX_InitTexObjFilterMode(GXTexObj *object, u8 minification, u8 magnification)
{
	calls++;
	object->val[4] = (object->val[4] & ~0xFFFFU) | (u32)magnification << 8 | minification;
}
void GX_InitTexObjLOD(GXTexObj *object, u8 minification, u8 magnification, f32 min_lod, f32 max_lod, f32 bias,
	u8 bias_clamp, u8 edge_lod, u8 anisotropy)
{
	calls++;
	object->val[4] = (u32)minification | (u32)magnification << 8 | (u32)bias_clamp << 16 | (u32)edge_lod << 24;
	object->val[5] = float_word(min_lod);
	object->val[6] = float_word(max_lod);
	object->val[7] = float_word(bias) ^ anisotropy;
}
void GX_LoadTexObj(GXTexObj *object, u8 map)
{
	GXTexObj copy = *object;

	copy.val[0] = CANONICAL(copy.val[0]);
	CALL_DATA("texture", map, &copy, sizeof(copy));
}

void GX_InitTlutObj(GXTlutObj *object, void *table, u8 format, u16 entries)
{
	calls++;
	object->val[0] = (u32)table;
	object->val[1] = format;
	object->val[2] = entries;
}
void GX_LoadTlut(GXTlutObj *object, u32 name)
{
#ifdef GX_STAND_IN_RECORD
	uint64_t hash = hash_bytes(14695981039346656037ULL, (const void *)object->val[0], object->val[2] * 2);

	CALL("tlut", name, object->val[1], (uint32_t)hash, (uint32_t)(hash >> 32));
#else
	CALL("tlut", name, 0);
	(void)object;
#endif
}

void GX_InitLightPos(GXLightObj *light, f32 x, f32 y, f32 z)
{
	calls++;
	light->val[0] = float_word(x);
	light->val[1] = float_word(y);
	light->val[2] = float_word(z);
}
void GX_InitLightDir(GXLightObj *light, f32 x, f32 y, f32 z)
{
	calls++;
	light->val[3] = float_word(x);
	light->val[4] = float_word(y);
	light->val[5] = float_word(z);
}
void GX_InitLightColor(GXLightObj *light, GXColor color) { calls++; light->val[6] = color_value(color); }
void GX_InitLightAttn(GXLightObj *light, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2)
{
	calls++;
	light->val[7] = float_word(a0);
	light->val[8] = float_word(a1);
	light->val[9] = float_word(a2);
	light->val[10] = float_word(k0);
	light->val[11] = float_word(k1);
	light->val[12] = float_word(k2);
}
void GX_LoadLightObj(const GXLightObj *light, u8 id) { CALL_DATA("light", id, light, 13 * sizeof(u32)); }

/* ---------- VIDEO, the caches, gu */

GXRModeObj *VIDEO_GetPreferredMode(GXRModeObj *mode)
{
	static GXRModeObj preferred;

	(void)mode;
	preferred.fbWidth = 640;
	preferred.efbHeight = 480;
	preferred.xfbHeight = 480;
	preferred.viWidth = 640;
	preferred.viHeight = 480;
	return &preferred;
}

void VIDEO_WaitVSync(void)
{
}

/* (a line at a time, as libogc's: dcbf has no effect in qemu's user mode, but
counts) */
void PPCDCacheFlushAsync(const volatile void *buffer, size_t size)
{
	unsigned long address = (unsigned long)buffer & ~31UL, end = (unsigned long)buffer + size;

	for (; address < end; address += 32)
		__asm__ volatile("dcbf 0, %0" : : "r"(address) : "memory");
}

void PPCDCacheInvalidate(const volatile void *buffer, size_t size)
{
	(void)buffer;
	(void)size;
}

void ps_guMtxIdentity(Mtx matrix)
{
	int row, column;

	for (row = 0; row < 3; row++)
		for (column = 0; column < 4; column++)
			matrix[row][column] = row == column ? 1.0f : 0.0f;
}

