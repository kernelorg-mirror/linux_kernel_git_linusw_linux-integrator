/*
 * Copyright (c) 2015 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <dt-bindings/sound/msm8x60-lpass.h>

/**
 * struct msm8660_wm8903_data - state container for the sound card
 * @spkr_pa_left: the speaker PA enable GPIO for the left channel
 * @spkr_pa_right: the speaker PA enable GPIO for the right channel
 * @mclk_gate: a GPIO gating the MCLK
 * @wm8903_mclk: the CODEC I2S MIC OSR clk is connected to the WM8903
 * MCLK, so this must be running for any of the I2S channels to the
 * WM89803 to work
 * @wm8903_mclk_bit: the CODEC I2S MIC bit clockk is connected to
 * the WM8903 MCLK, so this must be running for any of the I2S
 * channels to the WM89803 to work
 * @dai_link: the digital audio interface link
 */
struct msm8660_wm8903_data {
	struct gpio_desc *spkr_pa_left;
	struct gpio_desc *spkr_pa_right;
	struct gpio_desc *mclk_gate;
	struct clk *wm8903_mclk;
	struct clk *wm8903_mclk_bit;
	struct clk *spkr_osr_clk;
	struct clk *spkr_bit_clk;
	struct snd_soc_dai_link dai_link[];	/* dynamically allocated */
};

#define MSM8660_SYSCLK_MULT 4

static int msm8660_wm8903_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct msm8660_wm8903_data *data = snd_soc_card_get_drvdata(card);
	//snd_pcm_format_t format = params_format(params);
	unsigned int rate = params_rate(params);
	unsigned int codec_freq;
	int bitwidth, ret;

	pr_info("%s\n", __func__);
	dev_info(card->dev, "rate: %d\n", rate);

	/* WM8903 runs at LRC*256 */
	codec_freq = rate * 256;
	dev_info(card->dev, "request codec clock rate %d\n", codec_freq);

	/* Tell the codec what clock it gets in */
	ret = snd_soc_dai_set_sysclk(rtd->codec_dai, 0,
				     codec_freq, SND_SOC_CLOCK_IN);
	if (ret) {
		dev_err(card->dev, "error setting codec clock to %u: %d\n",
			codec_freq, ret);
		return ret;
	}

	ret = clk_set_rate(data->wm8903_mclk, codec_freq);
	if (ret) {
		dev_err(card->dev, "could not set WM8903 MCLK\n");
		return ret;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		// snd_soc_dai_digital_mute(rtd->codec_dai, 0);
		/* Set codec as master driving LRC and BCLK */
		ret = snd_soc_dai_set_fmt(rtd->codec_dai,
					  SND_SOC_DAIFMT_CBM_CFM |
					  SND_SOC_DAIFMT_I2S);
		if (ret) {
			dev_err(card->dev,
				"could not set CODEC DAI sound format\n");
			return ret;
		}
		/* Set CPU DAI as slave for LRC and BCLK */
		ret = snd_soc_dai_set_fmt(rtd->cpu_dai,
					  SND_SOC_DAIFMT_CBS_CFS);
		if (ret) {
			dev_err(card->dev,
				"could not set CPU DAI sound format\n");
			return ret;
		}
	} else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		// clk_set_rate(mic_bit_clk, 0);
	}

	return 0;
}

static const struct snd_soc_ops msm8660_wm8903_ops = {
	.hw_params = msm8660_wm8903_hw_params,
};

static int msm8660_wm8903_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct msm8660_wm8903_data *data = snd_soc_card_get_drvdata(card);
	int rval = 0;
	int ret;

	switch (cpu_dai->id) {
	case MI2S:
		dev_info(card->dev, "DAI init MI2S\n");
		break;
	case CODEC_I2S_MIC:
		dev_info(card->dev, "DAI init CODEC I2S MIC\n");
		break;
	case SPARE_I2S_MIC:
		dev_info(card->dev, "DAI init SPARE I2S MIC (WM8903)\n");
		break;
	case CODEC_I2S_SPKR:
		dev_info(card->dev, "DAI init CODEC I2S SPKR (WM8903 master, CPU I2S slave)\n");
		/* Set codec as master driving LRC and BCLK */
		ret = snd_soc_dai_set_fmt(rtd->codec_dai,
					  SND_SOC_DAIFMT_CBM_CFM |
					  SND_SOC_DAIFMT_I2S);
		if (ret) {
			dev_err(card->dev,
				"could not set CODEC DAI sound format\n");
			return ret;
		}
		/* Set CPU DAI as slave for LRC and BCLK */
		ret = snd_soc_dai_set_fmt(rtd->cpu_dai,
					  SND_SOC_DAIFMT_CBS_CFS);
		if (ret) {
			dev_err(card->dev,
				"could not set CPU DAI sound format\n");
			return ret;
		}
		break;
	case SPARE_I2S_SPKR:
		dev_info(card->dev, "DAI init SPARE I2S SPKR\n");
		break;
	default:
		dev_err(card->dev, "unsupported cpu dai configuration\n");
		rval = -EINVAL;
		break;
	}

	return rval;
}

static struct msm8660_wm8903_data *
msm8660_wm8903_parse_of(struct snd_soc_card *card)
{
	struct device *dev = card->dev;
	struct snd_soc_dai_link *link;
	struct device_node *np, *codec, *cpu, *node  = dev->of_node;
	struct msm8660_wm8903_data *data;
	int ret, num_links;

	ret = snd_soc_of_parse_card_name(card, "qcom,model");
	if (ret) {
		dev_err(dev, "Error parsing card name: %d\n", ret);
		return ERR_PTR(ret);
	}

	/* DAPM routes */
	if (of_property_read_bool(node, "qcom,audio-routing")) {
		ret = snd_soc_of_parse_audio_routing(card,
					"qcom,audio-routing");
		if (ret)
			return ERR_PTR(ret);
	}

	/* Populate links */
	num_links = of_get_child_count(node);

	/* Allocate the private data and the DAI link array */
	data = devm_kzalloc(dev, sizeof(*data) + sizeof(*link) * num_links,
			    GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);

	card->dai_link	= &data->dai_link[0];
	card->num_links	= num_links;

	link = data->dai_link;

	for_each_child_of_node(node, np) {
		cpu = of_get_child_by_name(np, "cpu");
		codec = of_get_child_by_name(np, "codec");

		if (!cpu || !codec) {
			dev_err(dev, "Can't find cpu/codec DT node\n");
			return ERR_PTR(-EINVAL);
		}

		link->cpu_of_node = of_parse_phandle(cpu, "sound-dai", 0);
		if (!link->cpu_of_node) {
			dev_err(card->dev, "error getting cpu phandle\n");
			return ERR_PTR(-EINVAL);
		}

		ret = snd_soc_of_get_dai_name(cpu, &link->cpu_dai_name);
		if (ret) {
			dev_err(card->dev, "error getting cpu dai name\n");
			return ERR_PTR(ret);
		}

		ret = snd_soc_of_get_dai_link_codecs(dev, codec, link);
		if (ret < 0) {
			dev_err(card->dev, "error getting codec dai link\n");
			return ERR_PTR(ret);
		}

		link->platform_of_node = link->cpu_of_node;
		ret = of_property_read_string(np, "link-name", &link->name);
		if (ret) {
			dev_err(card->dev, "error getting codec DAI link name\n");
			return ERR_PTR(ret);
		}

		link->stream_name = link->name;
		link->init = msm8660_wm8903_dai_init;
		link->ops = &msm8660_wm8903_ops;
		dev_info(dev, "parsed link \"%s\"\n", link->name);
		link++;
	}

	/* Obtain speaker PA GPIOs */
	data->spkr_pa_left = devm_gpiod_get(dev, "qcom,spkr-pa-left",
					    GPIOD_OUT_LOW);
	if (IS_ERR(data->spkr_pa_left)) {
		dev_err(dev, "could not obtain left speaker PA GPIO\n");
		return ERR_CAST(data->spkr_pa_left);
	}
	data->spkr_pa_right = devm_gpiod_get(dev, "qcom,spkr-pa-right",
					     GPIOD_OUT_LOW);
	if (IS_ERR(data->spkr_pa_left)) {
		dev_err(dev, "could not obtain left speaker PA GPIO\n");
		return ERR_CAST(data->spkr_pa_left);
	}

	/*
	 * FIXME: what on earth is this? My schematic does not have this
	 * but the vendor tree drives this line high.
	 */
	data->mclk_gate = devm_gpiod_get(dev, "qcom,mclk-gate", GPIOD_OUT_LOW);
	if (IS_ERR(data->mclk_gate)) {
		dev_err(dev, "could not obtain MCLK gate GPIO\n");
		return ERR_CAST(data->mclk_gate);
	}
	gpiod_set_value_cansleep(data->mclk_gate, 1);

	/* Obtain CODEC I2S MIC clock */
	data->wm8903_mclk = devm_clk_get(dev, "codec-i2s-mic-osr-clk");
	if (IS_ERR(data->wm8903_mclk)) {
		dev_err(dev, "could not get CODEC I2S MIC clock\n");
		return ERR_CAST(data->wm8903_mclk);
	}
	clk_set_rate(data->wm8903_mclk, 48000 * 256);
	ret = clk_prepare_enable(data->wm8903_mclk);
	if (ret) {
		dev_err(dev, "could not enable CODEC I2S MIC clock\n");
		return ERR_PTR(ret);
	}

	/* Obtain CODEC I2S bit clock so we latch out onto MCLK */
	data->wm8903_mclk_bit = devm_clk_get(dev, "codec-i2s-mic-bit-clk");
	if (IS_ERR(data->wm8903_mclk_bit)) {
		dev_err(dev, "could not get CODEC I2S MIC BIT clock\n");
		return ERR_CAST(data->wm8903_mclk_bit);
	}
	ret = clk_prepare_enable(data->wm8903_mclk_bit);
	if (ret) {
		dev_err(dev, "could not enable CODEC I2S MIC BIT clock\n");
		return ERR_PTR(ret);
	}

	/* FIXME: let runtime PM disable the CODEC I2S MIC clock */
	data->spkr_osr_clk = devm_clk_get(dev, "codec-i2s-spkr-osr-clk");
	if (IS_ERR(data->spkr_osr_clk)) {
		dev_err(dev, "could not get CODEC I2S SPKR OSR clock\n");
		return ERR_CAST(data->spkr_osr_clk);
	}
	clk_set_rate(data->spkr_osr_clk, 48000*256);
	clk_set_rate(data->spkr_osr_clk, 0);
	ret = clk_prepare_enable(data->spkr_osr_clk);
	if (ret) {
		dev_err(dev, "could not enable CODEC I2S SPKR OSR clock\n");
		return ERR_PTR(ret);
	}

	data->spkr_bit_clk = devm_clk_get(dev, "codec-i2s-spkr-bit-clk");
	if (IS_ERR(data->spkr_bit_clk)) {
		dev_err(dev, "could not get CODEC I2S SPKR BIT clock\n");
		return ERR_CAST(data->spkr_bit_clk);
	}
	ret = clk_prepare_enable(data->spkr_bit_clk);
	if (ret) {
		dev_err(dev, "could not enable CODEC I2S SPKR BIT clock\n");
		return ERR_PTR(ret);
	}

	return data;
}

static int msm8660_spkramp_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct msm8660_wm8903_data *data = snd_soc_card_get_drvdata(card);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		pr_info("enable SPKR GPIOs\n");
		gpiod_set_value_cansleep(data->spkr_pa_left, 1);
		gpiod_set_value_cansleep(data->spkr_pa_right, 1);
	} else {
		pr_info("disable SPKR GPIOs\n");
		gpiod_set_value_cansleep(data->spkr_pa_left, 0);
		gpiod_set_value_cansleep(data->spkr_pa_right, 0);
	}
	return 0;
}

static const struct snd_soc_dapm_widget msm8660_wm8903_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Headphone", msm8660_spkramp_event),
	SND_SOC_DAPM_SPK("Handset", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Handset Mic", NULL),
};

static int msm8660_wm8903_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_card *card;
	struct msm8660_wm8903_data *data;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->dev = dev;
	card->dapm_widgets = msm8660_wm8903_dapm_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(msm8660_wm8903_dapm_widgets);
	data = msm8660_wm8903_parse_of(card);
	if (IS_ERR(data)) {
		dev_err(&pdev->dev, "Error resolving DAI links: %ld\n",
			PTR_ERR(data));
		return PTR_ERR(data);
	}

	platform_set_drvdata(pdev, data);
	snd_soc_card_set_drvdata(card, data);

	return devm_snd_soc_register_card(&pdev->dev, card);
}

static const struct of_device_id msm8660_wm8903_device_id[]  = {
	{ .compatible = "qcom,msm8660-wm8903-sndcard" },
	{},
};
MODULE_DEVICE_TABLE(of, msm8660_wm8903_device_id);

static struct platform_driver msm8660_wm8903_platform_driver = {
	.driver = {
		.name = "qcom-msm8660-wm8903",
		.of_match_table = of_match_ptr(msm8660_wm8903_device_id),
	},
	.probe = msm8660_wm8903_platform_probe,
};
module_platform_driver(msm8660_wm8903_platform_driver);

MODULE_AUTHOR("Linus Walleij <linus.walleij@linaro.org>");
MODULE_DESCRIPTION("MSM8660 ASoC Machine Driver");
MODULE_LICENSE("GPL v2");
