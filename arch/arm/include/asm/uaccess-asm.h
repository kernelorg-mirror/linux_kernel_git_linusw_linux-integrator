/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ASM_UACCESS_ASM_H__
#define __ASM_UACCESS_ASM_H__

#include <asm/asm-offsets.h>
#include <asm/domain.h>
#include <asm/memory.h>
#include <asm/thread_info.h>
#include <asm/assembler.h>
#include <asm/proc-fns.h>

	.macro	csdb
#ifdef CONFIG_THUMB2_KERNEL
	.inst.w	0xf3af8014
#else
	.inst	0xe320f014
#endif
	.endm

/*
 * Switch to kernel virtual memory context on 4G-by-4G split
 */
	/* FIXME: eventually only do this switch if 4G/4G is enabled */
	.macro  kernel_vm_context, rd:req
	mrc	p15, 0, \rd, c2, c0, 1  @ read TTBR1
	mcr	p15, 0, \rd, c2, c0, 0  @ set TTBR0
	instr_sync
	.endm

	.macro  usr_vm_ttbrval, tmp0:req, tmp1:req, tmp2:req
	/*
	 * Restore TTBR0 to userspace PGD
	 * FIXME: elif defined(CONFIG_VMSPLIT_4G_4G)
	 *
	 * This is pretty much emulating:
	 * struct mm_struct *mm = current->active_mm;
	 * cpu_switch_mm(mm, mm->pgd);
	 */
	act_mm	\tmp0
#ifndef __ARMEB__
	ldr	\tmp1, [\tmp0, #MM_PGD]
	ldr	\tmp2, [\tmp0, #(MM_PGD + 4)]
#else
	ldr	\tmp2, [\tmp0, #MM_PGD]
	ldr	\tmp1, [\tmp0, #(MM_PGD + 4)]
#endif
	.endm

	.macro  usr_vm_context, tmp0:req, tmp1:req, tmp2:req
	// Just call cpu_v7_switch_mm()?
	usr_vm_ttbrval \tmp0, \tmp1, \tmp2
	mcrr	p15, 0, \tmp1, \tmp2, c2  @ set TTBR0
	instr_sync
	.endm

	.macro check_uaccess, addr:req, size:req, limit:req, tmp:req, bad:req
#ifndef CONFIG_CPU_USE_DOMAINS
	adds	\tmp, \addr, #\size - 1
	sbcscc	\tmp, \tmp, \limit
	bcs	\bad
#ifdef CONFIG_CPU_SPECTRE
	movcs	\addr, #0
	csdb
#endif
#endif
	.endm

	.macro uaccess_mask_range_ptr, addr:req, size:req, limit:req, tmp:req
#ifdef CONFIG_CPU_SPECTRE
	sub	\tmp, \limit, #1
	subs	\tmp, \tmp, \addr	@ tmp = limit - 1 - addr
	addhs	\tmp, \tmp, #1		@ if (tmp >= 0) {
	subshs	\tmp, \tmp, \size	@ tmp = limit - (addr + size) }
	movlo	\addr, #0		@ if (tmp < 0) addr = NULL
	csdb
#endif
	.endm

	.macro	uaccess_disable, tmp, isb=1
#ifdef CONFIG_CPU_SW_DOMAIN_PAN
	/*
	 * Whenever we re-enter userspace, the domains should always be
	 * set appropriately.
	 */
	mov	\tmp, #DACR_UACCESS_DISABLE
	mcr	p15, 0, \tmp, c3, c0, 0		@ Set domain register
	.if	\isb
	instr_sync
	.endif
#else
	/*
	 * FIXME: elseif defined(CONFIG_VMSPLIT_4G_4G)
	 * Doesn't work because sp is not at the right place!
	 */
	//kernel_vm_context \tmp
#endif
	.endm

	.macro	uaccess_enable, tmp, isb=1
#ifdef CONFIG_CPU_SW_DOMAIN_PAN
	/*
	 * Whenever we re-enter userspace, the domains should always be
	 * set appropriately.
	 */
	mov	\tmp, #DACR_UACCESS_ENABLE
	mcr	p15, 0, \tmp, c3, c0, 0
	.if	\isb
	instr_sync
	.endif
#else
	/*
	 * FIXME: elseif defined(CONFIG_VMSPLIT_4G_4G)
	 * Doesn't work because sp is not at the right place!
	 */
	//usr_vm_context \tmp
#endif
	.endm

#if defined(CONFIG_CPU_SW_DOMAIN_PAN) || defined(CONFIG_CPU_USE_DOMAINS)
#define DACR(x...)	x
#else
#define DACR(x...)
#endif

	/*
	 * Save the address limit on entry to a privileged exception.
	 *
	 * If we are using the DACR for kernel access by the user accessors
	 * (CONFIG_CPU_USE_DOMAINS=y), always reset the DACR kernel domain
	 * back to client mode, whether or not \disable is set.
	 *
	 * If we are using SW PAN, set the DACR user domain to no access
	 * if \disable is set.
	 */
	.macro	uaccess_entry, tsk, tmp0, tmp1, tmp2, disable
 DACR(	mrc	p15, 0, \tmp0, c3, c0, 0)
 DACR(	str	\tmp0, [sp, #SVC_DACR])
	.if \disable && IS_ENABLED(CONFIG_CPU_SW_DOMAIN_PAN)
	/* kernel=client, user=no access */
	mov	\tmp2, #DACR_UACCESS_DISABLE
	mcr	p15, 0, \tmp2, c3, c0, 0
	instr_sync
	.elseif IS_ENABLED(CONFIG_CPU_USE_DOMAINS)
	/* kernel=client */
	bic	\tmp2, \tmp0, #domain_mask(DOMAIN_KERNEL)
	orr	\tmp2, \tmp2, #domain_val(DOMAIN_KERNEL, DOMAIN_CLIENT)
	mcr	p15, 0, \tmp2, c3, c0, 0
	instr_sync
	.endif
	.endm

	/* Restore the user access state previously saved by uaccess_entry */
	.macro	uaccess_exit, tsk, tmp0, tmp1
 DACR(	ldr	\tmp0, [sp, #SVC_DACR])
 DACR(	mcr	p15, 0, \tmp0, c3, c0, 0)
	.endm

#undef DACR

#endif /* __ASM_UACCESS_ASM_H__ */
