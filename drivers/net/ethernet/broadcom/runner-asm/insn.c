// SPDX-License-Identifier: GPL-2.0-only
/*
 * Encoding forms recovered from BCM6846 reduced and CFE2 Runner images.
 * See README.rst for evidence and the fields whose semantics are unknown.
 * Use shifts, not C bitfields: the assembler runs on the build host.
 */
#include <string.h>

#include "runner.h"

struct field {
	unsigned int shift;
	unsigned int bits;
};

enum format {
	NORMAL,
	BRANCH,
	MEMORY,
	ABS32,
	ABS64,
};

struct opcode {
	const char *name;
	uint32_t base;
	enum format format;
	unsigned int count;
	struct field fields[RUNNER_OPERANDS];
};

#define ALU_FIELDS { { 20, 5 }, { 15, 5 }, { 5, 5 }, { 10, 5 }, { 0, 5 }, { 25, 1 } }
#define MEM_FIELDS { { 20, 5 }, { 15, 5 }, { 4, 11 } }
#define ABS_FIELDS { { 20, 5 }, { 4, 16 } }

static const struct opcode opcodes[] = {
	{ "nop", RUNNER_NOP, NORMAL, 0, { { 0, 0 } } },
	{ "movi", 0xb0000004, NORMAL, 2, { { 20, 5 }, { 4, 16 } } },
	{ "ld32.abs", 0x32000002, ABS32, 2, ABS_FIELDS },
	{ "ld64.abs", 0x32000003, ABS64, 2, ABS_FIELDS },
	{ "st32.abs", 0x36000002, ABS32, 2, ABS_FIELDS },
	{ "st64.abs", 0x36000003, ABS64, 2, ABS_FIELDS },
	{ "load", 0x30000000, MEMORY, 4, MEM_FIELDS },
	{ "store", 0x34000000, MEMORY, 4, MEM_FIELDS },
	{ "alu.add", 0x80000000, NORMAL, 6, ALU_FIELDS },
	{ "alu.sub", 0x84000000, NORMAL, 6, ALU_FIELDS },
	{ "alu.and", 0x88000000, NORMAL, 6, ALU_FIELDS },
	{ "alu.shift", 0x94000000, NORMAL, 6, ALU_FIELDS },
	{ "alu.mul", 0x9c000000, NORMAL, 6, ALU_FIELDS },
	{ "field.2a", 0xa8000000, NORMAL, 6, ALU_FIELDS },
	{ "field.2b", 0xac000000, NORMAL, 6, ALU_FIELDS },
	{ "branch", 0x00000000, BRANCH, 2, { { 0, 13 }, { 13, 13 } } },
	{ "jump", 0x04000000, BRANCH, 2, { { 0, 13 }, { 13, 13 } } },
	{ "jump.reg", 0x04000000, NORMAL, 2, { { 0, 5 }, { 13, 13 } } },
	/* These are encoding names, not claims about operation semantics. */
	{ "op02", 0x08000000, NORMAL, 6, ALU_FIELDS },
	{ "op08", 0x20000000, NORMAL, 2, { { 13, 13 }, { 0, 13 } } },
	{ "op09", 0x24000000, NORMAL, 2, { { 13, 13 }, { 0, 13 } } },
	{ "op30", 0xc0000000, NORMAL, 6, ALU_FIELDS },
	{ "op31", 0xc4000000, NORMAL, 6, ALU_FIELDS },
	{ "op33", 0xcc000000, NORMAL, 6, ALU_FIELDS },
};

static uint32_t field_value(uint32_t value, struct field field)
{
	if (value >= (1U << field.bits))
		error("operand %#x does not fit %u bits", value, field.bits);
	return value << field.shift;
}

void instruction(const char *name, const struct operands *args)
{
	const struct opcode *op = NULL;
	uint32_t word, value;
	unsigned int i, count;

	for (i = 0; i < sizeof(opcodes) / sizeof(opcodes[0]); i++) {
		if (!strcmp(name, opcodes[i].name)) {
			op = &opcodes[i];
			break;
		}
	}
	if (!op)
		error("unknown instruction: %s", name);
	if (args->count != op->count)
		error("%s needs %u operands", name, op->count);
	if (pass == 1) {
		emit(0);
		return;
	}
	word = op->base;
	count = op->count;
	if (op->format == MEMORY) {
		/* Low four mode bits and bit 25 are deliberately explicit. */
		value = args->value[--count];
		if (value > 31)
			error("memory mode exceeds five bits");
		word |= (value & 15) | ((value >> 4) << 25);
	}
	if (op->format == ABS32 || op->format == ABS64) {
		if (args->value[1] & 3)
			error("unaligned absolute memory address");
		if (op->format == ABS64 && (args->value[0] & 1))
			error("64-bit transfer needs an even register pair");
	}
	for (i = 0; i < count; i++) {
		value = args->value[i];
		if (op->format == BRANCH && !i) {
			if ((value & 3) || value >= RUNNER_WORDS * 4)
				error("unaligned or out-of-range branch target");
			value /= 4;
		}
		word |= field_value(value, op->fields[i]);
	}
	emit(word);
}
