// SPDX-License-Identifier: GPL-2.0-only
/* Symbols are defined in pass one and resolved in pass two. */
#include <stdlib.h>
#include <string.h>

#include "runner.h"

struct symbol {
	struct symbol *next;
	char *name;
	uint32_t value;
};

static struct symbol *symbols;

static struct symbol *find_symbol(const char *name)
{
	struct symbol *symbol;

	for (symbol = symbols; symbol; symbol = symbol->next)
		if (!strcmp(name, symbol->name))
			return symbol;
	return NULL;
}

void define_symbol(const char *name, uint32_t value)
{
	struct symbol *symbol = find_symbol(name);

	if (pass == 2) {
		if (!symbol || symbol->value != value)
			error("symbol changed between passes: %s", name);
		return;
	}
	if (symbol)
		error("duplicate symbol: %s", name);
	/* Register names cannot be redefined. */
	if (name[0] == 'r' && name[1] >= '0' && name[1] <= '9')
		error("reserved register name: %s", name);
	if (!strcmp(name, "."))
		error("the location counter cannot be redefined");
	symbol = alloc(sizeof(*symbol));
	symbol->name = alloc(strlen(name) + 1);
	strcpy(symbol->name, name);
	symbol->value = value;
	symbol->next = symbols;
	symbols = symbol;
}

uint32_t lookup_symbol(const char *name)
{
	struct symbol *symbol;

	if (!strcmp(name, "."))
		return pc;
	symbol = find_symbol(name);
	if (!symbol)
		error("undefined symbol: %s", name);
	return symbol->value;
}

void free_symbols(void)
{
	while (symbols) {
		struct symbol *next = symbols->next;

		free(symbols->name);
		free(symbols);
		symbols = next;
	}
}
