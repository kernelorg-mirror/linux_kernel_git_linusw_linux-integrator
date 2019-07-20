// SPDX-License-Identifier: GPL-2.0-only
/*
 * Splitter for squashfs root filesystems.
 *
 * Copyright (C) 2013 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2013 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (C) 2019 Linus Walleij <linus.walleij@linaro.org>
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/magic.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/byteorder/generic.h>

#include "partition-splitter.h"

struct squashfs_super_block {
	__le32 s_magic;
	__le32 pad0[9];
	__le64 bytes_used;
};

static int __init mtd_get_squashfs_len(struct mtd_info *master,
				       size_t offset,
				       size_t *squashfs_len)
{
	struct squashfs_super_block sb;
	size_t retlen;
	int ret;

	ret = mtd_read(master, offset, sizeof(sb), &retlen, (void *)&sb);
	if (ret || (retlen != sizeof(sb))) {
		pr_alert("error occured while reading from \"%s\"\n",
			 master->name);
		return -EIO;
	}

	if (le32_to_cpu(sb.s_magic) != SQUASHFS_MAGIC) {
		pr_alert("no squashfs found in \"%s\"\n", master->name);
		return -EINVAL;
	}

	retlen = le64_to_cpu(sb.bytes_used);
	if (retlen <= 0) {
		pr_alert("squashfs is empty in \"%s\"\n", master->name);
		return -ENODEV;
	}

	if (offset + retlen > master->size) {
		pr_alert("squashfs has invalid size in \"%s\"\n",
			 master->name);
		return -EINVAL;
	}

	*squashfs_len = retlen;
	return 0;
}

int __init mtdsplit_parse_squashfs(struct mtd_info *master,
				   struct mtd_info *parent,
				   uint64_t offset,
				   struct mtd_info *splitme,
				   const struct mtd_partition **pparts)
{
	struct mtd_partition *part;
	size_t squashfs_len;
	int ret;

	ret = mtd_get_squashfs_len(splitme, 0, &squashfs_len);
	if (ret)
		return ret;

	part = kzalloc(sizeof(*part), GFP_KERNEL);
	if (!part) {
		pr_alert("unable to allocate memory for \"%s\" partition\n",
			 ROOTFS_PART_NAME);
		return -ENOMEM;
	}

	part->name = ROOTFS_PART_NAME;
	part->offset = mtd_roundup_to_eb(offset + squashfs_len,
					 parent) - offset;
	part->size = mtd_rounddown_to_eb(splitme->size - part->offset,
					 splitme);

	*pparts = part;
	return 1;
}
