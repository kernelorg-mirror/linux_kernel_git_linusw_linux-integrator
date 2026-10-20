/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_ENTRY_H__
#define __ASM_ENTRY_H__

struct pt_regs;

void arm_rseq_user_enter(void);
void arm_rseq_user_exit(void);
void arm_irq_handler(struct pt_regs *regs, int mode);
void arm_fiq_handler(struct pt_regs *regs);
#ifdef CONFIG_CPU_V7M
void v7m_irq_handler(struct pt_regs *regs);
void v7m_pendsv_handler(struct pt_regs *regs);
#endif

#endif /* __ASM_ENTRY_H__ */
