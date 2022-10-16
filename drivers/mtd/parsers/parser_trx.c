// SPDX-License-Identifier: GPL-2.0-only
/*
 * Parser for TRX format partitions
 *
 * Copyright (C) 2012 - 2017 Rafał Miłecki <rafal@milecki.pl>
 * Copyright (C) 2022 Linus Walleij <linus.walleij@linaro.org>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

/* Maximum number of TRX headers that can be found in a flash */
#define TRX_PARSER_MAX_TRX		4
/*
 * Maximum number of partitions that can be found in total in a flash
 * 3 offsets and free space = 4.
 */
#define TRX_PARSER_MAX_PARTS		(TRX_PARSER_MAX_TRX * 4)

/* Magics */
#define TRX_MAGIC			0x30524448
#define UBI_EC_MAGIC			0x23494255	/* UBI# */

struct trx_header {
	uint32_t magic;
	uint32_t length;
	uint32_t crc32;
	uint16_t flags;
	uint16_t version;
	uint32_t offset[3];
} __packed;

static void parser_trx_dump_trx(struct trx_header *trx)
{
	pr_debug("TRX at %08x\n", (u32)trx);
	pr_debug("  MAGIC %08x\n", trx->magic);
	pr_debug("  LENGTH %08x\n", trx->length);
	pr_debug("  CRC32 %08x\n", trx->crc32);
	pr_debug("  FLAGS %04x\n", trx->flags);
	pr_debug("  VERSION %04x\n", trx->version);
	pr_debug("  OFFSET[0] %08x\n", trx->offset[0]);
	pr_debug("  OFFSET[1] %08x\n", trx->offset[1]);
	pr_debug("  OFFSET[2] %08x\n", trx->offset[2]);
}

static int parser_trx_data_part_name_and_extents(struct mtd_info *mtd,
						 uint64_t start, uint64_t end, int trx_index,
						 const char **out_name, uint64_t *out_size)
{
	uint32_t blocksize = mtd->erasesize;
	size_t bytes_read;
	uint64_t offset;
	uint64_t size;
	uint32_t magic;
	uint32_t tmp;
	char *name;
	int err;

	err  = mtd_read(mtd, start, sizeof(magic), &bytes_read, (uint8_t *)&magic);
	if (err && !mtd_is_bitflip(err)) {
		pr_err("mtd_read error while parsing (offset: 0x%08llx): %d\n",
		       offset, err);
		magic = 0x0;
	}

	/* Here other data partion types can be added as needed */
	switch (magic) {
	case UBI_EC_MAGIC:
		name = "ubi";
		break;
	default:
		magic = 0x0;
		name = "rootfs";
		break;
	}

	/* First partion is just "name", second is "name2" etc */
	if (trx_index > 0) {
		name = kasprintf(GFP_KERNEL, "%s%d", name, trx_index + 1);
		if (!name)
			return -ENOMEM;
	}

	/*
	 * Attempt to determine extents, first assume it is
	 * the rest of the TRX partition, then scan to check.
	 */
	size = end - start;
	if (magic == 0x0)
		goto out_no_scan;

	for (offset = start; offset <= end - blocksize;
	     offset += blocksize) {
		err  = mtd_read(mtd, offset, sizeof(tmp), &bytes_read, (uint8_t *)&tmp);
		if (err && !mtd_is_bitflip(err)) {
			pr_err("mtd_read error while parsing (offset: 0x%08llx): %d\n",
			       offset, err);
			break;
		}
		if (tmp != magic) {
			size = offset - start;
			break;
		}
	}

out_no_scan:
	*out_name = name;
	*out_size = size;

	return 0;
}

static uint64_t parser_trx_get_offset(uint64_t start, struct trx_header *trx, int i)
{
	/* Include the TRX header into the first partition */
	if ((i == 0) && (trx->offset[i] == sizeof(struct trx_header)))
		return start;
	return start + trx->offset[i];
}

static uint64_t parser_trx_get_size(uint64_t start, uint64_t end, struct trx_header *trx, int i)
{
	uint64_t this_offset;
	uint64_t next_offset;
	uint64_t retsz;

	/*
	 * Get the size from the next offset if that is > 0, else we assume
	 * we are the last partition in the TRX partition block, and then we
	 * use the rest of the available size.
	 */
	this_offset = parser_trx_get_offset(start, trx, i);
	if ((i < 2) && (trx->offset[i + 1] > 0))
		next_offset = parser_trx_get_offset(start, trx, i + 1);
	else
		next_offset = end;

	retsz = next_offset - this_offset;
	pr_debug("size of trx partion %d is %08llx\n", i, retsz);

	return retsz;
}

static const char *parser_trx_get_partname(const char *basename, int trx_index)
{
	if (trx_index == 0)
		return basename;
	return kasprintf(GFP_KERNEL, "%s%d", basename, trx_index + 1);
}

/*
 * This adds all the partitions found in a TRX partition block, which means
 * up to three individual partitions. Returns number of added partitions.
 */
static int parser_trx_add_trx_partition(struct mtd_info *mtd, struct trx_header *trx,
					int trx_index, uint64_t start, uint64_t end,
					struct mtd_partition *parts,
					int curr_part)
{
	struct mtd_partition *part;
	bool has_loader = false;
	uint64_t last_end;
	int i = 0;
	int ret;

	parser_trx_dump_trx(trx);

	/* We have an LZMA loader partition if there is an address in offset[2] */
	if (trx->offset[2])
		has_loader = true;

	if (has_loader) {
		part = &parts[curr_part + i];
		part->name = parser_trx_get_partname("loader", trx_index);
		if (!part->name)
			return -ENOMEM;
		part->offset = parser_trx_get_offset(start, trx, i);
		part->size = parser_trx_get_size(start, end, trx, i);
		i++;
	}

	if (trx->offset[i]) {
		part = &parts[curr_part + i];
		part->name = parser_trx_get_partname("linux", trx_index);
		if (!part->name)
			return -ENOMEM;
		part->offset = parser_trx_get_offset(start, trx, i);
		part->size = parser_trx_get_size(start, end, trx, i);
		i++;
	}

	if (trx->offset[i]) {
		part = &parts[curr_part + i];
		part->offset = parser_trx_get_offset(start, trx, i);
		part->size = parser_trx_get_size(start, end, trx, i);
		ret = parser_trx_data_part_name_and_extents(mtd, part->offset, end, trx_index,
							    &part->name, &part->size);
		if (ret)
			return ret;
		i++;
	}

	/*
	 * If there is free space after the last TRX add this as a separate
	 * partition. This happens when the data partition parser finds less than
	 * the space up until the end sector.
	 */
	last_end = part->offset + part->size;
	if (last_end < end) {
		part = &parts[curr_part + i];
		part->offset = last_end;
		part->size = end - last_end;
		part->name = "free";
		i++;
	}

	pr_debug("Added %d partitions\n", i);

	return i;
}

static int parser_trx_parse(struct mtd_info *mtd,
			    const struct mtd_partition **pparts,
			    struct mtd_part_parser_data *data)
{
	struct device_node *np = mtd_get_of_node(mtd);
	struct mtd_partition *parts;
	struct trx_header trxs[TRX_PARSER_MAX_TRX];
	uint64_t trx_offsets[TRX_PARSER_MAX_TRX];
	struct trx_header *trx;
	size_t bytes_read;
	int curr_part = 0;
	int found_trx = 0;
	int i;
	uint64_t offset;
	uint32_t blocksize = mtd->erasesize;
	uint32_t trx_magic = TRX_MAGIC;
	int ret;

	/* Get different magic from device tree if specified */
	ret = of_property_read_u32(np, "brcm,trx-magic", &trx_magic);
	if (ret && ret != -EINVAL)
		pr_err("failed to parse \"brcm,trx-magic\" DT attribute, using default: %d\n", ret);

	parts = kcalloc(TRX_PARSER_MAX_PARTS, sizeof(struct mtd_partition),
			GFP_KERNEL);
	if (!parts)
		return -ENOMEM;

	for (offset = 0; offset <= mtd->size - blocksize;
	     offset += blocksize) {

		trx = &trxs[found_trx];
		ret = mtd_read(mtd, offset, sizeof(*trx), &bytes_read, (u_char *)trx);
		if (ret) {
			pr_err("MTD reading error: %d\n", ret);
			goto out_free_parts;
		}

		if (trx->magic != trx_magic)
			continue;

		trx_offsets[found_trx] = offset;

		found_trx++;
	}

	for (i = 0; i < found_trx; i++) {
		uint64_t start;
		uint64_t end;

		trx = &trxs[i];
		start = trx_offsets[i];
		if (i < (found_trx - 1))
			end = trx_offsets[i + 1];
		else
			end = mtd->size;

		ret = parser_trx_add_trx_partition(mtd, trx, i, start, end,
						   parts, curr_part);
		if (ret < 0)
			goto out_free_parts;

		curr_part += ret;
	}

	*pparts = parts;
	return curr_part;

out_free_parts:
	kfree(parts);
	return ret;
};

static const struct of_device_id mtd_parser_trx_of_match_table[] = {
	{ .compatible = "brcm,trx" },
	{},
};
MODULE_DEVICE_TABLE(of, mtd_parser_trx_of_match_table);

static struct mtd_part_parser mtd_parser_trx = {
	.parse_fn = parser_trx_parse,
	.name = "trx",
	.of_match_table = mtd_parser_trx_of_match_table,
};
module_mtd_part_parser(mtd_parser_trx);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Parser for TRX format partitions");
