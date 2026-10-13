/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
#ifndef _DT_BINDINGS_RESET_BRCM_BCM6846_PMB_H
#define _DT_BINDINGS_RESET_BRCM_BCM6846_PMB_H

/* XRDP packet engine and its three Runner cores. */
#define BCM6846_RESET_XRDP	0

/* PCIe PMB soft resets, preserving the lane configuration. */
#define BCM6846_RESET_PCIE0	1
#define BCM6846_RESET_PCIE1	2

/* Process, voltage and temperature monitor analog acquisition reset. */
#define BCM6846_RESET_PVTMON	3

#endif
