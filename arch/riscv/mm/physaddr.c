// SPDX-License-Identifier: GPL-2.0

#include <linux/types.h>
#include <linux/mmdebug.h>
#include <linux/mm.h>
#include <asm/page.h>
#include <asm/sections.h>

unsigned long virt_to_pfn(const void *vaddr)
{
	return phys_to_pfn(__pa(vaddr));
}
EXPORT_SYMBOL(virt_to_pfn);
