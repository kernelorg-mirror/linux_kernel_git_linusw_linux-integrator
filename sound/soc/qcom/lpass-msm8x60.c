/*
 * lpass-msm8x60.c - ALSA SoC CPU DAI driver for MSM8X60 Low-power Audio
 * Subsystem (LPASS)
 *
 * Copyright (C) 2017 Linus Walleij <linus.walleij@linaro.org>
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include <dt-bindings/sound/msm8x60-lpass.h>
#include "lpass-lpaif-reg.h"
#include "lpass.h"

static struct snd_soc_dai_driver msm8x60_lpass_cpu_dai_driver[] = {
	[MI2S] =  {
		.id = MI2S,
		.name = "Mono I2S",
		.playback = {
			.stream_name	= "Mono I2S Playback",
			.formats	= SNDRV_PCM_FMTBIT_S16 |
						SNDRV_PCM_FMTBIT_S24 |
						SNDRV_PCM_FMTBIT_S32,
			.rates		= SNDRV_PCM_RATE_8000 |
						SNDRV_PCM_RATE_16000 |
						SNDRV_PCM_RATE_32000 |
						SNDRV_PCM_RATE_48000 |
						SNDRV_PCM_RATE_96000,
			.rate_min	= 8000,
			.rate_max	= 96000,
			.channels_min	= 1,
			.channels_max	= 8,
		},
		.capture = {
			.stream_name	= "Mono I2S Capture",
			.formats	= SNDRV_PCM_FMTBIT_S16 |
						SNDRV_PCM_FMTBIT_S24 |
						SNDRV_PCM_FMTBIT_S32,
			.rates		= SNDRV_PCM_RATE_8000 |
						SNDRV_PCM_RATE_16000 |
						SNDRV_PCM_RATE_32000 |
						SNDRV_PCM_RATE_48000 |
						SNDRV_PCM_RATE_96000,
			.rate_min	= 8000,
			.rate_max	= 96000,
			.channels_min	= 1,
			.channels_max	= 8,
		},
		.probe	= &asoc_qcom_lpass_cpu_dai_probe,
		.ops    = &asoc_qcom_lpass_cpu_dai_ops,
	},
	[CODEC_I2S_MIC] =  {
		.id = CODEC_I2S_MIC,
		.name = "Codec I2S Microphone",
		.capture = {
			.stream_name	= "Codec I2S Microphone",
			.formats	= SNDRV_PCM_FMTBIT_S16 |
						SNDRV_PCM_FMTBIT_S24 |
						SNDRV_PCM_FMTBIT_S32,
			.rates		= SNDRV_PCM_RATE_8000 |
						SNDRV_PCM_RATE_16000 |
						SNDRV_PCM_RATE_32000 |
						SNDRV_PCM_RATE_48000 |
						SNDRV_PCM_RATE_96000,
			.rate_min	= 8000,
			.rate_max	= 96000,
			.channels_min	= 1,
			.channels_max	= 8,
		},
		.probe	= &asoc_qcom_lpass_cpu_dai_probe,
		.ops    = &asoc_qcom_lpass_cpu_dai_ops,
	},
	[SPARE_I2S_MIC] =  {
		.id = SPARE_I2S_MIC,
		.name = "Spare I2S Microphone",
		.capture = {
			.stream_name	= "Spare I2S Microphone",
			.formats	= SNDRV_PCM_FMTBIT_S16 |
						SNDRV_PCM_FMTBIT_S24 |
						SNDRV_PCM_FMTBIT_S32,
			.rates		= SNDRV_PCM_RATE_8000 |
						SNDRV_PCM_RATE_16000 |
						SNDRV_PCM_RATE_32000 |
						SNDRV_PCM_RATE_48000 |
						SNDRV_PCM_RATE_96000,
			.rate_min	= 8000,
			.rate_max	= 96000,
			.channels_min	= 1,
			.channels_max	= 8,
		},
		.probe	= &asoc_qcom_lpass_cpu_dai_probe,
		.ops    = &asoc_qcom_lpass_cpu_dai_ops,
	},
	[CODEC_I2S_SPKR] =  {
		.id = CODEC_I2S_SPKR,
		.name = "Codec I2S Speaker",
		.playback = {
			/* FIXME: removed some formats and rates */
			.stream_name	= "Codec I2S Speaker",
			.formats	= SNDRV_PCM_FMTBIT_S16_LE,
			.rates		= SNDRV_PCM_RATE_8000 |
			SNDRV_PCM_RATE_16000 |
			SNDRV_PCM_RATE_32000 |
			SNDRV_PCM_RATE_48000,
			.rate_min	= 8000,
			.rate_max	= 48000,
			.channels_min	= 1,
			.channels_max	= 2,
		},
		.probe	= &asoc_qcom_lpass_cpu_dai_probe,
		.ops    = &asoc_qcom_lpass_cpu_dai_ops,
	},
	[SPARE_I2S_SPKR] =  {
		.id = SPARE_I2S_SPKR,
		.name = "Spare I2S Speaker",
		.playback = {
			.stream_name	= "Spare I2S Speaker",
			.formats	= SNDRV_PCM_FMTBIT_S16 |
						SNDRV_PCM_FMTBIT_S24 |
						SNDRV_PCM_FMTBIT_S32,
			.rates		= SNDRV_PCM_RATE_8000 |
						SNDRV_PCM_RATE_16000 |
						SNDRV_PCM_RATE_32000 |
						SNDRV_PCM_RATE_48000 |
						SNDRV_PCM_RATE_96000,
			.rate_min	= 8000,
			.rate_max	= 96000,
			.channels_min	= 1,
			.channels_max	= 8,
		},
		.probe	= &asoc_qcom_lpass_cpu_dai_probe,
		.ops    = &asoc_qcom_lpass_cpu_dai_ops,
	},
};

static int msm8x60_lpass_alloc_dma_channel(struct lpass_data *drvdata,
					   int direction)
{
	struct lpass_variant *v = drvdata->variant;
	int chan = 0;

	if (direction == SNDRV_PCM_STREAM_PLAYBACK) {
		chan = find_first_zero_bit(&drvdata->dma_ch_bit_map,
					v->rdma_channels);

		if (chan >= v->rdma_channels)
			return -EBUSY;
	} else {
		chan = find_next_zero_bit(&drvdata->dma_ch_bit_map,
					v->wrdma_channel_start +
					v->wrdma_channels,
					v->wrdma_channel_start);

		if (chan >=  v->wrdma_channel_start + v->wrdma_channels)
			return -EBUSY;
	}

	set_bit(chan, &drvdata->dma_ch_bit_map);

	pr_info("Allocated LPASS DMA channel %d\n", chan);
	return chan;
}

static int msm8x60_lpass_free_dma_channel(struct lpass_data *drvdata, int chan)
{
	clear_bit(chan, &drvdata->dma_ch_bit_map);
	pr_info("Free:ed LPASS DMA channel %d\n", chan);
	return 0;
}

static int msm8x60_lpass_init(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "initialized MSM8x60 LPASS\n");

	return 0;
}

static int msm8x60_lpass_exit(struct platform_device *pdev)
{
	return 0;
}

static struct lpass_variant msm8x60_data = {
	.i2sctrl_reg_base	= 0x0004, /* IPQ8064 has 0x0010, Hint at 0x0004 */
	.i2sctrl_reg_stride	= 0x04,
	.i2s_ports		= 5,
	.irq_reg_base		= 0x3000,
	.irq_reg_stride		= 0x1000,
	.irq_ports		= 3,
	.rdma_reg_base		= 0x6000,
	.rdma_reg_stride	= 0x1000,
	.rdma_channels		= 5,
	/* .dmactl_audif_start	= 1, */ /* No idea */
	.wrdma_reg_base		= 0xB000, /* 0x6000 + 5 * 0x1000 */
	.wrdma_reg_stride	= 0x1000,
	.wrdma_channel_start	= 5,
	.wrdma_channels		= 5,
	.dai_driver		= msm8x60_lpass_cpu_dai_driver,
	.num_dai		= ARRAY_SIZE(msm8x60_lpass_cpu_dai_driver),
	.dai_osr_clk_names	= (const char *[]) {
				"mi2s-osr-clk",
				"codec-i2s-mic-osr-clk",
				"spare-i2s-mic-osr-clk",
				"codec-i2s-spkr-osr-clk",
				"spare-i2s-spkr-osr-clk",
				},
	.dai_bit_clk_names	= (const char *[]) {
				"mi2s-bit-clk",
				"codec-i2s-mic-bit-clk",
				"spare-i2s-mic-bit-clk",
				"codec-i2s-spkr-bit-clk",
				"spare-i2s-spkr-bit-clk",
				},
	.init			= msm8x60_lpass_init,
	.exit			= msm8x60_lpass_exit,
	.alloc_dma_channel	= msm8x60_lpass_alloc_dma_channel,
	.free_dma_channel	= msm8x60_lpass_free_dma_channel,
};

static const struct of_device_id msm8x60_lpass_cpu_device_id[] = {
	{ .compatible = "qcom,lpass-cpu-msm8660", .data = &msm8x60_data },
	{}
};
MODULE_DEVICE_TABLE(of, msm8x60_lpass_cpu_device_id);

static struct platform_driver msm8x60_lpass_cpu_platform_driver = {
	.driver	= {
		.name		= "msm8x60-lpass-cpu",
		.of_match_table	= of_match_ptr(msm8x60_lpass_cpu_device_id),
	},
	.probe	= asoc_qcom_lpass_cpu_platform_probe,
	.remove	= asoc_qcom_lpass_cpu_platform_remove,
};
module_platform_driver(msm8x60_lpass_cpu_platform_driver);

MODULE_DESCRIPTION("MSM8X60 LPASS CPU DAI Driver");
MODULE_LICENSE("GPL v2");
