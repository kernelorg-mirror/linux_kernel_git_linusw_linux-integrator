// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm_types.h>
#include <linux/pgtable.h>

#include <asm/cputype.h>
#include <asm/idmap.h>
#include <asm/hwcap.h>
#include <asm/pgalloc.h>
#include <asm/sections.h>
#include <asm/system_info.h>

/*
 * Note: accesses outside of the kernel image and the identity map area
 * are not supported on any CPU using the idmap tables as its current
 * page tables.
 */
pgd_t *idmap_pgd __ro_after_init;
long long arch_phys_to_idmap_offset __ro_after_init;

#ifdef CONFIG_ARM_LPAE
static void idmap_add_pmd(pud_t *pud, unsigned long addr, unsigned long end,
	unsigned long prot)
{
	pmd_t *pmd;
	unsigned long next;

	pr_info("Add LPAE PMD for address %08lx, pud = %08x\n", addr, (u32)pud);
	if (pud_none_or_clear_bad(pud) || (pud_val(*pud) & L_PGD_SWAPPER)) {
		pmd = pmd_alloc_one(&init_mm, addr);
		if (!pmd) {
			pr_warn("Failed to allocate identity pmd.\n");
			return;
		}
		/*
		 * Copy the original PMD to ensure that the PMD entries for
		 * the kernel image are preserved.
		 */
		if (!pud_none(*pud))
			memcpy(pmd, pmd_offset(pud, 0),
			       PTRS_PER_PMD * sizeof(pmd_t));
		pud_populate(&init_mm, pud, pmd);
		pmd += pmd_index(addr);
	} else
		pmd = pmd_offset(pud, addr);

	do {
		next = pmd_addr_end(addr, end);
		*pmd = __pmd((addr & PMD_MASK) | prot);
		flush_pmd_entry(pmd);
		pr_info("Add LPAE PMD for address %08lx, pmd = %08x, *pmd = %08x\n",
			addr, (u32)pmd, (u32)*pmd);
	} while (pmd++, addr = next, addr != end);
}
#else	/* !CONFIG_ARM_LPAE */
static void idmap_add_pmd(pud_t *pud, unsigned long addr, unsigned long end,
	unsigned long prot)
{
	pmd_t *pmd = pmd_offset(pud, addr);

	addr = (addr & PMD_MASK) | prot;
	pmd[0] = __pmd(addr);
	addr += SECTION_SIZE;
	pmd[1] = __pmd(addr);
	flush_pmd_entry(pmd);
}
#endif	/* CONFIG_ARM_LPAE */

static void idmap_add_pud(pgd_t *pgd, unsigned long addr, unsigned long end,
	unsigned long prot)
{
	p4d_t *p4d = p4d_offset(pgd, addr);
	pud_t *pud = pud_offset(p4d, addr);
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
		pr_info("Use P4D/PUD entry for address %08lx: p4d = %08x, pud = %08x\n",
			addr, (u32)p4d, (u32)pud);
		idmap_add_pmd(pud, addr, next, prot);
	} while (pud++, addr = next, addr != end);
}

static void identity_mapping_add(pgd_t *pgd, const char *text_start,
				 const char *text_end, unsigned long prot)
{
	unsigned long addr, end;
	unsigned long next;

	addr = virt_to_idmap(text_start);
	end = virt_to_idmap(text_end);
	pr_info("Setting up static identity map for 0x%lx - 0x%lx\n", addr, end);

	prot |= PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AF;

	if (cpu_architecture() <= CPU_ARCH_ARMv5TEJ && !cpu_is_xscale_family())
		prot |= PMD_BIT4;

	pgd += pgd_index(addr);
	do {
		next = pgd_addr_end(addr, end);
		pr_info("Use PGD entry for address %08lx: pgd_index() = %lu, pgd size = %d bytes, pgd = %08x, *pgd = %08x\n",
			addr, pgd_index(addr), sizeof(pgd_t), (u32)pgd, (u32)*pgd);
		idmap_add_pud(pgd, addr, next, prot);
	} while (pgd++, addr = next, addr != end);
}

extern char  __idmap_text_start[], __idmap_text_end[];

/*
 * If you lose printk messages ("dropped...") then increase the kernel
 * log buffer size CONFIG_LOG_BUF_SHIFT
 */
void dump_pagetable(pgd_t *pgd)
{
	pgd_t *tmp;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	int i, j;
	u32 addr = 0;

	pr_info("LPAE PAGE TABLE:\n");

	for (i = 0; i < 4; i++) {
		tmp = pgd + pgd_index(addr);
		pr_info("pgd%d @%08x = 0x%016llx\n", i, (u32)tmp, *tmp);
		for (j = 0; j < PTRS_PER_PMD; j++) {
			p4d = p4d_offset(tmp, addr);
			pud = pud_offset(p4d, addr);
			if (pud_none_or_clear_bad(pud)) {
				pr_info("  p4d/pud/pmd%d for address %08x UNDEFINED\n", j, addr);
			} else {
				pmd = pmd_offset(pud, addr);
				pr_info("  p4d/pud/pmd%d @%08x = %016llx, address %08x\n", j, (u32)pmd, *pmd, addr);
			}
			addr += PMD_SIZE;
		}
	}
}

static int __init init_static_idmap(void)
{
	idmap_pgd = pgd_alloc(&init_mm);
	if (!idmap_pgd)
		return -ENOMEM;

	pr_info("Created LPAE PGD pgd = %08x, *pgd = %08x\n",
		(u32)idmap_pgd, (u32)*idmap_pgd);

	identity_mapping_add(idmap_pgd, __idmap_text_start,
			     __idmap_text_end, 0);

	//dump_pagetable(idmap_pgd);

	/* Flush L1 for the hardware to see this page table content */
	if (!(elf_hwcap & HWCAP_LPAE))
		flush_cache_louis();

	return 0;
}
early_initcall(init_static_idmap);

/*
 * In order to soft-boot, we need to switch to a 1:1 mapping for the
 * cpu_reset functions. This will then ensure that we have predictable
 * results when turning off the mmu.
 */
void setup_mm_for_reboot(void)
{
	/* Switch to the identity mapping. */
	cpu_switch_mm(idmap_pgd, &init_mm);
	local_flush_bp_all();

#ifdef CONFIG_CPU_HAS_ASID
	/*
	 * We don't have a clean ASID for the identity mapping, which
	 * may clash with virtual addresses of the previous page tables
	 * and therefore potentially in the TLB.
	 */
	local_flush_tlb_all();
#endif
}
