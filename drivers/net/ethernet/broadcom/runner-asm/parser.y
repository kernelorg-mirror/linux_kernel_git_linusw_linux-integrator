/* SPDX-License-Identifier: GPL-2.0-only */
%{
#include <stdlib.h>
#include <stdint.h>
#include "runner.h"

/* Forward references are allowed in instructions, not layout directives. */
static bool resolving;
%}

%code requires {
#include "runner.h"
}

%union {
	uint32_t number;
	char *name;
	struct operands operands;
}

%token <number> NUMBER REGISTER
%token <name> NAME
%token PAD ASSERT CONST LSHIFT
%type <number> expr
%type <operands> operands arguments
%destructor { free($$); } <name>
%left '|'
%left '&'
%left LSHIFT
%left '+' '-'

%%
program: %empty | program line;
line:
	'\n'
	| NAME ':' { define_symbol($1, pc); free($1); }
	| CONST NAME '=' { resolving = true; } expr '\n'
		{ define_symbol($2, $5); free($2); resolving = false; }
	| PAD { resolving = true; } expr '\n' { pad($3); resolving = false; }
	| ASSERT { resolving = true; } expr '\n'
		{ assert_pc($3); resolving = false; }
	| NAME arguments '\n' { instruction($1, &$2); free($1); }
	;
arguments:
	%empty { $$.count = 0; }
	| operands { $$ = $1; }
	;
operands:
	expr { $$.count = 1; $$.value[0] = $1; }
	| operands ',' expr {
		$$ = $1;
		if ($$.count == RUNNER_OPERANDS)
			error("too many operands");
		$$.value[$$.count++] = $3;
	}
	;
expr:
	NUMBER { $$ = $1; }
	| REGISTER { $$ = $1; }
	| NAME {
		$$ = pass == 2 || resolving ? lookup_symbol($1) : 0;
		free($1);
	}
	| '(' expr ')' { $$ = $2; }
	| expr '+' expr {
		if ($1 > UINT32_MAX - $3)
			error("addition overflow");
		$$ = $1 + $3;
	}
	| expr '-' expr {
		if ((pass == 2 || resolving) && $1 < $3)
			error("subtraction underflow");
		$$ = pass == 1 && !resolving ? 0 : $1 - $3;
	}
	| expr LSHIFT expr { $$ = shift_left($1, $3); }
	| expr '&' expr { $$ = $1 & $3; }
	| expr '|' expr { $$ = $1 | $3; }
	;
%%
