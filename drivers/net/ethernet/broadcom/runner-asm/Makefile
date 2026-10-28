# SPDX-License-Identifier: GPL-2.0-only
hostprogs-always-y := runner-as
runner-as-objs := main.o symbol.o insn.o parser.tab.o lexer.lex.o
HOST_EXTRACFLAGS += -I$(src) -I$(obj)

$(obj)/lexer.lex.o: $(obj)/parser.tab.h
