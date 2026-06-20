// SPDX-License-Identifier: GPL-2.0
#include <linux/hardirq.h>
#include <linux/irq-entry-common.h>
#include <linux/irq.h>

#include <asm/entry.h>
#include <asm/stacktrace.h>
#include <asm/traps.h>

#include "irq.h"

static void noinstr handle_arm_irq(void *data)
{
	struct pt_regs *regs = data;
	struct pt_regs *old_regs;

	irq_enter_rcu();
	old_regs = set_irq_regs(regs);

	handle_arch_irq(regs);

	set_irq_regs(old_regs);
	irq_exit_rcu();
}

noinstr void arm_irq_handler(struct pt_regs *regs, int mode)
{
	irqentry_state_t state = irqentry_enter(regs);

	/*
	 * mode == 1 means we came from userspace, and then we
	 * should just immediately switch to the irq stack.
	 * Then we check of we are on the thread stack. If we are
	 * not, then by definition we are already using the irq stack.
	 */
	if (mode == 1 || on_thread_stack())
		call_on_irq_stack(handle_arm_irq, regs);
	else
		handle_arm_irq(regs);

	irqentry_exit(regs, state);
}

noinstr void arm_fiq_handler(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_nmi_enter(regs);

	handle_fiq_as_nmi(regs);

	irqentry_nmi_exit(regs, state);
}
