/*
NV2A_CPU.C

The Xbox's vertex programs (NV2A vertex microcode) run on the processor
(nv2a_cpu.h). Each program is decoded once into its instructions' fields,
as port/linux/src/nv2a_vsh.c reads them, then run for each vertex with the
same semantics as the GLSL that file writes: the MAC (vector) and ILU
(scalar) units read their operands A, B and C, then write their results
(the ILU's to r1 when both run), r12 is the position output read back,
a0 indexes the constants relatively.
*/

#include "nv2a_cpu.h"

#include <math.h>
#include <string.h>

enum
{
	_mac_nop, _mac_mov, _mac_mul, _mac_add, _mac_mad, _mac_dp3, _mac_dph, _mac_dp4,
	_mac_dst, _mac_min, _mac_max, _mac_slt, _mac_sge, _mac_arl,
};

enum
{
	_ilu_nop, _ilu_mov, _ilu_rcp, _ilu_rcc, _ilu_rsq, _ilu_exp, _ilu_log, _ilu_lit,
};

enum
{
	_mux_unknown, _mux_temporary, _mux_input, _mux_constant,
};

/* o[] addresses */
enum
{
	_output_position = 0, _output_d0 = 3, _output_d1 = 4, _output_fog = 5, _output_point_size = 6,
	_output_b0 = 7, _output_b1 = 8, _output_t0 = 9,
};

static unsigned long field(const DWORD *instruction, int word, int low_bit, int bit_count)
{
	return (instruction[word] >> low_bit) & ((1UL << bit_count) - 1);
}

static void operand_fields(const DWORD *instruction, int which, struct nv2a_cpu_operand *operand)
{
	switch (which)
	{
	case 0:
		operand->negate = (unsigned char)field(instruction, 1, 8, 1);
		operand->swizzle[0] = (unsigned char)field(instruction, 1, 6, 2);
		operand->swizzle[1] = (unsigned char)field(instruction, 1, 4, 2);
		operand->swizzle[2] = (unsigned char)field(instruction, 1, 2, 2);
		operand->swizzle[3] = (unsigned char)field(instruction, 1, 0, 2);
		operand->index = (unsigned char)field(instruction, 2, 28, 4);
		operand->mux = (unsigned char)field(instruction, 2, 26, 2);
		break;
	case 1:
		operand->negate = (unsigned char)field(instruction, 2, 25, 1);
		operand->swizzle[0] = (unsigned char)field(instruction, 2, 23, 2);
		operand->swizzle[1] = (unsigned char)field(instruction, 2, 21, 2);
		operand->swizzle[2] = (unsigned char)field(instruction, 2, 19, 2);
		operand->swizzle[3] = (unsigned char)field(instruction, 2, 17, 2);
		operand->index = (unsigned char)field(instruction, 2, 13, 4);
		operand->mux = (unsigned char)field(instruction, 2, 11, 2);
		break;
	default:
		operand->negate = (unsigned char)field(instruction, 2, 10, 1);
		operand->swizzle[0] = (unsigned char)field(instruction, 2, 8, 2);
		operand->swizzle[1] = (unsigned char)field(instruction, 2, 6, 2);
		operand->swizzle[2] = (unsigned char)field(instruction, 2, 4, 2);
		operand->swizzle[3] = (unsigned char)field(instruction, 2, 2, 2);
		operand->index = (unsigned char)((field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2));
		operand->mux = (unsigned char)field(instruction, 3, 28, 2);
		break;
	}
}

void nv2a_cpu_compile(const DWORD *instructions, unsigned long instruction_count, struct nv2a_cpu_program *program)
{
	unsigned long index;

	program->count = 0;
	for (index = 0; index < instruction_count && index < NV2A_CPU_MAXIMUM_INSTRUCTIONS; index++)
	{
		const DWORD *word = instructions + index * 4;
		struct nv2a_cpu_instruction *instruction = &program->instructions[program->count++];
		int which;

		instruction->mac = (unsigned char)field(word, 1, 21, 4);
		instruction->ilu = (unsigned char)field(word, 1, 25, 3);
		instruction->mac_mask = (unsigned char)field(word, 3, 24, 4);
		instruction->temporary = (unsigned char)field(word, 3, 20, 4);
		instruction->ilu_mask = (unsigned char)field(word, 3, 16, 4);
		instruction->output_mask = (unsigned char)field(word, 3, 12, 4);
		instruction->output_is_register = (unsigned char)field(word, 3, 11, 1);
		instruction->output_address = (unsigned char)field(word, 3, 3, 8);
		instruction->output_from_ilu = (unsigned char)field(word, 3, 2, 1);
		instruction->relative = (unsigned char)field(word, 3, 1, 1);
		instruction->input = (unsigned char)field(word, 1, 9, 4);
		instruction->constant = (unsigned char)field(word, 1, 13, 8);
		for (which = 0; which < 3; which++)
			operand_fields(word, which, &instruction->operands[which]);
		instruction->captures_clip = instruction->ilu == _ilu_rcc && instruction->operands[2].mux == _mux_temporary &&
			instruction->operands[2].index == 12;
		if (field(word, 3, 0, 1))
			break;
	}
}

struct registers
{
	float r[12][4];
	float position[4];
	const float (*inputs)[4];
	const float (*constants)[4];
	int a0;
};

static void read_operand(const struct registers *registers, const struct nv2a_cpu_instruction *instruction,
	const struct nv2a_cpu_operand *operand, float out[4])
{
	static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const float *source = zero;
	int component;

	switch (operand->mux)
	{
	case _mux_temporary:
		/* r12 reads back the position output */
		source = operand->index == 12 ? registers->position : registers->r[operand->index < 12 ? operand->index : 0];
		break;
	case _mux_input:
		source = registers->inputs[instruction->input];
		break;
	case _mux_constant:
	{
		int constant = instruction->constant + (instruction->relative ? registers->a0 : 0);

		if (constant < 0)
			constant = 0;
		if (constant >= NV2A_CPU_CONSTANT_COUNT)
			constant = NV2A_CPU_CONSTANT_COUNT - 1;
		source = registers->constants[constant];
		break;
	}
	default:
		break;
	}
	for (component = 0; component < 4; component++)
		out[component] = operand->negate ? -source[operand->swizzle[component]] : source[operand->swizzle[component]];
}

static void write_masked(float destination[4], const float value[4], unsigned mask)
{
	/* (bit 3 is x) */
	if (mask & 8) destination[0] = value[0];
	if (mask & 4) destination[1] = value[1];
	if (mask & 2) destination[2] = value[2];
	if (mask & 1) destination[3] = value[3];
}

static float reciprocal_clamped(float x)
{
	float r = 1.0f / x;

	if (r > 0.0f)
		return r < 5.42101e-20f ? 5.42101e-20f : r > 1.884467e+19f ? 1.884467e+19f : r;
	return r > -5.42101e-20f ? -5.42101e-20f : r < -1.884467e+19f ? -1.884467e+19f : r;
}

void nv2a_cpu_run(const struct nv2a_cpu_program *program, const float (*constants)[4], const float (*inputs)[4],
	float point_size, struct nv2a_cpu_output *output)
{
	struct registers registers;
	unsigned long index;
	int component;

	memset(&registers, 0, sizeof(registers));
	registers.position[3] = 1.0f;
	registers.inputs = inputs;
	registers.constants = constants;
	memset(output, 0, sizeof(*output));
	output->d0[3] = output->d1[3] = output->b0[3] = output->b1[3] = 1.0f;
	for (component = 0; component < 4; component++)
		output->t[component][3] = 1.0f;
	output->fog = 1.0f;
	output->point_size = point_size;

	for (index = 0; index < program->count; index++)
	{
		const struct nv2a_cpu_instruction *instruction = &program->instructions[index];
		float a[4], b[4], c[4], mac[4] = { 0 }, ilu[4] = { 0 };

		read_operand(&registers, instruction, &instruction->operands[0], a);
		read_operand(&registers, instruction, &instruction->operands[1], b);
		read_operand(&registers, instruction, &instruction->operands[2], c);
		switch (instruction->mac)
		{
		case _mac_mov: case _mac_arl: memcpy(mac, a, sizeof(mac)); break;
		case _mac_mul: for (component = 0; component < 4; component++) mac[component] = a[component] * b[component]; break;
		case _mac_add: for (component = 0; component < 4; component++) mac[component] = a[component] + c[component]; break;
		case _mac_mad: for (component = 0; component < 4; component++) mac[component] = a[component] * b[component] + c[component]; break;
		case _mac_dp3: mac[0] = mac[1] = mac[2] = mac[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
		case _mac_dph: mac[0] = mac[1] = mac[2] = mac[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3]; break;
		case _mac_dp4: mac[0] = mac[1] = mac[2] = mac[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; break;
		case _mac_dst: mac[0] = 1.0f; mac[1] = a[1] * b[1]; mac[2] = a[2]; mac[3] = b[3]; break;
		case _mac_min: for (component = 0; component < 4; component++) mac[component] = a[component] < b[component] ? a[component] : b[component]; break;
		case _mac_max: for (component = 0; component < 4; component++) mac[component] = a[component] > b[component] ? a[component] : b[component]; break;
		case _mac_slt: for (component = 0; component < 4; component++) mac[component] = a[component] < b[component] ? 1.0f : 0.0f; break;
		case _mac_sge: for (component = 0; component < 4; component++) mac[component] = a[component] >= b[component] ? 1.0f : 0.0f; break;
		default: break;
		}
		switch (instruction->ilu)
		{
		case _ilu_mov: memcpy(ilu, c, sizeof(ilu)); break;
		case _ilu_rcp: ilu[0] = ilu[1] = ilu[2] = ilu[3] = 1.0f / c[0]; break;
		case _ilu_rcc: ilu[0] = ilu[1] = ilu[2] = ilu[3] = reciprocal_clamped(c[0]); break;
		case _ilu_rsq: ilu[0] = ilu[1] = ilu[2] = ilu[3] = 1.0f / sqrtf(fabsf(c[0])); break;
		case _ilu_exp:
			ilu[0] = exp2f(floorf(c[0]));
			ilu[1] = c[0] - floorf(c[0]);
			ilu[2] = exp2f(c[0]);
			ilu[3] = 1.0f;
			break;
		case _ilu_log:
		{
			float x = fabsf(c[0]);

			if (x == 0.0f)
			{
				ilu[0] = ilu[2] = -1.0e30f;
				ilu[1] = ilu[3] = 1.0f;
			}
			else
			{
				float exponent = floorf(log2f(x));

				ilu[0] = exponent;
				ilu[1] = x / exp2f(exponent);
				ilu[2] = log2f(x);
				ilu[3] = 1.0f;
			}
			break;
		}
		case _ilu_lit:
		{
			float power = c[3] < -127.9961f ? -127.9961f : c[3] > 127.9961f ? 127.9961f : c[3];

			ilu[0] = 1.0f;
			ilu[1] = c[0] > 0.0f ? c[0] : 0.0f;
			ilu[2] = c[0] > 0.0f ? powf(c[1] > 0.0f ? c[1] : 0.0f, power) : 0.0f;
			ilu[3] = 1.0f;
			break;
		}
		default:
			break;
		}
		/* the screen-space conversion: the position before it */
		if (instruction->captures_clip)
		{
			memcpy(output->clip, registers.position, sizeof(output->clip));
			output->clip_captured = 1;
		}

		/* results written once both units have read their inputs */
		if (instruction->mac == _mac_arl)
		{
			registers.a0 = (int)floorf(mac[0] + 0.001f);
		}
		else if (instruction->mac != _mac_nop && instruction->mac_mask)
		{
			write_masked(instruction->temporary == 12 ? registers.position :
				registers.r[instruction->temporary < 12 ? instruction->temporary : 0], mac, instruction->mac_mask);
		}
		if (instruction->ilu != _ilu_nop && instruction->ilu_mask)
		{
			unsigned temporary = instruction->mac != _mac_nop ? 1 : instruction->temporary;

			write_masked(temporary == 12 ? registers.position : registers.r[temporary < 12 ? temporary : 0], ilu,
				instruction->ilu_mask);
		}
		if (instruction->output_mask && instruction->output_is_register &&
			(instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0)
		{
			const float *source = instruction->output_from_ilu ? ilu : mac;

			switch (instruction->output_address)
			{
			case _output_position: write_masked(registers.position, source, instruction->output_mask); break;
			case _output_d0: write_masked(output->d0, source, instruction->output_mask); break;
			case _output_d1: write_masked(output->d1, source, instruction->output_mask); break;
			case _output_b0: write_masked(output->b0, source, instruction->output_mask); break;
			case _output_b1: write_masked(output->b1, source, instruction->output_mask); break;
			case _output_fog: if (instruction->output_mask & 8) output->fog = source[0]; break;
			case _output_point_size: if (instruction->output_mask & 8) output->point_size = source[0]; break;
			default:
				if (instruction->output_address >= _output_t0 && instruction->output_address < _output_t0 + 4)
					write_masked(output->t[instruction->output_address - _output_t0], source, instruction->output_mask);
				break;
			}
		}
	}
	memcpy(output->position, registers.position, sizeof(output->position));
}
