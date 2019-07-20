/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef PARTITION_SPLITTER_H
#define PARTITION_SPLITTER_H

#include <linux/types.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

#define KERNEL_PART_NAME	"kernel"
#define ROOTFS_PART_NAME	"rootfs"

#ifdef CONFIG_MTD_PARTITION_SPLITTER

enum mtd_part_splitter_type {
	MTD_SPLITTER_TYPE_DEVICE = 0,
	MTD_SPLITTER_TYPE_ROOTFS,
	MTD_SPLITTER_TYPE_FIRMWARE,
};

enum mtd_part_splitter_rootfs_type {
	MTD_SPLITTER_ROOTFS_TYPE_UNKNOWN = 0,
	MTD_SPLITTER_ROOTFS_TYPE_SQUASHFS,
	MTD_SPLITTER_ROOTFS_TYPE_JFFS2,
	MTD_SPLITTER_ROOTFS_TYPE_UBI,
};

void mtd_partition_split(struct mtd_info *master, struct mtd_info *parent,
			 uint64_t offset, struct mtd_info *splitme);

int mtd_find_rootfs_from(struct mtd_info *mtd,
			 size_t from,
			 size_t limit,
			 size_t *ret_offset,
			 enum mtd_part_splitter_rootfs_type *type);

/* Just line up all splitters here so we can put then in the array */
int mtdsplit_parse_wrgg(struct mtd_info *master,
			struct mtd_info *parent,
			uint64_t offset,
			struct mtd_info *splitme,
			const struct mtd_partition **pparts);
int mtdsplit_parse_squashfs(struct mtd_info *master,
			    struct mtd_info *parent,
			    uint64_t offset,
			    struct mtd_info *splitme,
			    const struct mtd_partition **pparts);

#else

static inline void mtd_partition_split(struct mtd_info *master,
				       struct mtd_info *parent,
				       uint64_t offset,
				       struct mtd_info *splitme)
{
}

#endif

#endif /* PARTITION_SPLITTER_H */
