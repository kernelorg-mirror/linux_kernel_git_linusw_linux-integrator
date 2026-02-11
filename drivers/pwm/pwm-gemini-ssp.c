// SPDX-License-Identifier: GPL-2.0+
/* Hack driver using the Faraday FTSSP010 as PWM generator */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>

#define SSP_DEVICE_ID		0x00
#define SSP_CTRL_STATUS		0x04
#define SSP_FRAME_CTRL		0x08
#define SSP_BAUD_RATE		0x0c
#define SSP_FRAME_CTRL2		0x10
#define SSP_FIFO_CTRL		0x14
#define SSP_TX_SLOT_VALID0	0x18
#define SSP_TX_SLOT_VALID1	0x1c
#define SSP_TX_SLOT_VALID2	0x20
#define SSP_TX_SLOT_VALID3	0x24
#define SSP_RX_SLOT_VALID0	0x28
#define SSP_RX_SLOT_VALID1	0x2c
#define SSP_RX_SLOT_VALID2	0x30
#define SSP_RX_SLOT_VALID3	0x34
#define SSP_SLOT_SIZE0		0x38
#define SSP_SLOT_SIZE1		0x3c
#define SSP_SLOT_SIZE2		0x40
#define SSP_SLOT_SIZE3		0x44
#define SSP_READ_PORT		0x48
#define SSP_WRITE_PORT		0x4c

struct ftssp010_pwm_chip {
	struct device *dev;
	void __iomem *base;
};

static inline struct ftssp010_pwm_chip *
to_ftssp010_pwm_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int ftssp010_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			 const struct pwm_state *state)
{
	struct ftssp010_pwm_chip *ftssp = to_ftssp010_pwm_chip(chip);
	unsigned int duty;
	u32 val;

#if 0
	if (!state->enabled && pwm_is_enabled(pwm))
		writel(0x1F100000, ftssp->base + SSP_CTRL_STATUS);
#endif

	/* Scale to 0..0x7f */
	duty = state->duty_cycle * 128;
	duty = DIV_ROUND_CLOSEST(duty, (unsigned int)state->period);
	if (duty > 0x7f)
		duty = 0x7f;
	duty &= 0x7f;
	dev_info(ftssp->dev, "set duty cycle 0x%02x\n", duty);

	if (pwm_get_duty_cycle(pwm) < 0x10 && duty > 0x0f) {
		/* Set to max */
		val = readl(ftssp->base + SSP_FRAME_CTRL2);
		val |= 0x7f;
		writel(val, ftssp->base + SSP_FRAME_CTRL2);
		msleep(1000); // spin up
	}

	/* Duty cycle 0..7f, 0 will stop the fan */
	val = readl(ftssp->base + SSP_FRAME_CTRL2);
        val &= ~0x7F;
        val |= (duty & 0x7F);
        writel(val, ftssp->base + SSP_FRAME_CTRL2);

#if 0
	if (state->enabled && !pwm_is_enabled(pwm)) {
		writel(0x1F100000, ftssp->base + SSP_CTRL_STATUS);
		msleep(250);
		writel(0x00100000, ftssp->base + SSP_CTRL_STATUS);
	}
#endif

	return 0;
}

static const struct pwm_ops ftssp010_pwm_ops = {
	.apply = ftssp010_pwm_apply,
};

static void ftssp010_init(struct ftssp010_pwm_chip *ftssp)
{
	u32 val;

	val = readl(ftssp->base + SSP_DEVICE_ID);
	dev_info(ftssp->dev, "FTSSP010 device ID %08x\n", val);

	/*
	 * Set bit 16 to 1 to use the external clock for baud reference
	 * and maximum clock divider to start.
	 */
	writel(0x04010fff, ftssp->base + SSP_FRAME_CTRL);
	/*
	 * param_m = 3f
	 * param_n = 06
	 * param_a2 = 10 (-1 cycle adjustment)
	 * param_a1 = 01 (+1 cycle adjustment)
	 */
	writel(0x3f020601, ftssp->base + SSP_BAUD_RATE);
	/* Max duty cycle 0x7f, max speed */
	writel(0x000F807F, ftssp->base + SSP_FRAME_CTRL2);
	/* TXwrite 5, TXread 8, RXwrite 2, RXread 5 */
        writel(0x00004714, ftssp->base + SSP_FIFO_CTRL);
	/* One TX slot, one RX slot */
        writel(0x00000001, ftssp->base + SSP_TX_SLOT_VALID0);
        writel(0x00000000, ftssp->base + SSP_TX_SLOT_VALID1);
        writel(0x00000000, ftssp->base + SSP_TX_SLOT_VALID2);
        writel(0x00000000, ftssp->base + SSP_TX_SLOT_VALID3);
        writel(0x00000001, ftssp->base + SSP_RX_SLOT_VALID0);
        writel(0x00000000, ftssp->base + SSP_RX_SLOT_VALID1);
        writel(0x00000000, ftssp->base + SSP_RX_SLOT_VALID2);
        writel(0x00000000, ftssp->base + SSP_RX_SLOT_VALID3);
	/* Max slot size */
        writel(0xffffffff, ftssp->base + SSP_SLOT_SIZE0);
        writel(0xffffffff, ftssp->base + SSP_SLOT_SIZE1);
        writel(0xffffffff, ftssp->base + SSP_SLOT_SIZE2);
        writel(0xffffffff, ftssp->base + SSP_SLOT_SIZE3);
	/* stop and start */
        writel(0x1F100000, ftssp->base + SSP_CTRL_STATUS);
        msleep(250);
        writel(0x00100000, ftssp->base + SSP_CTRL_STATUS);
}

static int ftssp010_pwm_probe(struct platform_device *pdev)
{
	struct pwm_chip *chip;
	struct ftssp010_pwm_chip *ftssp;
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, 1, sizeof(*ftssp));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	ftssp = to_ftssp010_pwm_chip(chip);
	ftssp->dev = &pdev->dev;

	ftssp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ftssp->base))
		return PTR_ERR(ftssp->base);

	ftssp010_init(ftssp);

	chip->ops = &ftssp010_pwm_ops;

	ret = devm_pwmchip_add(&pdev->dev, chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to add pwm chip %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct of_device_id ftssp010_pwm_dt_ids[] = {
	{ .compatible = "faraday,ftssp010", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ftssp010_pwm_dt_ids);

static struct platform_driver ftssp010_pwm_driver = {
	.driver = {
		.name = "ftssp010-pwm",
		.of_match_table = ftssp010_pwm_dt_ids,
	},
	.probe = ftssp010_pwm_probe,
};
module_platform_driver(ftssp010_pwm_driver);

MODULE_ALIAS("platform:ftssp010-pwm");
MODULE_AUTHOR("Linus Walleij <linusw@kernel.org>");
MODULE_DESCRIPTION("Farady FTSSP010 SSP PWM Driver");
MODULE_LICENSE("GPL v2");
