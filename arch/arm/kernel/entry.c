// SPDX-License-Identifier: GPL-2.0
#include <linux/hardirq.h>
#include <linux/irq-entry-common.h>
#include <linux/irq.h>

#include <asm/entry.h>
#include <asm/stacktrace.h>
#include <asm/traps.h>
#ifdef CONFIG_CPU_V7M
#include <asm/v7m.h>
#endif

#include "irq.h"

noinstr void arm_rseq_user_enter(void)
{
	rseq_note_user_irq_entry();
}

noinstr void arm_rseq_user_exit(void)
{
	rseq_irqentry_exit_to_user_mode();
}

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

#ifdef CONFIG_CPU_V7M
/*
 * On V7M architecture, the execution priority cannot be lowered without an
 * exception return. Therefore, an IRQ handler cannot directly perform a
 * context switch. To fit generic IRQ entry into this hardware constraint:
 *
 * - IRQ from user mode: if exit work is pending, defer the exit to the PendSV
 *   handler. PendSV has the kernel's priority (that of SVCall), below all
 *   IRQs, so the PendSV handler runs after all pending IRQs. Otherwise exit to
 *   user mode directly.
 * - IRQ from kernel mode: exit directly with rescheduling suppressed using
 *   arch_irqentry_exit_need_resched(). PendSV cannot preempt the kernel, so a
 *   reschedule cannot be deferred to the PendSV handler.
 * - IRQ taken while an exit to user mode is deferred (i.e., while PendSV is
 *   pending): handle it as an IRQ from kernel mode. This IRQ is logically
 *   nested from an irqentry_{enter,exit}() perspective (see
 *   Documentation/core-api/entry.rst), even if its stacked frame is the user
 *   frame (tail-chaining, or late arrival).
 */
noinstr void v7m_irq_handler(struct pt_regs *regs)
{
	if (!user_mode(regs) || (readl_relaxed(BASEADDR_V7M_SCB + V7M_SCB_ICSR) &
				V7M_SCB_ICSR_PENDSVSET)) {
		/* IRQ from kernel mode, or IRQ taken while PendSV is pending */
		irqentry_state_t state = irqentry_enter_from_kernel_mode(regs);

		if (on_thread_stack())
			call_on_irq_stack(handle_arm_irq, regs);
		else
			handle_arm_irq(regs);

		/* arch_irqentry_exit_need_resched() prevents rescheduling here */
		irqentry_exit_to_kernel_mode(regs, state);
	} else {
		irqentry_enter_from_user_mode(regs);

		call_on_irq_stack(handle_arm_irq, regs);

		/*
		 * Check TIF bits in 0..15 to judge whether exit work will be
		 * triggered, as tested in ret_fast_syscall.
		 */
		if (read_thread_flags() & GENMASK(15, 0)) {
			/*
			 * Raise PendSV exception to defer the exit to user
			 * mode to the PendSV handler.
			 */
			writel_relaxed(V7M_SCB_ICSR_PENDSVSET, BASEADDR_V7M_SCB +
					V7M_SCB_ICSR);
		} else {
			/*
			 * Fast path: Directly perform exit in the IRQ handler.
			 * Safe because work won't be triggered.
			 */
			irqentry_exit_to_user_mode(regs);
		}
	}
}

noinstr void v7m_pendsv_handler(struct pt_regs *regs)
{
	/* Clear PendSV pending state */
	writel_relaxed(V7M_SCB_ICSR_PENDSVCLR, BASEADDR_V7M_SCB + V7M_SCB_ICSR);

	/* Call exit and perform the deferred exit work */
	irqentry_exit_to_user_mode(regs);
}
#endif
