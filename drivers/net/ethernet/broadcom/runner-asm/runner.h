/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RUNNER_AS_H
#define RUNNER_AS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define RUNNER_WORDS 8192
#define RUNNER_NOP 0xfc000000U
#define RUNNER_OPERANDS 6

struct operands {
	unsigned int count;
	uint32_t value[RUNNER_OPERANDS];
};

extern const char *input_name;
extern unsigned int pass, pc;
extern unsigned int source_line;
extern int yylineno;
extern FILE *yyin;

void error(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void *alloc(size_t size);
uint32_t number(const char *text);
void define_symbol(const char *name, uint32_t value);
uint32_t lookup_symbol(const char *name);
void free_symbols(void);
void instruction(const char *name, const struct operands *args);
void emit(uint32_t word);
void pad(uint32_t end);
void assert_pc(uint32_t expected);
uint32_t shift_left(uint32_t value, uint32_t shift);
int yyparse(void);
int yylex(void);
void yyrestart(FILE *input);
int yylex_destroy(void);
void yyerror(const char *message);

#endif
