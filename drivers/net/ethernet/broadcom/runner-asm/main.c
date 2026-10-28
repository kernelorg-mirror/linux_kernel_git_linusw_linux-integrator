// SPDX-License-Identifier: GPL-2.0-only
/*
 * BCM6846 Runner assembler. Like aicasm, keep the scanner, grammar,
 * symbols and instruction formats separate from output generation.
 */
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runner.h"

const char *input_name;
unsigned int pass, pc;
unsigned int source_line = 1;
static uint32_t image[RUNNER_WORDS];

void error(const char *fmt, ...)
{
	va_list args;

	fprintf(stderr, "%s:%u: ", input_name, source_line);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

void *alloc(size_t size)
{
	void *ptr = malloc(size);

	if (!ptr)
		error("out of memory");
	return ptr;
}

uint32_t number(const char *text)
{
	unsigned long long value;
	char *end;

	errno = 0;
	value = strtoull(text, &end, !strncmp(text, "0x", 2) ? 16 : 10);
	if (errno || *end || value > UINT32_MAX)
		error("invalid 32-bit number: %s", text);
	return value;
}

uint32_t shift_left(uint32_t value, uint32_t shift)
{
	if (shift >= 32 || (shift && value > UINT32_MAX >> shift))
		error("shift overflow");
	return value << shift;
}

void emit(uint32_t word)
{
	if (pc >= sizeof(image))
		error("program exceeds instruction RAM");
	image[pc / 4] = word;
	pc += 4;
}

void pad(uint32_t end)
{
	if ((end & 3) || end < pc || end > sizeof(image))
		error("invalid padding address: %#x", end);
	while (pc < end)
		emit(RUNNER_NOP);
}

void assert_pc(uint32_t expected)
{
	if (pc != expected)
		error("ABI address moved: expected %#x, got %#x", expected, pc);
}

void yyerror(const char *message)
{
	error("%s", message);
}

static void write_image(const char *name)
{
	char *temporary = alloc(strlen(name) + sizeof(".XXXXXX"));
	unsigned int i;
	FILE *output;
	int fd;
	bool failed = false;

	sprintf(temporary, "%s.XXXXXX", name);
	fd = mkstemp(temporary);
	if (fd < 0)
		error("cannot create %s: %s", name, strerror(errno));
	output = fdopen(fd, "wb");
	if (!output) {
		close(fd);
		unlink(temporary);
		error("cannot open output stream: %s", strerror(errno));
	}
	/* Emit bytes explicitly: neither host nor target endianness matters. */
	for (i = 0; i < pc / 4; i++) {
		uint32_t word = image[i];
		unsigned char bytes[] = { word >> 24, word >> 16, word >> 8, word };

		if (fwrite(bytes, 1, sizeof(bytes), output) != sizeof(bytes)) {
			failed = true;
			break;
		}
	}
	if (fclose(output))
		failed = true;
	if (failed || rename(temporary, name)) {
		unlink(temporary);
		error("cannot write %s", name);
	}
	free(temporary);
}

int main(int argc, char **argv)
{
	unsigned int first_size = 0;
	FILE *input;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s source output\n", argv[0]);
		return EXIT_FAILURE;
	}
	input_name = argv[1];
	input = fopen(input_name, "r");
	if (!input)
		error("cannot open input: %s", strerror(errno));
	for (pass = 1; pass <= 2; pass++) {
		pc = 0;
		yylineno = 1;
		source_line = 1;
		yyrestart(input);
		if (yyparse() || ferror(input))
			error("cannot parse input");
		if (pass == 1) {
			first_size = pc;
			if (fseek(input, 0, SEEK_SET))
				error("cannot rewind input");
		}
	}
	if (!pc || pc != first_size)
		error("empty program or size changed between passes");
	fclose(input);
	write_image(argv[2]);
	free_symbols();
	yylex_destroy();
	return EXIT_SUCCESS;
}
