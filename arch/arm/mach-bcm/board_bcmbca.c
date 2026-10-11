// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Linus Walleij <linus.walleij@linaro.org>

#include <linux/io.h>
#include <linux/of.h>
#include <linux/smp.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/psci.h>
#include <asm/smp_plat.h>

#ifdef CONFIG_DEBUG_BCMBCA
/* This is needed for LL-debug/earlyprintk/debug-macro.S */
static struct map_desc bcmbca_io_desc[] __initdata = {
	{
		.virtual = CONFIG_DEBUG_UART_VIRT,
		.pfn = __phys_to_pfn(CONFIG_DEBUG_UART_PHYS),
		.length = SZ_4K,
		.type = MT_DEVICE,
	},
};
#endif

static void __init bcmbca_map_io(void)
{
#ifdef CONFIG_DEBUG_BCMBCA
	iotable_init(bcmbca_io_desc, ARRAY_SIZE(bcmbca_io_desc));
#endif
}

#ifdef CONFIG_SMP
#define BCM6846_CFE_MAILBOX	0xff800568

static int bcm6846_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	void __iomem *mailbox;

	/* CFE has already powered up CPU1 and left it waiting in WFI. */
	if (cpu_logical_map(cpu) != 1)
		return -EINVAL;

	mailbox = ioremap(BCM6846_CFE_MAILBOX, sizeof(u32));
	if (!mailbox)
		return -ENOMEM;

	/*
	 * The CFE stub reads GPIO GeneralPurpose after an interrupt and
	 * branches to it in ARM state. Clear any Thumb function pointer
	 * tag and complete the MMIO write before sending the wakeup IPI.
	 */
	writel(__pa_symbol(secondary_startup_arm) & ~1, mailbox);
	dsb(sy);
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));
	iounmap(mailbox);

	return 0;
}

static const struct smp_operations bcm6846_smp_ops __initconst = {
	.smp_boot_secondary = bcm6846_boot_secondary,
};

static bool __init bcm6846_smp_init(void)
{
	if (!of_machine_is_compatible("genexis,xg6846b") || psci_smp_available())
		return false;

	smp_set_ops(&bcm6846_smp_ops);
	return true;
}
#endif

static const char * const bcmbca_dt_compat[] = {
	/* TODO: Add other BCMBCA SoCs here to get debug UART support */
	"brcm,bcm6846",
	NULL,
};

DT_MACHINE_START(BCMBCA_DT, "BCMBCA Broadband Access Processors")
	.map_io = bcmbca_map_io,
	.smp_init = smp_init_ops(bcm6846_smp_init),
	.dt_compat = bcmbca_dt_compat,
MACHINE_END
