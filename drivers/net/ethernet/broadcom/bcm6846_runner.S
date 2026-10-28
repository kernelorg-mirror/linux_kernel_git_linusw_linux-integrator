/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

	.section .rodata.bcm6846_runner, "a"
	.balign 4
SYM_DATA_START(bcm6846_runner_start)
	.incbin "bcm6846_runner.bin"
SYM_DATA_END_LABEL(bcm6846_runner_start, SYM_L_GLOBAL, bcm6846_runner_end)

	.section .note.GNU-stack, "", %progbits
