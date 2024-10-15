// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2002 ARM Ltd.
 *  All Rights Reserved
 *
 * This code is specific to the hardware found on ARM Realview and
 * Bcm6846 Express platforms where the CPUs are unable to be individually
 * woken, and where there is no way to hot-unplug CPUs.  Real platforms
 * should not copy this code.
 */
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/smp.h>
#include <linux/of.h>

#include <asm/cacheflush.h>
#include <asm/smp_plat.h>
#include <asm/io.h>

#include "platsmp.h"

static void * __iomem cpu_release_addr[NR_CPUS];

static int bcm6846_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	u32 __iomem *release_addr;

	release_addr = cpu_release_addr[cpu];
	if (!release_addr) {
		pr_err("NO RELEASE ADDRESS\n");
		return -EINVAL;
	}
	pr_info("CPU%d: release at address %08x\n",
		cpu, (u32)release_addr);

	writel(__pa_symbol(secondary_startup), release_addr);
	smp_wmb();
	sync_cache_w(&release_addr);

	/*
	 * Send the secondary CPU a soft interrupt, thereby causing
	 * the boot monitor to read the system wide flags register,
	 * and branch to the address found there.
	 */
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));

	return 0;
}

static void __init bcm6846_smp_dt_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np;
	u32 release_addr;
	int ret;
	int cpu;

	for_each_possible_cpu(cpu) {
		np = of_get_cpu_node(cpu, NULL);
		if (!np) {
			pr_err("CPU %d: missing CPU OF node\n",
			       cpu);
			continue;
		}

		/*
		 * Determine the address from which the CPU is polling.
		 */
		ret = of_property_read_u32(np, "cpu-release-addr",
					   &release_addr);
		if (ret)
			pr_err("CPU %d: missing or invalid cpu-release-addr property\n",
			       cpu);


		cpu_release_addr[cpu] = ioremap_cache(release_addr, sizeof(release_addr));
		pr_info("CPU %d: IOREMAP release address %08x -> %08x\n",
			cpu, release_addr, (u32)cpu_release_addr[cpu]);

		of_node_put(np);
	}
}

static const struct smp_operations bcm6846_smp_ops __initconst = {
	.smp_prepare_cpus	= bcm6846_smp_dt_prepare_cpus,
	.smp_boot_secondary	= bcm6846_boot_secondary,
};
CPU_METHOD_OF_DECLARE(bcm6846_smp, "spin-table", &bcm6846_smp_ops);
