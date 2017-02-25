/*
 * Qualcomm MSM8660/APQ8060 Low-power Audio Subsystem (LPASS) Clock Controller
 * Copyright (c) 2017 Linus Walleij <linus.walleij@linaro.org>
 *
 * Based on a copy of the IPQ806x driver
 * (C) 2014 Rajendra Nayak
 * and portions of the MDM9615 driver
 * (C) 2014 Neil Armstrong.
 */

#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/clk-provider.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,lcc-msm8660.h>

#include "common.h"
#include "clk-regmap.h"
#include "clk-pll.h"
#include "clk-rcg.h"
#include "clk-branch.h"
#include "clk-regmap-divider.h"
#include "clk-regmap-mux.h"

/*
 * The vendor tree calls this "PLL0" but we are going to refer to it as
 * LPA_PLL0 so as not to confuse it with the PLL0 on the GCC.
 */
static struct clk_pll lpa_pll0 = {
	.l_reg = 0x4,
	.m_reg = 0x8,
	.n_reg = 0xc,
	.config_reg = 0x14,
	.mode_reg = 0x0,
	.status_reg = 0x18,
	.status_bit = 16,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "lpa_pll0",
		.parent_names = (const char *[]){ "pxo" },
		.num_parents = 1,
		.ops = &clk_pll_ops,
	},
};

/*
 * The l/m/n values were read out of the registers after a cold boot.
 * The config register read 00c22080 and the mode register 0x00000007.
 */
static const struct pll_config lpa_pll0_config = {
	.l = 0xf,
	.m = 0x1c,
	.n = 0x465,
	.vco_val = BIT(17),
	.vco_mask = BIT(17) | BIT(16),
	.pre_div_val = 0x0,
	.pre_div_mask = BIT(19),
	.post_div_val = 0x0,
	.post_div_mask = BIT(21) | BIT(20),
	.mn_ena_mask = BIT(22),
	.main_output_mask = BIT(23),
};

enum {
	P_PXO,
	P_CXO,
	P_PLL4_LPA_PLL0,
	P_GND,
};

/* The vendor code uses PLL4 as parent everywhere */
static const struct parent_map lcc_parent_map[] = {
	{ P_PXO, 0 },
	{ P_CXO, 1 },
	/* Select RPM PLL4, but also used for selecting LPA PLL0 */
	{ P_PLL4_LPA_PLL0, 2 },
	/* Will just ground the line */
	{ P_GND, 6 },
};

static const char * const lcc_parent_tbl[] = {
	"pxo",
	"cxo",
	/*
	 * PLL4 is an RPM clock on MSM8660/APQ8060, set to "pll4" for this
	 * If we enable and mux in the LPA_PLL0 on this platform, we can
	 * set this to "lpa_pll0" instead
	 */
	"pll4_clk",
	"gnd", /* This is a very inactive parent */
};

/*
 * This table is evidently for using PLL4 as parent, if we start using
 * LPA_PLL0 we need to provide a second table.
 */
static struct freq_tbl clk_tbl_aif_osr_pll4[] = {
	{   768000, P_PLL4_LPA_PLL0, 4,  1, 176 },
	{  1024000, P_PLL4_LPA_PLL0, 4,  1, 132 },
	{  1536000, P_PLL4_LPA_PLL0, 4,  1,  88 },
	{  2048000, P_PLL4_LPA_PLL0, 4,  1,  66 },
	{  3072000, P_PLL4_LPA_PLL0, 4,  1,  44 },
	{  4096000, P_PLL4_LPA_PLL0, 4,  1,  33 },
	{  6144000, P_PLL4_LPA_PLL0, 4,  1,  22 },
	{  8192000, P_PLL4_LPA_PLL0, 2,  1,  33 },
	{ 12288000, P_PLL4_LPA_PLL0, 4,  1,  11 },
	{ 24576000, P_PLL4_LPA_PLL0, 2,  1,  11 },
	{ 27000000, P_PXO,           1,  0,   0 },
	{ }
};

/*
 * This macro is modified from lcc-mdm9516.c, it's used for all the
 * AIF clocks, all of them have an OSR clock and a bit clock derived
 * from the OSR clock.
 *
 * These clocks differ from many other platforms by using
 * BRANCH_HALT_DELAY for the *_bit_div_clk
 */
#define CLK_AIF_OSR_DIV(prefix, _ns, _md, _hr)			\
static struct clk_rcg prefix##_osr_src = {			\
	.ns_reg = _ns,						\
	.md_reg = _md,						\
	.mn = {							\
		.mnctr_en_bit = 8,				\
		.mnctr_reset_bit = 7,				\
		.mnctr_mode_shift = 5,				\
		.n_val_shift = 24,				\
		.m_val_shift = 8,				\
		.width = 8,					\
	},							\
	.p = {							\
		.pre_div_shift = 3,				\
		.pre_div_width = 2,				\
	},							\
	.s = {							\
		.src_sel_shift = 0,				\
		.parent_map = lcc_parent_map,			\
	},							\
	.freq_tbl = clk_tbl_aif_osr_pll4,			\
	.clkr = {						\
		.enable_reg = _ns,				\
		.enable_mask = BIT(9),				\
		.hw.init = &(struct clk_init_data){		\
			.name = #prefix "_osr_src",		\
			.parent_names = lcc_parent_tbl,		\
			.num_parents = 4,			\
			.ops = &clk_rcg_ops,			\
			.flags = CLK_SET_RATE_GATE,		\
		},						\
	},							\
};								\
								\
static const char * const lcc_##prefix##_parents[] = {		\
	#prefix "_osr_src",					\
};								\
								\
static struct clk_branch prefix##_osr_clk = {			\
	.halt_reg = _hr,					\
	.halt_bit = 1,						\
	.halt_check = BRANCH_HALT_ENABLE,			\
	.clkr = {						\
		.enable_reg = _ns,				\
		.enable_mask = BIT(17),				\
		.hw.init = &(struct clk_init_data){		\
			.name = #prefix "_osr_clk",		\
			.parent_names = lcc_##prefix##_parents,	\
			.num_parents = 1,			\
			.ops = &clk_branch_ops,			\
			.flags = CLK_SET_RATE_PARENT,		\
		},						\
	},							\
};								\
								\
static struct clk_regmap_div prefix##_div_clk = {		\
	.reg = _ns,						\
	.shift = 10,						\
	.width = 4,						\
	.clkr = {						\
		.hw.init = &(struct clk_init_data){		\
			.name = #prefix "_div_clk",		\
			.parent_names = lcc_##prefix##_parents,	\
			.num_parents = 1,			\
			.ops = &clk_regmap_div_ops,		\
		},						\
	},							\
};								\
								\
static struct clk_branch prefix##_bit_div_clk = {		\
	.halt_reg = _hr,					\
	.halt_bit = 0,						\
	.halt_check = BRANCH_HALT_DELAY,			\
	.clkr = {						\
		.enable_reg = _ns,				\
		.enable_mask = BIT(15),				\
		.hw.init = &(struct clk_init_data){		\
			.name = #prefix "_bit_div_clk",		\
			.parent_names = (const char *[]){	\
				#prefix "_div_clk"		\
			},					\
			.num_parents = 1,			\
			.ops = &clk_branch_ops,			\
			.flags = CLK_SET_RATE_PARENT,		\
		},						\
	},							\
};								\
								\
static struct clk_regmap_mux prefix##_bit_clk = {		\
	.reg = _ns,						\
	.shift = 14,						\
	.width = 1,						\
	.clkr = {						\
		.hw.init = &(struct clk_init_data){		\
			.name = #prefix "_bit_clk",		\
			.parent_names = (const char *[]){	\
				#prefix "_bit_div_clk",		\
				#prefix "_codec_clk",		\
			},					\
			.num_parents = 2,			\
			.ops = &clk_regmap_mux_closest_ops,	\
			.flags = CLK_SET_RATE_PARENT,		\
		},						\
	},							\
}

CLK_AIF_OSR_DIV(mi2s, 0x48, 0x4c, 0x50);
CLK_AIF_OSR_DIV(codec_i2s_mic, 0x60, 0x64, 0x68);
CLK_AIF_OSR_DIV(spare_i2s_mic, 0x78, 0x7c, 0x80);
CLK_AIF_OSR_DIV(codec_i2s_spkr, 0x6c, 0x70, 0x74);
CLK_AIF_OSR_DIV(spare_i2s_spkr, 0x84, 0x88, 0x8c);

/*
 * PCM clock
 * This table is evidently for using PLL4 as parent, if we start using
 * LPA_PLL0 we need to provide a second table.
 */
static struct freq_tbl clk_tbl_pcm_pll4[] = {
	{   512000, P_PLL4_LPA_PLL0, 4, 1, 264 },
	{   768000, P_PLL4_LPA_PLL0, 4, 1, 176 },
	{  1024000, P_PLL4_LPA_PLL0, 4, 1, 132 },
	{  1536000, P_PLL4_LPA_PLL0, 4, 1,  88 },
	{  2048000, P_PLL4_LPA_PLL0, 4, 1,  66 },
	{  3072000, P_PLL4_LPA_PLL0, 4, 1,  44 },
	{  4096000, P_PLL4_LPA_PLL0, 4, 1,  33 },
	{  6144000, P_PLL4_LPA_PLL0, 4, 1,  22 },
	{  8192000, P_PLL4_LPA_PLL0, 2, 1,  33 },
	{ 12288000, P_PLL4_LPA_PLL0, 4, 1,  11 },
	{ 24580000, P_PLL4_LPA_PLL0, 2, 1,  11 },
	{ 27000000, P_PXO,           1, 0,   0 },
	{ },
};

static struct clk_rcg pcm_src = {
	.ns_reg = 0x54,
	.md_reg = 0x58,
	.mn = {
		.mnctr_en_bit = 8,
		.mnctr_reset_bit = 7,
		.mnctr_mode_shift = 5,
		.n_val_shift = 16,
		.m_val_shift = 16,
		.width = 16,
	},
	.p = {
		.pre_div_shift = 3,
		.pre_div_width = 2,
	},
	.s = {
		.src_sel_shift = 0,
		.parent_map = lcc_parent_map,
	},
	.freq_tbl = clk_tbl_pcm_pll4,
	.clkr = {
		.enable_reg = 0x54,
		.enable_mask = BIT(9),
		.hw.init = &(struct clk_init_data){
			.name = "pcm_src",
			.parent_names = lcc_parent_tbl,
			.num_parents = 4,
			.ops = &clk_rcg_ops,
			.flags = CLK_SET_RATE_GATE,
		},
	},
};

static struct clk_branch pcm_clk_out = {
	.halt_reg = 0x5c,
	.halt_bit = 0,
	.halt_check = BRANCH_HALT_ENABLE,
	.clkr = {
		.enable_reg = 0x54,
		.enable_mask = BIT(11),
		.hw.init = &(struct clk_init_data){
			.name = "pcm_clk_out",
			.parent_names = (const char *[]){ "pcm_src" },
			.num_parents = 1,
			.ops = &clk_branch_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_regmap_mux pcm_clk = {
	.reg = 0x54,
	.shift = 10,
	.width = 1,
	.clkr = {
		.hw.init = &(struct clk_init_data){
			.name = "pcm_clk",
			.parent_names = (const char *[]){
				"pcm_clk_out",
				"pcm_codec_clk",
			},
			.num_parents = 2,
			.ops = &clk_regmap_mux_closest_ops,
			.flags = CLK_SET_RATE_PARENT,
		},
	},
};

static struct clk_regmap *lcc_msm8660_clks[] = {
	[LPA_PLL0] = &lpa_pll0.clkr,
	[MI2S_OSR_SRC] = &mi2s_osr_src.clkr,
	[MI2S_OSR_CLK] = &mi2s_osr_clk.clkr,
	[MI2S_DIV_CLK] = &mi2s_div_clk.clkr,
	[MI2S_BIT_DIV_CLK] = &mi2s_bit_div_clk.clkr,
	[MI2S_BIT_CLK] = &mi2s_bit_clk.clkr,
	[CODEC_I2S_MIC_OSR_SRC] = &codec_i2s_mic_osr_src.clkr,
	[CODEC_I2S_MIC_OSR_CLK] = &codec_i2s_mic_osr_clk.clkr,
	[CODEC_I2S_MIC_DIV_CLK] = &codec_i2s_mic_div_clk.clkr,
	[CODEC_I2S_MIC_BIT_DIV_CLK] = &codec_i2s_mic_bit_div_clk.clkr,
	[CODEC_I2S_MIC_BIT_CLK] = &codec_i2s_mic_bit_clk.clkr,
	[SPARE_I2S_MIC_OSR_SRC] = &spare_i2s_mic_osr_src.clkr,
	[SPARE_I2S_MIC_OSR_CLK] = &spare_i2s_mic_osr_clk.clkr,
	[SPARE_I2S_MIC_DIV_CLK] = &spare_i2s_mic_div_clk.clkr,
	[SPARE_I2S_MIC_BIT_DIV_CLK] = &spare_i2s_mic_bit_div_clk.clkr,
	[SPARE_I2S_MIC_BIT_CLK] = &spare_i2s_mic_bit_clk.clkr,
	[CODEC_I2S_SPKR_OSR_SRC] = &codec_i2s_spkr_osr_src.clkr,
	[CODEC_I2S_SPKR_OSR_CLK] = &codec_i2s_spkr_osr_clk.clkr,
	[CODEC_I2S_SPKR_DIV_CLK] = &codec_i2s_spkr_div_clk.clkr,
	[CODEC_I2S_SPKR_BIT_DIV_CLK] = &codec_i2s_spkr_bit_div_clk.clkr,
	[CODEC_I2S_SPKR_BIT_CLK] = &codec_i2s_spkr_bit_clk.clkr,
	[SPARE_I2S_SPKR_OSR_SRC] = &spare_i2s_spkr_osr_src.clkr,
	[SPARE_I2S_SPKR_OSR_CLK] = &spare_i2s_spkr_osr_clk.clkr,
	[SPARE_I2S_SPKR_DIV_CLK] = &spare_i2s_spkr_div_clk.clkr,
	[SPARE_I2S_SPKR_BIT_DIV_CLK] = &spare_i2s_spkr_bit_div_clk.clkr,
	[SPARE_I2S_SPKR_BIT_CLK] = &spare_i2s_spkr_bit_clk.clkr,
	[PCM_SRC] = &pcm_src.clkr,
	[PCM_CLK_OUT] = &pcm_clk_out.clkr,
	[PCM_CLK] = &pcm_clk.clkr,
};

static const struct regmap_config lcc_msm8660_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0xfc,
	.fast_io	= true,
};

static const struct qcom_cc_desc lcc_msm8660_desc = {
	.config = &lcc_msm8660_regmap_config,
	.clks = lcc_msm8660_clks,
	.num_clks = ARRAY_SIZE(lcc_msm8660_clks),
};

static const struct of_device_id lcc_msm8660_match_table[] = {
	{ .compatible = "qcom,lcc-msm8660" },
	{ }
};
MODULE_DEVICE_TABLE(of, lcc_msm8660_match_table);

static int lcc_msm8660_probe(struct platform_device *pdev)
{
	u32 val;
	struct regmap *regmap;

	regmap = qcom_cc_map(pdev, &lcc_msm8660_desc);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/* Configure the rate of LPA_PLL0 if the bootloader hasn't already */
	regmap_read(regmap, 0x0, &val);
	if (!val) {
		dev_info(&pdev->dev, "configuring LPA_PLL0\n");
		clk_pll_configure_sr(&lpa_pll0, regmap, &lpa_pll0_config, true);
	} else {
		dev_info(&pdev->dev,
			 "LPA_PLL0 already configured\n");
	}

	/*
	 * Enable LPA_PLL0 source on the LPASS Primary PLL Mux. Incidentally
	 * this is set to 0x00000001 at boot.
	 * 0x01 = LPA_PLL0
	 */
	regmap_write(regmap, 0xc4, 0x1);

	return qcom_cc_really_probe(pdev, &lcc_msm8660_desc, regmap);
}

static struct platform_driver lcc_msm8660_driver = {
	.probe		= lcc_msm8660_probe,
	.driver		= {
		.name	= "lcc-msm8660",
		.of_match_table = lcc_msm8660_match_table,
	},
};
module_platform_driver(lcc_msm8660_driver);

MODULE_DESCRIPTION("QCOM LCC MSM8660 Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:lcc-msm8660");
