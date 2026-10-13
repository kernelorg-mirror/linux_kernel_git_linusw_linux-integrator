/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MFD_BCMBCA_PMB_H
#define __LINUX_MFD_BCMBCA_PMB_H

#include <linux/types.h>

/* Logical regmap addresses: 8-bit bus, 8-bit device, 16-bit word offset. */
#define BCMBCA_PMB_REG(bus, device, offset) \
	(((u32)(bus) << 24) | ((u32)(device) << 16) | (offset))

#endif
