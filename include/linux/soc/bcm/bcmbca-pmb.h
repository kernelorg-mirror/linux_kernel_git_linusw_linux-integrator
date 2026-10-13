/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_BCM_BCMBCA_PMB_H
#define __LINUX_SOC_BCM_BCMBCA_PMB_H

#include <linux/types.h>

struct device;
struct bcmbca_pmb;

/* The parent bus device owns the Processor Monitor Bus (PMB) transport. */
struct bcmbca_pmb *bcmbca_pmb_get(struct device *dev);

/*
 * Processor Monitor Bus (PMB) operations sleep. Hold the transport lock
 * across a complete sequence, including read/modify/write operations.
 * A successful lock must be paired with unlock, including on errors.
 */
int bcmbca_pmb_lock(struct bcmbca_pmb *pmb);
void bcmbca_pmb_unlock(struct bcmbca_pmb *pmb);
int bcmbca_pmb_read(struct bcmbca_pmb *pmb, u8 bus, u8 device,
		    u16 offset, u32 *value);
int bcmbca_pmb_write(struct bcmbca_pmb *pmb, u8 bus, u8 device,
		     u16 offset, u32 value);

#endif
