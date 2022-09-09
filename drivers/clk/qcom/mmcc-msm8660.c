// SPDX-License-Identifier: GPL-2.0-only
/*
 * Multimedia Clock Controller driver for MSM8660 and APQ8060
 * Based on clock-8x60.c
 */
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#include <dt-bindings/clock/qcom,mmcc-msm8660.h>
#include <dt-bindings/reset/qcom,mmcc-msm8660.h>

#include "common.h"
#include "clk-regmap.h"
#include "clk-pll.h"
#include "clk-rcg.h"
#include "clk-branch.h"
#include "reset.h"

enum {
	P_PXO,
	P_PLL2, /* Also known as MMSS_PLL1 */
	P_PLL3, /* Also known as MMSS_PLL2 */
	P_PLL8,
	P_DSI_PLL_BYTECLK,
};

#define F_MN(f, s, _m, _n) { .freq = f, .src = s, .m = _m, .n = _n }

static const struct parent_map mmcc_pxo_map[] = {
	{ P_PXO, 0 },
};

static const char * const mmcc_pxo[] = {
	"pxo",
};

static const struct parent_map mmcc_pxo_pll8_pll2_map[] = {
	{ P_PXO, 0 },
	{ P_PLL8, 2 },
	{ P_PLL2, 1 }, /* Also known as MMSS_PLL1 */
};

static const char * const mmcc_pxo_pll8_pll2[] = {
	"pxo",
	"pll8_vote",
	"pll2",
};

static const struct parent_map mmcc_pll8_pll2_map[] = {
	{ P_PLL8, 2 },
	{ P_PLL2, 1 }, /* Also known as MMSS_PLL1 */
};

static const char * const mmcc_pll8_pll2[] = {
	"pll8_vote",
	"pll2",
};

static const struct parent_map mmcc_pxo_dsi_byte_map[] = {
	{ P_PXO, 0 },
	{ P_DSI_PLL_BYTECLK, 1 },
};

static const char * const mmcc_pxo_dsi_byte[] = {
	"pxo",
	"dsipllbyte",
};


/*
 * PLL2 is also known as MM_PLL1
 * Registers from Stephens old patch from 2010:
 * http://lists.infradead.org/pipermail/linux-arm-kernel/2010-December/035458.html
 */
static struct clk_pll pll2 = {
	.l_reg = 0x320,
	.m_reg = 0x324,
	.n_reg = 0x328,
	.config_reg = 0x32c,
	.mode_reg = 0x31c,
	.status_reg = 0x334,
	.status_bit = 16, /* FIXME: correct? */
	.clkr.hw.init = &(struct clk_init_data){
		.name = "pll2",
		.parent_names = (const char *[]){ "pxo" },
		.num_parents = 1,
		.ops = &clk_pll_ops,
	},
};

/* PLL3 is also known as MM_PLL2, uses same registers as MSM8960 PLL15 */
static struct clk_pll pll3 = {
	.l_reg = 0x33c,
	.m_reg = 0x340,
	.n_reg = 0x344,
	.config_reg = 0x348,
	.mode_reg = 0x338,
	.status_reg = 0x350,
	.status_bit = 16, /* FIXME: correct? */
	.clkr.hw.init = &(struct clk_init_data){
		.name = "pll3",
		.parent_names = (const char *[]){ "pxo" },
		.num_parents = 1,
		.ops = &clk_pll_ops,
	},
};

/* CHECKME: copied verbatim from MSM8960 PLL15 config */
static const struct pll_config pll3_config = {
	.l = 33,
	.m = 1,
	.n = 3,
	.vco_val = 0x2 << 16,
	.vco_mask = 0x3 << 16,
	.pre_div_val = 0x0,
	.pre_div_mask = BIT(19),
	.post_div_val = 0x0,
	.post_div_mask = 0x3 << 20,
	.mn_ena_mask = BIT(22),
	.main_output_mask = BIT(23),
};

/* AXI peripheral logic clocks */

static struct clk_branch gmem_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 6,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(24),
		.hw.init = &(struct clk_init_data){
			.name = "gmem_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch ijpeg_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 4,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(21),
		.hw.init = &(struct clk_init_data){
			.name = "ijpeg_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

/* Has a retain bit, unhandled */
static struct clk_branch imem_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 7,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(22),
		.hw.init = &(struct clk_init_data){
			.name = "imem_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch jpegd_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 5,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(25),
		.hw.init = &(struct clk_init_data){
			.name = "jpegd_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

/* Has a retain bit, unhandled */
static struct clk_branch mdp_axi_clk = {
	.hwcg_reg = 0x0018,
	.hwcg_bit = 16, /* checkme: lifted from MSM8960 */
	.halt_reg = 0x01d8,
	.halt_bit = 8,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(23),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vcodec_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 3,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(19),
		.hw.init = &(struct clk_init_data){
			.name = "vcodec_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vfe_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 0,
	.clkr = {
		.enable_reg = 0x0018,
		.enable_mask = BIT(18),
		.hw.init = &(struct clk_init_data){
			.name = "vfe_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch rot_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 2,
	.clkr = {
		.enable_reg = 0x0020,
		.enable_mask = BIT(24),
		.hw.init = &(struct clk_init_data){
			.name = "rot_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vpe_axi_clk = {
	.halt_reg = 0x01d8,
	.halt_bit = 1,
	.clkr = {
		.enable_reg = 0x0020,
		.enable_mask = BIT(26),
		.hw.init = &(struct clk_init_data){
			.name = "vpe_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch smi_2x_axi_clk = {
	.halt_reg = 0x01e8,
	.halt_bit = 0,
	.clkr = {
		.enable_reg = 0x0020,
		.enable_mask = BIT(30),
		.hw.init = &(struct clk_init_data){
			.name = "smi_2x_axi_clk",
			.ops = &clk_branch_ops,
		},
	},
};


/* AHB peripheral logic clocks */

static struct clk_branch amp_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 18,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(24),
		.hw.init = &(struct clk_init_data){
			.name = "amp_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch csi0_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 16,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(7),
		.hw.init = &(struct clk_init_data){
			.name = "csi0_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};


static struct clk_branch csi1_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 17,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(20),
		.hw.init = &(struct clk_init_data){
			.name = "csi1_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch dsi_m_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 19,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(9),
		.hw.init = &(struct clk_init_data){
			.name = "dsi_m_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch dsi_s_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 20,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(18),
		.hw.init = &(struct clk_init_data){
			.name = "dsi_s_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch gfx2d0_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 2,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(19),
		.hw.init = &(struct clk_init_data){
			.name = "gfx2d0_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch gfx2d1_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 3,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "gfx2d1_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch gfx3d_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 4,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(3),
		.hw.init = &(struct clk_init_data){
			.name = "gfx3d_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch hdmi_m_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 5,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(14),
		.hw.init = &(struct clk_init_data){
			.name = "hdmi_m_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch hdmi_s_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 6,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(4),
		.hw.init = &(struct clk_init_data){
			.name = "hdmi_s_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch ijpeg_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 9,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(5),
		.hw.init = &(struct clk_init_data){
			.name = "ijpeg_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch imem_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 10,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(6),
		.hw.init = &(struct clk_init_data){
			.name = "imem_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch jpegd_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 7,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(21),
		.hw.init = &(struct clk_init_data){
			.name = "jpegd_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch mdp_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 11,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(10),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch rot_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 13,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(12),
		.hw.init = &(struct clk_init_data){
			.name = "rot_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch smmu_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 22,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(15),
		.hw.init = &(struct clk_init_data){
			.name = "smmu_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch tv_enc_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 23,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(25),
		.hw.init = &(struct clk_init_data){
			.name = "tv_enc_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vcodec_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 12,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(11),
		.hw.init = &(struct clk_init_data){
			.name = "vcodec_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

/*
 * FIXME: In the vendor tree this VFE clock also has "retain" bit 0
 * in register AHB_EN2_REG 0x0038. This seems unsupported by the current
 * qualcomm clock framework.
 */
static struct clk_branch vfe_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 14,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(13),
		.hw.init = &(struct clk_init_data){
			.name = "vfe_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct clk_branch vpe_ahb_clk = {
	.halt_reg = 0x01dc,
	.halt_bit = 15,
	.clkr = {
		.enable_reg = 0x0008,
		.enable_mask = BIT(16),
		.hw.init = &(struct clk_init_data){
			.name = "vpe_ahb_clk",
			.ops = &clk_branch_ops,
		},
	},
};

static struct freq_tbl clk_tbl_mdp[] = {
	{   9600000, P_PLL8, 1, 1, 40 },
	{  13710000, P_PLL8, 1, 1, 28 },
	{  27000000, P_PXO,  1, 0,  0 },
	{  29540000, P_PLL8, 1, 1, 13 },
	{  34910000, P_PLL8, 1, 1, 11 },
	{  38400000, P_PLL8, 1, 1, 10 },
	{  59080000, P_PLL8, 1, 2, 13 },
	{  76800000, P_PLL8, 1, 1,  5 },
	{  85330000, P_PLL8, 1, 2,  9 },
	{  96000000, P_PLL8, 1, 1,  4 },
	{ 128000000, P_PLL8, 1, 1,  3 },
	{ 160000000, P_PLL2, 1, 1,  5 },
	{ 177780000, P_PLL2, 1, 2,  9 },
	{ 200000000, P_PLL2, 1, 1,  4 },
	{ }
};

static struct clk_dyn_rcg mdp_src = {
	.ns_reg[0] = 0x00d0,
	.ns_reg[1] = 0x00d0,
	.md_reg[0] = 0x00c4,
	.md_reg[1] = 0x00c8,
	.bank_reg = 0x00c0,
	.mn[0] = {
		.mnctr_en_bit = 8, /* mnd_en_mask */
		.mnctr_reset_bit = 31, /* rst_mask */
		.mnctr_mode_shift = 9, /* mode_mask bits */
		.n_val_shift = 22, /* ns_mask high bits */
		.m_val_shift = 8,
		.width = 8,
	},
	.mn[1] = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 30,
		.mnctr_mode_shift = 6, /* mode_mask */
		.n_val_shift = 14,
		.m_val_shift = 8,
		.width = 8,
	},
	.s[0] = {
		.src_sel_shift = 3, /* ns_mask low bits */
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.s[1] = {
		.src_sel_shift = 0, /* ns_mask low bits */
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.mux_sel_bit = 11, /* bank_sel_mask */
	.freq_tbl = clk_tbl_mdp,
	.clkr = {
		.enable_reg = 0x00c0,
		.enable_mask = BIT(2), /* root_en_mask */
		.hw.init = &(struct clk_init_data){
			.name = "mdp_src",
			.parent_names = mmcc_pxo_pll8_pll2,
			.num_parents = 3,
			.ops = &clk_dyn_rcg_ops,
		},
	},
};

static struct clk_branch mdp_clk = {
	.halt_reg = 0x01d0,
	.halt_bit = 10,
	.clkr = {
		.enable_reg = 0x00c0,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_clk",
			.parent_names = (const char *[]){ "mdp_src" },
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct freq_tbl clk_tbl_mdp_vsync[] = {
	{  27000000, P_PXO,  1, 0,  0 },
	{ }
};

/*
 * The VSYNC clock needs to be a src + branch clock because of having
 * a halt register (branch) but also a source selection register with
 * one single bit (!)
 */
static struct clk_rcg mdp_vsync_src = {
	.ns_reg = 0x005c,
	/*
	 * No .mn so mn.width == 0 and no scaling happens,
	 * no prescaling either.
	 */
	.s = {
		.src_sel_shift = 13, /* ns_mask low bits */
		.parent_map = mmcc_pxo_map,
	},
	.freq_tbl = clk_tbl_mdp_vsync,
	.clkr = {
		/* No root enable bit */
		.hw.init = &(struct clk_init_data){
			.name = "mdp_vsync_src",
			.parent_names = mmcc_pxo,
			.num_parents = 1,
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch mdp_vsync_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 22,
	.clkr = {
		.enable_reg = 0x0058,
		.enable_mask = BIT(6),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_vsync_clk",
			.parent_names = (const char *[]){ "mdp_vsync_src" },
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct freq_tbl clk_tbl_mdp_pixel[] = {
	{  25600000, P_PLL8, 3,   1,    5 },
	{  42667000, P_PLL8, 1,   1,    9 },
	{  43192000, P_PLL8, 1,  64,  569 },
	{  48000000, P_PLL8, 4,   1,    2 },
	{  53990000, P_PLL8, 2, 169,  601 },
	{  64000000, P_PLL8, 2,   1,    3 },
	{  69300000, P_PLL8, 1, 231, 1280 },
	{  76800000, P_PLL8, 1,   1,    5 },
	{  85333000, P_PLL8, 1,   2,    9 },
	{  96000000, P_PLL8, 1,   1,    4 },
	{ 106500000, P_PLL8, 1,  71,  256 },
	{ 109714000, P_PLL8, 1,   2,    7 },
	{ }
};

static struct clk_rcg mdp_pixel_src = {
	.ns_reg = 0x00dc,
	.md_reg = 0x00d8,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 7,
		.mnctr_mode_shift = 6,
		.n_val_shift = 16,
		.m_val_shift = 8,
		.width = 16,
	},
	.p = {
		.pre_div_shift = 14,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_mdp_pixel,
	.clkr = {
		.enable_reg = 0x00d4,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_pixel_src",
			.parent_names = mmcc_pxo_pll8_pll2,
			.num_parents = 3,
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch mdp_pixel_clk = {
	.halt_reg = 0x01d0,
	.halt_bit = 23,
	.clkr = {
		.enable_reg = 0x00d4,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_pixel_clk",
			.parent_names = (const char *[]){ "mdp_pixel_src" },
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch mdp_lcdc_clk = {
	.halt_reg = 0x01d0,
	.halt_bit = 21,
	.clkr = {
		.enable_reg = 0x00d4,
		.enable_mask = BIT(8),
		.hw.init = &(struct clk_init_data){
			.name = "mdp_lcdc_clk",
			.parent_names = (const char *[]){ "mdp_pixel_clk" },
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct freq_tbl clk_tbl_cam[] = {
	{   6000000, P_PLL8, 4, 1, 16 },
	{   8000000, P_PLL8, 4, 1, 12 },
	{  12000000, P_PLL8, 4, 1,  8 },
	{  16000000, P_PLL8, 4, 1,  6 },
	{  19200000, P_PLL8, 4, 1,  5 },
	{  24000000, P_PLL8, 4, 1,  4 },
	{  32000000, P_PLL8, 4, 1,  3 },
	{  48000000, P_PLL8, 4, 1,  2 },
	{  64000000, P_PLL8, 3, 1,  2 },
	{  96000000, P_PLL8, 4, 0,  0 },
	{ 128000000, P_PLL8, 3, 0,  0 },
	{ }
};

static struct clk_rcg cam_src = {
	.ns_reg = 0x0148,
	.md_reg = 0x0144,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 8,
		.reset_in_cc = true,
		.mnctr_mode_shift = 6, /* ctl_mask */
		.n_val_shift = 24,
		.m_val_shift = 8,
		.width = 8,
	},
	.p = {
		.pre_div_shift = 14, /* ns_mask mid bits */
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0, /* ns_mask low bits */
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_cam,
	.clkr = {
		.enable_reg = 0x0140,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "cam_src",
			.parent_names = mmcc_pxo_pll8_pll2,
			.num_parents = 3,
			.ops = &clk_rcg_ops,
		},
	},
};

static struct clk_branch cam_clk = {
	.halt_check = BRANCH_HALT_DELAY,
	.clkr = {
		.enable_reg = 0x0140,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "cam_clk",
			.parent_names = (const char *[]){ "cam_src" },
			.num_parents = 1,
			.ops = &clk_branch_ops,
		},
	},

};

/*
 * FIXME: CSI clock only has ns_reg, uncertain how to handle this,
 * beware of bugs in this clock. It seems to be a simple "select one
 * of two" passthrough mux.
 */
static struct freq_tbl clk_tbl_csi[] = {
	{ 192000000, P_PLL8, 2, 0, 0 },
	{ 384000000, P_PLL2, 1, 0, 0 },
	{ }
};

static struct clk_rcg csi_src = {
	.ns_reg = 0x0048,
	.mn = {
		.reset_in_cc = true,
		.n_val_shift = 12, /* ns_mask high bits */
		.m_val_shift = 8,
		.width = 8,
	},
	.s = {
		.src_sel_shift = 0, /* ns_mask low bits */
		.parent_map = mmcc_pll8_pll2_map,
	},
	.freq_tbl = clk_tbl_csi,
	.clkr = {
		.enable_reg = 0x0040,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "csi_src",
			.parent_names = mmcc_pll8_pll2,
			.num_parents = 2,
			.ops = &clk_rcg_ops, /* rcg_bypass_ops? */
		},
	},
};

static struct clk_branch csi0_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 13,
	.clkr = {
		.enable_reg = 0x0040,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.parent_names = (const char *[]){ "csi_src" },
			.num_parents = 1,
			.name = "csi0_clk",
			.ops = &clk_branch_ops,
			/* Certainly not CLK_SET_RATE_PARENT */
		},
	},
};

static struct clk_branch csi1_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 14,
	.clkr = {
		.enable_reg = 0x0040,
		.enable_mask = BIT(7),
		.hw.init = &(struct clk_init_data){
			.parent_names = (const char *[]){ "csi_src" },
			.num_parents = 1,
			.name = "csi1_clk",
			.ops = &clk_branch_ops,
			/* Certainly not CLK_SET_RATE_PARENT */
		},
	},
};

static struct clk_rcg dsi_byte_src = {
	.ns_reg = 0x005c,
	.p = {
		.pre_div_shift = 24, /* ns_mask */
		.pre_div_width = 4,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_dsi_byte_map,
	},
	.clkr = {
		.enable_reg = 0x0058,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "dsi_byte_src",
			.parent_names = mmcc_pxo_dsi_byte,
			.num_parents = 2,
			.ops = &clk_rcg_bypass2_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch dsi_byte_clk = {
	.halt_check = BRANCH_HALT_DELAY,
	.clkr = {
		.enable_reg = 0x0058, /* Guesswork, not present in vendor tree */
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "dsi_byte_clk",
			.parent_names = (const char *[]){ "dsi_byte_src" },
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch dsi_esc_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 24,
	.clkr = {
		.enable_reg = 0x0058,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "dsi_esc_clk",
			.ops = &clk_branch_ops,
		},
	},
};

/* Adreno 3D engine core clock */
static struct freq_tbl clk_tbl_gfx3d[] = {
	F_MN( 27000000, P_PXO,  0,  0),
	F_MN( 48000000, P_PLL8, 1,  8),
	F_MN( 54857000, P_PLL8, 1,  7),
	F_MN( 64000000, P_PLL8, 1,  6),
	F_MN( 76800000, P_PLL8, 1,  5),
	F_MN( 96000000, P_PLL8, 1,  4),
	F_MN(128000000, P_PLL8, 1,  3),
	F_MN(145455000, P_PLL2, 2, 11),
	F_MN(160000000, P_PLL2, 1,  5),
	F_MN(177778000, P_PLL2, 2,  9),
	F_MN(200000000, P_PLL2, 1,  4),
	F_MN(228571000, P_PLL2, 2,  7),
	F_MN(266667000, P_PLL2, 1,  3),
	F_MN(320000000, P_PLL2, 2,  5),
	{ }
};

static struct clk_dyn_rcg gfx3d_src = {
	.ns_reg[0] = 0x008c,
	.ns_reg[1] = 0x008c,
	.md_reg[0] = 0x0084,
	.md_reg[1] = 0x0088,
	.bank_reg = 0x0080,
	.mn[0] = {
		.mnctr_en_bit = 8,
		.mnctr_reset_bit = 25,
		.mnctr_mode_shift = 9,
		.n_val_shift = 18,
		.m_val_shift = 4,
		.width = 4,
	},
	.mn[1] = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 24,
		.mnctr_mode_shift = 6,
		.n_val_shift = 14,
		.m_val_shift = 4,
		.width = 4,
	},
	.s[0] = {
		.src_sel_shift = 3,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.s[1] = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll8_pll2_map,
	},
	.mux_sel_bit = 11,
	.freq_tbl = clk_tbl_gfx3d,
	.clkr = {
		.enable_reg = 0x0080,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "gfx3d_src",
			.parent_names = mmcc_pxo_pll8_pll2,
			.num_parents = 3,
			.ops = &clk_dyn_rcg_ops,
		},
	},
};

static struct clk_branch gfx3d_clk = {
	.halt_reg = 0x01c8,
	.halt_bit = 4,
	.clkr = {
		.enable_reg = 0x0080,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gfx3d_clk",
			.parent_names = (const char *[]){ "gfx3d_src" },
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

/* TODO: Add ROT clock here */

/*
 * TV clock definitions
 *
 * We assume the three lower bits in NS selects source and can
 * select also PXO though the vendor driver hardcodes this to
 * pll3 (pll3_to_mm_mux, value 3)
 */
static const struct parent_map mmcc_pxo_pll3_map[] = {
	{ P_PXO, 0 },
	{ P_PLL3, 3 }
};

static const char * const mmcc_pxo_pll3_name[] = {
	"pxo",
	"pll3",
};

static struct freq_tbl clk_tbl_tv[] = {
	{ 25200000, P_PLL3, 2, 0, 0 },
	{ 27000000, P_PLL3, 2, 0, 0 },
	{ 27030000, P_PLL3, 4, 0, 0 },
	{ 74250000, P_PLL3, 2, 0, 0 },
	{ 148500000, P_PLL3, 2, 0, 0 },
	{ }
};

static struct clk_rcg tv_src = {
	.ns_reg = 0x00f4,
	.md_reg = 0x00f0,
	.mn = {
		.mnctr_en_bit = 5,
		.mnctr_reset_bit = 7,
		.mnctr_mode_shift = 6,
		.n_val_shift = 16,
		.m_val_shift = 8,
		.width = 8,
	},
	.p = {
		.pre_div_shift = 14,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = mmcc_pxo_pll3_map,
	},
	.freq_tbl = clk_tbl_tv,
	.clkr = {
		.enable_reg = 0x00ec,
		.enable_mask = BIT(2),
		.hw.init = &(struct clk_init_data){
			.name = "tv_src",
			.parent_names = mmcc_pxo_pll3_name,
			.num_parents = 2,
			.ops = &clk_rcg_bypass_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static const char * const tv_src_name[] = { "tv_src" };

static struct clk_branch tv_enc_clk = {
	.halt_reg = 0x01d4,
	.halt_bit = 8,
	.halt_check = BRANCH_HALT_SKIP,
	.clkr = {
		.enable_reg = 0x00ec,
		.enable_mask = BIT(8),
		.hw.init = &(struct clk_init_data){
			.parent_names = tv_src_name,
			.num_parents = 1,
			.name = "tv_enc_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch tv_dac_clk = {
	.halt_reg = 0x01d4,
	.halt_bit = 9,
	.clkr = {
		.enable_reg = 0x00ec,
		.enable_mask = BIT(10),
		.hw.init = &(struct clk_init_data){
			.parent_names = tv_src_name,
			.num_parents = 1,
			.name = "tv_dac_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch mdp_tv_clk = {
	.halt_reg = 0x01d4,
	.halt_bit = 11,
	.clkr = {
		.enable_reg = 0x00ec,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.parent_names = tv_src_name,
			.num_parents = 1,
			.name = "mdp_tv_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_branch hdmi_tv_clk = {
	.halt_reg = 0x01d4,
	.halt_bit = 10,
	.clkr = {
		.enable_reg = 0x00ec,
		.enable_mask = BIT(12),
		.hw.init = &(struct clk_init_data){
			.parent_names = tv_src_name,
			.num_parents = 1,
			.name = "hdmi_tv_clk",
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

/* This clock seems to not have any clearly indicated parent? */
static struct clk_branch hdmi_app_clk = {
	.halt_reg = 0x01cc,
	.halt_bit = 25,
	.clkr = {
		.enable_reg = 0x005c,
		.enable_mask = BIT(11),
		.hw.init = &(struct clk_init_data){
			.name = "hdmi_app_clk",
			.ops = &clk_branch_ops,
		},
	},
};

/* TODO: implement VCODEC, VPE and VFE clocks */

static struct clk_regmap *mmcc_msm8660_clks[] = {
	/* AXI clocks */
	[GMEM_AXI_CLK] = &gmem_axi_clk.clkr,
	[IJPEG_AXI_CLK] = &ijpeg_axi_clk.clkr,
	[IMEM_AXI_CLK] = &imem_axi_clk.clkr,
	[JPEGD_AXI_CLK] = &jpegd_axi_clk.clkr,
	[MDP_AXI_CLK] = &mdp_axi_clk.clkr,
	[VCODEC_AXI_CLK] = &vcodec_axi_clk.clkr,
	[VFE_AXI_CLK] = &vfe_axi_clk.clkr,
	[ROT_AXI_CLK] = &rot_axi_clk.clkr,
	[VPE_AXI_CLK] = &vpe_axi_clk.clkr,
	[SMI_2X_AXI_CLK] = &smi_2x_axi_clk.clkr,
	/* AHB clocks all using AHB_EN_REG */
	[AMP_AHB_CLK] = &amp_ahb_clk.clkr,
	[CSI0_AHB_CLK] = &csi0_ahb_clk.clkr,
	[CSI1_AHB_CLK] = &csi1_ahb_clk.clkr,
	[DSI_M_AHB_CLK] = &dsi_m_ahb_clk.clkr,
	[DSI_S_AHB_CLK] = &dsi_s_ahb_clk.clkr,
	[GFX2D0_AHB_CLK] = &gfx2d0_ahb_clk.clkr,
	[GFX2D1_AHB_CLK] = &gfx2d1_ahb_clk.clkr,
	[GFX3D_AHB_CLK] = &gfx3d_ahb_clk.clkr,
	[HDMI_M_AHB_CLK] = &hdmi_m_ahb_clk.clkr,
	[HDMI_S_AHB_CLK] = &hdmi_s_ahb_clk.clkr,
	[IJPEG_AHB_CLK] = &ijpeg_ahb_clk.clkr,
	[IMEM_AHB_CLK] = &imem_ahb_clk.clkr,
	[JPEGD_AHB_CLK] = &jpegd_ahb_clk.clkr,
	[MDP_AHB_CLK] = &mdp_ahb_clk.clkr,
	[ROT_AHB_CLK] = &rot_ahb_clk.clkr,
	[SMMU_AHB_CLK] = &smmu_ahb_clk.clkr,
	[TV_ENC_AHB_CLK] = &tv_enc_ahb_clk.clkr,
	[VCODEC_AHB_CLK] = &vcodec_ahb_clk.clkr,
	[VFE_AHB_CLK] = &vfe_ahb_clk.clkr,
	[VPE_AHB_CLK] = &vpe_ahb_clk.clkr,
	[GFX3D_SRC] = &gfx3d_src.clkr,
	[GFX3D_CLK] = &gfx3d_clk.clkr,
	[MDP_SRC] = &mdp_src.clkr,
	[MDP_CLK] = &mdp_clk.clkr,
	[MDP_VSYNC_SRC] = &mdp_vsync_src.clkr,
	[MDP_VSYNC_CLK] = &mdp_vsync_clk.clkr,
	[MDP_PIXEL_SRC] = &mdp_pixel_src.clkr,
	[MDP_PIXEL_CLK] = &mdp_pixel_clk.clkr,
	[MDP_LCDC_CLK] = &mdp_lcdc_clk.clkr,
	[CAM_SRC] = &cam_src.clkr,
	[CAM_CLK] = &cam_clk.clkr,
	[CSI_SRC] = &csi_src.clkr,
	[CSI0_CLK] = &csi0_clk.clkr,
	[CSI1_CLK] = &csi1_clk.clkr,
	[DSI_BYTE_SRC] = &dsi_byte_src.clkr,
	[DSI_BYTE_CLK] = &dsi_byte_clk.clkr,
	[DSI_ESC_CLK] = &dsi_esc_clk.clkr,
	[TV_SRC] = &tv_src.clkr,
	[TV_ENC_CLK] = &tv_enc_clk.clkr,
	[TV_DAC_CLK] = &tv_dac_clk.clkr,
	[HDMI_TV_CLK] = &hdmi_tv_clk.clkr,
	[HDMI_APP_CLK] &hdmi_app_clk.clkr,
	[MDP_TV_CLK] = &mdp_tv_clk.clkr,
	[PLL2] = &pll2.clkr,
	[PLL3] = &pll3.clkr,
};

static const struct qcom_reset_map mmcc_msm8660_resets[] = {
	/* AXI clocks */
	[IJPEG_AXI_RESET] = { 0x0208, 14 },
	[IMEM_AXI_RESET] = { 0x0210, 10 },
	[MDP_AXI_RESET] = { 0x0208, 13 },
	/* FIXME: VCODEC lists bits 4 AND 5 for reset! */
	[VCODEC_AXI_RESET] = { 0x0208, 4 },
	[VFE_AXI_RESET] = { 0x0208, 9 },
	[ROT_AXI_RESET] = { 0x0208, 6 },
	[VPE_AXI_RESET] = { 0x0208, 15 },
	/* AHB resets */
	[AMP_AHB_RESET] = { 0x0210, 20 },
	[CSI0_AHB_RESET] = { 0x020c, 17 },
	[CSI1_AHB_RESET] = { 0x020c, 16 },
	[DSI_M_AHB_RESET] = { 0x020c, 6 },
	[DSI_S_AHB_RESET] = { 0x020c, 5 },
	[GFX2D0_AHB_RESET] = { 0x020c, 12 },
	[GFX2D1_AHB_RESET] = { 0x020c, 11 },
	[GFX3D_AHB_RESET] = { 0x020c, 10 },
	/* Same bit resets both HDMI AHB clocks */
	[HDMI_M_AHB_RESET] = { 0x020c, 9 },
	[HDMI_S_AHB_RESET] = { 0x020c, 9 },
	[IJPEG_AHB_RESET] = { 0x020c, 7 },
	[IMEM_AHB_RESET] = { 0x020c, 8 },
	[JPEGD_AHB_RESET] = { 0x020c, 4 },
	[MDP_AHB_RESET] = { 0x020c, 3 },
	[ROT_AHB_RESET] = { 0x020c, 2 },
	/* No reset for SMMU */
	[TV_ENC_AHB_RESET] = { 0x020c, 15 },
	[VCODEC_AHB_RESET] = { 0x020c, 1 },
	[VFE_AHB_RESET] = { 0x020c, 0 },
	[VPE_AHB_RESET] = { 0x020c, 14 },
	/* SW Core reset blocks */
	[MDP_RESET] = { 0x0210, 21 },
	[MDP_VSYNC_RESET] = { 0x210, 3 },
	[MDP_PIXEL_RESET] = { 0x210, 5 },
	[ROT_RESET] = { 0x0210, 2 },
	[GFX3D_RESET] = { 0x0210, 12 },
	[CSI0_RESET] = { 0x0210, 8 },
	[CSI1_RESET] = { 0x0210, 18 },
	[DSI_BYTE_RESET] = { 0x0210, 7 },
	[TV_ENC_RESET] = { 0x0210, 0 },
	[MDP_TV_RESET] = { 0x0210, 4 },
	[HDMI_TV_RESET] = { 0x0210, 1 },
	[HDMI_APP_RESET] = { 0x0210, 11 },
	[VCODEC_RESET] = { 0x0210, 6 },
	[VPE_RESET] = { 0x210, 17 },
	[VFE_RESET] = { 0x210, 15 },
	[CSI0_VFE_RESET] = { 0x210, 12 },
	[CSI1_VFE_RESET] = { 0x210, 23 },
};

static const struct regmap_config mmcc_msm8660_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0x350, /* MM_PLL2_STATUS_REG */
	.fast_io	= true,
};

static const struct qcom_cc_desc mmcc_msm8660_desc = {
	.config = &mmcc_msm8660_regmap_config,
	.clks = mmcc_msm8660_clks,
	.num_clks = ARRAY_SIZE(mmcc_msm8660_clks),
	.resets = mmcc_msm8660_resets,
	.num_resets = ARRAY_SIZE(mmcc_msm8660_resets),
};

static const struct of_device_id mmcc_msm8660_match_table[] = {
	{ .compatible = "qcom,mmcc-msm8660", .data = &mmcc_msm8660_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, mmcc_msm8660_match_table);

#define AHB_EN_REG			0x0008
#define MAXI_EN_REG			0x0018
#define MAXI_EN2_REG			0x0020
#define MAXI_EN3_REG			0x002c
#define SAXI_EN_REG			0x0030
#define AHB_EN2_REG			0x0038
#define CSI_CC_REG			0x0040
#define MISC_CC_REG			0x0058
#define MISC_CC2_REG			0x005c
#define GFX2D0_CC_REG			0x0060
#define GFX2D1_CC_REG			0x0074
#define GFX3D_CC_REG			0x0080
#define IJPEG_CC_REG			0x0098
#define JPEGD_CC_REG			0x00A4
#define MDP_CC_REG			0x00C0
#define PIXEL_CC_REG			0x00D4
#define ROT_CC_REG			0x00E0
#define TV_CC_REG			0x00EC
#define VCODEC_CC_REG			0x00F8
#define VFE_CC_REG			0x0104
#define VPE_CC_REG			0x0110
#define PIXEL_CC2_REG			0x0120
#define TV_CC2_REG			0x0124
#define SW_RESET_ALL_REG		0x0204
#define SW_RESET_AXI_REG		0x0208
#define SW_RESET_AHB_REG		0x020C
#define SW_RESET_CORE_REG		0x0210
#define MM_PLL2_MODE_REG		0x0338
#define MM_PLL2_CONFIG_REG		0x0348

static int mmcc_msm8660_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct regmap *regmap;
	struct device *dev = &pdev->dev;
	int ret;

	match = of_match_device(mmcc_msm8660_match_table, dev);
	if (!match)
		return -EINVAL;

	regmap = qcom_cc_map(pdev, match->data);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/*
	 * FIXME: the following is cargo-cult code, check if it is
	 * really needed anymore.
	 */

	/* Set-up PLL3 but turn it off, first disable output */
	regmap_update_bits(regmap, MM_PLL2_MODE_REG, 0x01, 0x00);
	/* Set ref, bypass, assert reset, disable output, disable test mode */
	regmap_write(regmap,  MM_PLL2_MODE_REG, 0);
	/* Enable main output */
	regmap_write(regmap, MM_PLL2_CONFIG_REG, BIT(23));

	/* Deassert the reset to all MM clocks */
	regmap_write(regmap, SW_RESET_ALL_REG, 0);

	/*
	 * Initialize MM AHB registers: Enable the FPB clock and disable HW
	 * gating for all clocks. Also set VFE_AHB's FORCE_CORE_ON bit to
	 * prevent its memory from being collapsed when the clock is halted.
	 * The sleep and wake-up delays are set to safe values.
	 */
	regmap_update_bits(regmap, AHB_EN_REG, 0x6C000003, 0x03);
	regmap_write(regmap, AHB_EN2_REG, 0x000007f9);

	/* Deassert all locally-owned MM AHB resets. */
	regmap_update_bits(regmap, SW_RESET_AHB_REG, 0xFFF7DFFF, 0);

	/*
	 * Initialize MM AXI registers: Enable HW gating for all clocks that
	 * support it. Also set FORCE_CORE_ON bits, and any sleep and wake-up
	 * delays to safe values.
	 */
	regmap_update_bits(regmap, MAXI_EN_REG, 0x1803FFFF, 0x100207F9);
	regmap_update_bits(regmap, MAXI_EN2_REG, 0x7A3FFFFF, 0x7027FCFF);
	regmap_write(regmap, MAXI_EN3_REG, 0x3FE7FCFF);
	regmap_write(regmap, SAXI_EN_REG, 0x000001D8);

	/*
	 * Initialize MM CC registers: Set MM FORCE_CORE_ON bits so that core
	 * memories retain state even when not clocked. Also, set sleep and
	 * wake-up delays to safe values.
	 */
	regmap_update_bits(regmap, CSI_CC_REG, 0x00000018, 0);
	regmap_update_bits(regmap, MISC_CC_REG, 0x017C0400, 0x00000400);
	regmap_update_bits(regmap, MISC_CC2_REG, 0x70C2E7FF, 0x000007FD);

	regmap_update_bits(regmap, GFX2D0_CC_REG, 0xE0FF0010, 0x80FF0000);
	regmap_update_bits(regmap, GFX2D1_CC_REG, 0xE0FF0010, 0x80FF0000);
	regmap_update_bits(regmap, GFX3D_CC_REG, 0xE0FF0010, 0x80FF0000);
	regmap_update_bits(regmap, IJPEG_CC_REG, 0xE0FF0018, 0x80FF0000);
	regmap_update_bits(regmap, JPEGD_CC_REG, 0xE0FF0018, 0x80FF0000);
	regmap_update_bits(regmap, MDP_CC_REG, 0xE1FF0010, 0x80FF0000);
	regmap_update_bits(regmap, PIXEL_CC_REG, 0xE1FF0010, 0x80FF0000);
	regmap_update_bits(regmap, PIXEL_CC2_REG, 0x000007FF, 0x000004FF);
	regmap_update_bits(regmap, ROT_CC_REG, 0xE0FF0010, 0x80FF0000);
	regmap_update_bits(regmap, TV_CC_REG, 0xE1FFC010, 0x80FF0000);
	regmap_update_bits(regmap, TV_CC2_REG, 0x000027FF, 0x000004FF);
	regmap_update_bits(regmap, VCODEC_CC_REG, 0xE0FF0010, 0xC0FF0000);
	regmap_update_bits(regmap, VFE_CC_REG, 0xE0FFC010, 0x80FF0000);
	regmap_update_bits(regmap, VPE_CC_REG, 0xE0FF0010, 0x80FF0000);

	/* De-assert MM AXI resets to all hardware blocks. */
	regmap_write(regmap, SW_RESET_AXI_REG, 0);

	/* Deassert all MM core resets. */
	regmap_write(regmap, SW_RESET_CORE_REG, 0);

	/*
	 * Set the dsi_byte_clk src to the DSI PHY PLL,
	 * dsi_esc_clk to PXO/2, and the hdmi_app_clk src to PXO
	 */
	regmap_update_bits(regmap, MISC_CC2_REG, 0x424003, 0x400001);

	/* PLL3 is also known as MM_PLL2, corresponds to MSM8960 PLL15 */
	clk_pll_configure_sr(&pll3, regmap, &pll3_config, false);

	ret = qcom_cc_really_probe(pdev, match->data, regmap);
	if (ret)
		return ret;

	clk_pll_ops.enable(&pll2.clkr.hw);
	return 0;
}

static struct platform_driver mmcc_msm8660_driver = {
	.probe		= mmcc_msm8660_probe,
	.driver		= {
		.name	= "mmcc-msm8660",
		.of_match_table = mmcc_msm8660_match_table,
	},
};

module_platform_driver(mmcc_msm8660_driver);

MODULE_DESCRIPTION("QCOM MMCC MSM8660 Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:mmcc-msm8660");
