/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_ARM_ENTRY_COMMON_H
#define __ASM_ARM_ENTRY_COMMON_H

#include <asm/stacktrace.h>

#ifdef CONFIG_CPU_V7M
/* V7M never reschedules on exit to kernel mode */
static inline bool arch_irqentry_exit_need_resched(void)
{
	return false;
}
#define arch_irqentry_exit_need_resched arch_irqentry_exit_need_resched
#endif

#endif
