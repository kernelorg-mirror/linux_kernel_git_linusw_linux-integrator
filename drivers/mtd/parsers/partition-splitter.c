// SPDX-License-Identifier: GPL-2.0-only
/*
 * MTD partition splitter: splits partitions based on names and content using
 * specific splitting plug-ins. This is intended for root filesystems on
 * flash.
 *
 * Copyright (C) 2009-2013 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2009-2013 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (C) 2012 Jonas Gorski <jogo@openwrt.org>
 * Copyright (C) 2013 Hauke Mehrtens <hauke@hauke-m.de>
 * Copyright (C) 2019 Linus Walleij <linus.walleij@linaro.org>
 */

#include <linux/kconfig.h>
#include <linux/module.h>
#include <linux/magic.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>
#include <linux/mtd/partitions.h>
#include "partition-splitter.h"

#define UBI_EC_MAGIC            0x55424923      /* UBI# */
#define SPLIT_FIRMWARE_NAME    "upgrade" // "firmware"

struct mtd_part_splitter{
	const char *name;
	int (*parse_fn)(struct mtd_info *, struct mtd_info *,
			uint64_t, struct mtd_info *,
			const struct mtd_partition **);
	enum mtd_part_splitter_type type;
};

/* Add the different types of splitters here */
static struct mtd_part_splitter __initdata splitter_table[] = {
	{
		.name = "wrgg-fw-splitter",
		.parse_fn = mtdsplit_parse_wrgg,
		.type = MTD_SPLITTER_TYPE_FIRMWARE,
	},
};

static int __init split_mtd_partitions_by_type(struct mtd_info *master,
					struct mtd_info *parent,
					uint64_t offset,
					struct mtd_info *splitme,
					enum mtd_part_splitter_type type,
					const struct mtd_partition **pparts)
{
	int ret = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(splitter_table); i++) {
		struct mtd_part_splitter *s;

		s = &splitter_table[i];
		if (s->type != type)
			continue;

		ret = (*s->parse_fn)(master, parent, offset, splitme, pparts);

		if (ret > 0) {
			printk(KERN_NOTICE
			       "%d %s partitions found on MTD device %s\n",
			       ret, s->name, splitme->name);
			break;
		}
	}
	return ret;
}

static int __init
run_splitters_by_type(struct mtd_info *master, struct mtd_info *parent,
		      uint64_t offset, struct mtd_info *splitme,
		      enum mtd_part_splitter_type type)
{
	struct mtd_partition *parts;
	int nr_parts;
	int i;

	nr_parts = split_mtd_partitions_by_type(master, parent, offset,
				splitme, type,
				(const struct mtd_partition **)&parts);
	if (nr_parts <= 0)
		return nr_parts;

	if (WARN_ON(!parts))
		return 0;

	for (i = 0; i < nr_parts; i++) {
		/* adjust partition offsets */
		parts[i].offset += offset;

		mtd_add_partition(parent,
				  parts[i].name,
				  parts[i].offset,
				  parts[i].size);
	}

	kfree(parts);

	return nr_parts;
}

static void __init split_firmware(struct mtd_info *master,
				  struct mtd_info *parent,
				  uint64_t offset, struct mtd_info *splitme)
{
	run_splitters_by_type(master, parent, offset, splitme,
			      MTD_SPLITTER_TYPE_FIRMWARE);
}

void __ref mtd_partition_split(struct mtd_info *master,
				struct mtd_info *parent,
				uint64_t offset, struct mtd_info *splitme)
{
	/* TODO : make sure this will just resturn after init */
	if (!strcmp(splitme->name, "rootfs")) {
		run_splitters_by_type(master, parent, offset, splitme,
				      MTD_SPLITTER_TYPE_ROOTFS);
	}

	if (!strcmp(splitme->name, SPLIT_FIRMWARE_NAME) &&
	    !of_find_property(mtd_get_of_node(splitme), "compatible", NULL))
		split_firmware(master, parent, offset, splitme);
}

static ssize_t __init mtd_next_eb(struct mtd_info *mtd, size_t offset)
{
	return mtd_rounddown_to_eb(offset, mtd) + mtd->erasesize;
}

static int __init mtd_check_rootfs_magic(struct mtd_info *mtd, size_t offset,
				enum mtd_part_splitter_rootfs_type *type)
{
	u32 magic;
	size_t retlen;
	int ret;

	ret = mtd_read(mtd, offset, sizeof(magic), &retlen,
		       (unsigned char *) &magic);
	if (ret)
		return ret;

	if (retlen != sizeof(magic))
		return -EIO;

	if (le32_to_cpu(magic) == SQUASHFS_MAGIC) {
		dev_info(&mtd->dev, "found squashfs rootfs @0x%zx\n", offset);
		if (type)
			*type = MTD_SPLITTER_ROOTFS_TYPE_SQUASHFS;
		return 0;
	} else if (magic == 0x19852003) {
		dev_info(&mtd->dev, "found JFFS2 rootfs @0x%zx\n", offset);
		if (type)
			*type = MTD_SPLITTER_ROOTFS_TYPE_JFFS2;
		return 0;
	} else if (be32_to_cpu(magic) == UBI_EC_MAGIC) {
		dev_info(&mtd->dev, "found UBI rootfs @0x%zx\n", offset);
		if (type)
			*type = MTD_SPLITTER_ROOTFS_TYPE_UBI;
		return 0;
	}

	return -EINVAL;
}

int __init mtd_find_rootfs_from(struct mtd_info *mtd,
				size_t from,
				size_t limit,
				size_t *ret_offset,
				enum mtd_part_splitter_rootfs_type *type)
{
	size_t offset;
	int err;

	for (offset = from; offset < limit;
	     offset = mtd_next_eb(mtd, offset)) {
		err = mtd_check_rootfs_magic(mtd, offset, type);
		if (err)
			continue;

		*ret_offset = offset;
		return 0;
	}

	return -ENODEV;
}
