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

phys_addr_t __phys_addr_symbol(unsigned long x)
{
	unsigned long kernel_start = kernel_map.virt_addr;
	unsigned long kernel_end = kernel_start + kernel_map.size;

	/*
	 * Boundary checking aginst the kernel image mapping.
	 * __pa_symbol should only be used on kernel symbol addresses.
	 */
	VIRTUAL_BUG_ON(x < kernel_start || x > kernel_end);

	return __va_to_pa_nodebug(x);
}
EXPORT_SYMBOL(__phys_addr_symbol);

phys_addr_t linear_mapping_va_to_pa(unsigned long x)
{
	BUG_ON(!kernel_map.va_pa_offset);

	return ((unsigned long)(x) - kernel_map.va_pa_offset);
}
EXPORT_SYMBOL(linear_mapping_va_to_pa);

void *linear_mapping_pa_to_va(unsigned long x)
{
	BUG_ON(!kernel_map.va_pa_offset);

	return ((void *)((unsigned long)(x) + kernel_map.va_pa_offset));
}
EXPORT_SYMBOL(linear_mapping_pa_to_va);
