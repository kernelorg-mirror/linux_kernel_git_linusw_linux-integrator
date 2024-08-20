// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Linus Walleij <linus.walleij@linaro.org>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#ifdef CONFIG_DEBUG_BCM6846
/* This is needed for LL-debug/earlyprintk/debug-macro.S */
static struct map_desc bcm6846_io_desc[] __initdata = {
	{
		.virtual = CONFIG_DEBUG_UART_VIRT,
		.pfn = __phys_to_pfn(CONFIG_DEBUG_UART_PHYS),
		.length = SZ_4K,
		.type = MT_DEVICE,
	},
};

static void __init bcm6846_map_io(void)
{
	iotable_init(bcm6846_io_desc, ARRAY_SIZE(bcm6846_io_desc));
}
#else
#define bcm6846_map_io NULL
#endif

static const char * const bcm6846_dt_compat[] = {
	"brcm,bcm6846",
	NULL,
};

DT_MACHINE_START(BCM6846_DT, "BCM6846 Application Processor")
	.map_io = bcm6846_map_io,
	.dt_compat = bcm6846_dt_compat,
MACHINE_END
