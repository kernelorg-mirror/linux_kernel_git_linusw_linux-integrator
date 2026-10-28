/* SPDX-License-Identifier: GPL-2.0-only */
%option noyywrap nounput noinput yylineno nodefault
%{
#include <stdlib.h>
#include <string.h>
#include "runner.h"
#include "parser.tab.h"

/* flex has already advanced yylineno when returning a newline token. */
#define YY_USER_ACTION \
	if (yytext[0] != '\n') source_line = yylineno;
%}

%%
[ \t\r]+	;
#[^\n]*	;
".const"	return CONST;
".pad"		return PAD;
".assert_pc"	return ASSERT;
"<<"		return LSHIFT;
0x[0-9a-fA-F]+|[0-9]+ {
		yylval.number = number(yytext);
		return NUMBER;
	}
r[0-9]+ {
		yylval.number = number(yytext + 1);
		if (yylval.number > 31)
			error("register outside r0..r31");
		return REGISTER;
	}
[a-zA-Z_.][a-zA-Z_0-9.]* {
		yylval.name = alloc(yyleng + 1);
		memcpy(yylval.name, yytext, yyleng + 1);
		return NAME;
	}
[:,=()+&|\n-]	return yytext[0];
.		error("unexpected character: %s", yytext);
%%
