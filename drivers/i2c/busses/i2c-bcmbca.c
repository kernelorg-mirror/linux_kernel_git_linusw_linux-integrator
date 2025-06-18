// SPDX-License-Identifier: GPL-2.0-only

/*
 * I2C driver for the Broadcom Broadband Access (BCA) chipsets.
 * Based on code and know-how from Pratapa Reddy <pratapas@yahoo.com>
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define CLK_375K    0
#define CLK_390K    1
#define CLK_187_5K  2 //187.5K
#define CLK_200K    3
#define CLK_93_75K  4 //93.75K
#define CLK_97_5K   5
#define CLK_46_875K 6 //46.875K
#define CLK_50K     7
#define I2C_CTL_SCL_SEL_S             4
#define I2C_CTL_SCL_SEL_M             3
#define I2C_CTL_DIV_CLK_S             7
#define I2C_CTL_DIV_CLK_M             1

/* Max slave clock stretching per transaction as per SMBus spec */
#define TLOW_SEXT  25000
/* Max master clock stretching per transaction as per SMBus spec */
#define TLOW_MEXT  10000
/* Maximum clock cycles for a 32B transaction */
#define MAX_CLKS_PER_TRANSACTION  306; /* 34 * 9 */

/* Time in uS when CLK = 375K or 390K */
#define CLK_375K_TIME 3
/* Time in uS when CLK = 187.5K or 200K */
#define CLK_187_5K_TIME 6


#define I2C_BCMBCA_CHIP_ADDRESS		0x00
#define I2C_BCMBCA_DATAIN_0		0x04
#define I2C_BCMBCA_DATAIN_1		0x08
#define I2C_BCMBCA_DATAIN_2		0x0c
#define I2C_BCMBCA_DATAIN_3		0x10
#define I2C_BCMBCA_DATAIN_4		0x14
#define I2C_BCMBCA_DATAIN_5		0x18
#define I2C_BCMBCA_DATAIN_6		0x1c
#define I2C_BCMBCA_DATAIN_7		0x20
#define I2C_BCMBCA_CNT			0x24
#define I2C_BCMBCA_CTL			0x28
#define I2C_BCMBCA_IIC_ENABLE		0x2c
#define I2C_BCMBCA_DATAOUT_0		0x30
#define I2C_BCMBCA_DATAOUT_1		0x34
#define I2C_BCMBCA_DATAOUT_2		0x38
#define I2C_BCMBCA_DATAOUT_3		0x3c
#define I2C_BCMBCA_DATAOUT_4		0x40
#define I2C_BCMBCA_DATAOUT_5		0x44
#define I2C_BCMBCA_DATAOUT_6		0x48
#define I2C_BCMBCA_DATAOUT_7		0x4c
#define I2C_BCMBCA_CTL_HI		0x50
#define I2C_BCMBCA_SCL_PARAM		0x54

struct i2c_bcmbca {
	struct device *dev;
	struct i2c_adapter adapter;
	struct completion completion;
	void __iomem *base;
	int xfer_timeout;
};

#define I2C_CHIP_ADDRESS_MASK           0x000000f7
#define I2C_CHIP_ADDRESS_SHIFT          0x1

#define I2C_CNT_REG1_SHIFT              0x0
#define I2C_CNT_REG2_SHIFT              0x6

#define I2C_CTL_DTF_MASK            0x00000003
#define I2C_CTL_DTF_WRITE           0x0
#define I2C_CTL_DTF_READ            0x1
#define I2C_CTL_DTF_READ_AND_WRITE  0x2
#define I2C_CTL_DTF_WRITE_AND_READ  0x3
#define I2C_CTL_DEGLITCH_DISABLE    0x00000004
#define I2C_CTL_DELAY_DISABLE       0x00000008
#define I2C_CTL_SCL_SEL_MASK        0x00000030
#define I2C_CTL_SCL_CLK_375KHZ      0x00000000
#define I2C_CTL_SCL_CLK_390KHZ      0x00000010
#define I2C_CTL_SCL_CLK_187_5KHZ    0x00000020
#define I2C_CTL_SCL_CLK_200KHZ      0x00000030
#define I2C_CTL_INT_ENABLE          0x00000040
#define I2C_CTL_DIV_CLK             0x00000080

#define I2C_IIC_ENABLE			BIT(0)
#define I2C_IIC_INTRP			BIT(1)
#define I2C_IIC_NO_ACK			BIT(2)
#define I2C_IIC_NO_STOP			BIT(4)
#define I2C_IIC_NO_START		BIT(5)

#define I2C_CTLHI_WAIT_DISABLE		BIT(0)
#define I2C_CTLHI_IGNORE_ACK		BIT(1)
#define I2C_CTLHI_DATA_SIZE		BIT(6)

/*  Keep polling until the bcm63000 completes the I2C transaction or timeout */
static int bcmbca_i2c_wait_xfer_done(struct i2c_bcmbca *b)
{
	int i;
	u32 val;

	for (i = 0; i < b->xfer_timeout; i++) {
		val = readl(b->base + I2C_BCMBCA_IIC_ENABLE);

		if (val & I2C_IIC_NO_ACK) {
			dev_err(b->dev, "No ACK received\n");
			return -EIO;
		}
		/*
		 * The xfer is complete and successful when INTRP is set
		 * and NOACK is not set.
		 */
		if (val & I2C_IIC_INTRP)
			return 0;

		/* TBD: use usleep_range() */
		udelay(1);
	}

	return -ETIMEDOUT;
}

static void bcmbca_i2c_rd_wr_common(struct i2c_bcmbca *b, u8 chip_addr, u16 xfer_cnt, u8 dtf)
{
	u32 val;

	/* Write the Address */
	writel(chip_addr << I2C_CHIP_ADDRESS_SHIFT, b->base + I2C_BCMBCA_CHIP_ADDRESS);

	/* Write the Transfer Count */
	writel(xfer_cnt, b->base + I2C_BCMBCA_CNT);

	/* Set the Control Register Data Transfer Format in the lowest bits */
	val = readl(b->base + I2C_BCMBCA_CTL);
	val &= ~I2C_CTL_DTF_MASK;
	val |= dtf;
	writel(val, b->base + I2C_BCMBCA_CTL);
}

/*
 * Read data of given length from the slave. Note that 2 or more transactions
 * would be required on the bus if the length is more than fifo_len
 */
static int bcmbca_i2c_read(struct i2c_bcmbca *b, struct i2c_msg *p, int fifo_len)
{
	int i, j, ret = 0;
	u16 xfer_cnt;
	unsigned int len = p->len;
	unsigned char *buf = p->buf;
	unsigned char addr = p->addr;
	int loops = (len + (fifo_len - 1))/fifo_len;
	u32 val;

	for (i = 0; i < loops; i++) {
		/*
		 * Make sure IIC_Enable bit is zero before start of a new transaction.
		 * Note that a 0-1 transition of IIC_Enable is required to initiate
		 * an I2C bus transaction
		 */
		writel(0, b->base + I2C_BCMBCA_IIC_ENABLE);

		/* Transfer size for the last iteration will be the remaining size */
		xfer_cnt = (i == loops-1) ? (len - (i * fifo_len)) : fifo_len;

		/* Set the address, transfer count, and data transfer format */
		bcmbca_i2c_rd_wr_common(b, addr, xfer_cnt, I2C_CTL_DTF_READ);

		/* Start the I2C transaction */
		val = I2C_IIC_ENABLE;

		/* Handle multiple loops */
		if (loops > 1) {
			if (i == 0) {
				/* first loop, with start and without stop */
				val |= I2C_IIC_NO_STOP;
			} else if (i == (loops - 1)) {
				/* last loop, without start[also without phy address, r/w bits], with stop */
				val |= I2C_IIC_NO_START;
			} else {
				/* middle loops, without start[also without phy address, r/w bits], without stop */
				val |= I2C_IIC_NO_START | I2C_IIC_NO_STOP;
			}
		}
		writel(val, b->base + I2C_BCMBCA_IIC_ENABLE);

		/* Wait for completion */
		ret = bcmbca_i2c_wait_xfer_done(b);
		if (ret)
			goto end;

		/* Read the data */
		if (fifo_len == 8) {
			/* 8-bit data */
			for (j = 0; j < xfer_cnt; j++)
				buf[i*fifo_len + j] = (u8)readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4);
		} else {
			/* 32-bit data */
			int num_dwords = xfer_cnt / 4;
			int left_over = xfer_cnt % 4;
			u32 temp_data = 0;
			for (j = 0; j < num_dwords; j++) {
				if (!IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
					*((int *)&buf[i*fifo_len + j*4]) =
						readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4);
				} else {
					*((int *)&buf[i*fifo_len + j*4]) =
						swab32(readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4));
				}
				if (left_over) {
					if (!IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
						temp_data =
							readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4);
					} else {
						temp_data =
							swab32(readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4));
					}
					memcpy((char*)&buf[i*fifo_len + j*4], (char*)&temp_data, left_over);
				}
			}
		}
	}

end:
	/* I2C bus transaction is complete, so set the IIC_Enable to zero */
	writel(0, b->base + I2C_BCMBCA_IIC_ENABLE);

	return ret;
}

/*
 * Write data of given length to the slave. Note that 2 or more transactions
 * would be required on the bus if the length is more than the fifo_len
 */
static int bcmbca_i2c_write(struct i2c_bcmbca *b, struct i2c_msg *p, int fifo_len)
{
	int i, j, ret = 0;
	u16 xfer_cnt;
	unsigned int len = p->len;
	unsigned char *buf = p->buf;
	unsigned char addr = p->addr;
	int loops = (len + (fifo_len - 1))/fifo_len;
	u32 val;

	for (i = 0; i < loops; i++) {
		/*
		 * Make sure IIC_Enable bit is zero before start of a new transaction.
		 * Note that a 0-1 transition of IIC_Enable is required to initiate
		 * an I2C bus transaction
		 */
		writel(0, b->base + I2C_BCMBCA_IIC_ENABLE);

		/* Transfer size for the last iteration will be the remaining size */
		xfer_cnt = (i==loops-1) ? (len - (i * fifo_len)) : fifo_len;

		/* Set the address, transfer count, and data transfer format */
		bcmbca_i2c_rd_wr_common(b, addr, xfer_cnt, I2C_CTL_DTF_WRITE);

		/* Write the data */
		if (fifo_len == 8) {
			/* 8-bit data */
			for (j = 0; j < xfer_cnt; j++)
				writel(buf[i * fifo_len + j], b->base + I2C_BCMBCA_DATAIN_0 + j*4);
		} else {
			/* 32-bit data */
			int num_dwords = (xfer_cnt + 3)/4;
			for (j = 0; j < num_dwords; j++) {
				if (!IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
					writel(*((int *)&buf[i * fifo_len + j*4]), b->base + I2C_BCMBCA_DATAIN_0 + j*4);
				} else {
					writel(swab32(*((int *)&buf[i * fifo_len + j*4])), b->base + I2C_BCMBCA_DATAIN_0 + j*4);
				}
			}
		}

		/* Start the I2C transaction */
		val = I2C_IIC_ENABLE;

		/* multiple loops */
		if (loops > 1) {
			if (i == 0) {
				/* first loop, with start and without stop */
				val |= I2C_IIC_NO_STOP;
			} else if (i == (loops - 1)) {
				/* last loop, without start[also without phy address, r/w bits], with stop */
				val |= I2C_IIC_NO_START;
			} else {
				/* middle loops, without start[also without phy address, r/w bits], without stop */
				val |= I2C_IIC_NO_START | I2C_IIC_NO_STOP;
			}
		}

		writel(val, b->base + I2C_BCMBCA_IIC_ENABLE);

		/* Wait for completion */
		ret = bcmbca_i2c_wait_xfer_done(b);
		if (ret)
			/* Breaks the loop on error */
			goto end;
	}

end:
	/* I2C bus transaction is complete, so set the IIC_Enable to zero */
	writel(0, b->base + I2C_BCMBCA_IIC_ENABLE);

	return ret;
}

/* read followed by write without a stop in between */
static int bcmbca_i2c_read_then_write(struct i2c_bcmbca *b, struct i2c_msg *msgs, int fifo_len)
{
	int j, ret = 0;
	u16 xfer_cnt;
	struct i2c_msg *p = &msgs[0], *q = &msgs[1];

	writel(0, b->base + I2C_BCMBCA_IIC_ENABLE);

	/* Set the transfer counts for both the transactions */
	xfer_cnt = (q->len << I2C_CNT_REG2_SHIFT) | p->len;

	/* Set the address, transfer count, and data transfer format */
	bcmbca_i2c_rd_wr_common(b, (u8)p->addr, xfer_cnt, I2C_CTL_DTF_READ_AND_WRITE);

	/* Write the data */
	if (fifo_len == 8) {
		/* 8-bit data */
		for (j = 0; j < (q->len); j++)
			writel(q->buf[j], b->base + I2C_BCMBCA_DATAIN_0 + j*4);
	} else {
		/* 32-bit data */
		int num_dwords = ((q->len) + 3)/4;
		for (j = 0; j < num_dwords; j++) {
			if (!IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
				writel(*((int *)&q->buf[j*4]), b->base + I2C_BCMBCA_DATAIN_0 + j*4);
			} else {
				writel(swab32(*((int *)&q->buf[j*4])), b->base + I2C_BCMBCA_DATAIN_0 + j*4);
			}
		}
	}

	/* Start the I2C transaction */
	writel(I2C_IIC_ENABLE, b->base + I2C_BCMBCA_IIC_ENABLE);

	/* Wait for completion */
	ret = bcmbca_i2c_wait_xfer_done(b);
	if (ret)
		/* Breaks on error */
		goto end;

	/* Read the data */
	/* FIXME: code duplication */
	if (fifo_len == 8) {
		/* 8-bit data */
		for (j = 0; j < (p->len); j++)
			p->buf[j] = (u8)readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4);
	} else {
		/* 32-bit data */
		int num_dwords = p->len / 4;
		int left_over = p->len % 4;
		u32 temp_data = 0;
		for (j = 0; j < num_dwords; j++) {
			if (!IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
				*((int *)&p->buf[j*4]) =
					readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4);
			} else {
				*((int *)&p->buf[j*4]) =
					swab32(readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4));
			}
		}

		if (left_over) {
			if (!IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
				temp_data =
					readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4);
			} else {
				temp_data =
					swab32(readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4));
			}
			memcpy((char*)&p->buf[j*4], (char*)&temp_data, left_over);
		}

        }

end:
	/* I2C bus transaction is complete, so set the IIC_Enable to zero */
	writel(0, b->base + I2C_BCMBCA_IIC_ENABLE);

	return ret;
}

/* write followed by read without a stop in between */
static int bcmbca_i2c_write_then_read(struct i2c_bcmbca *b, struct i2c_msg *msgs, int fifo_len)
{
	int j, ret = 0;
	u16 xfer_cnt;
	struct i2c_msg *p = &msgs[0], *q = &msgs[1];

	writel(0, b->base + I2C_BCMBCA_IIC_ENABLE);

	/* Set the transfer counts for both the transactions */
	xfer_cnt = (q->len << I2C_CNT_REG2_SHIFT) | p->len;

	/* Set the address, transfer count, and data transfer format */
	bcmbca_i2c_rd_wr_common(b, (u8)p->addr, xfer_cnt, I2C_CTL_DTF_WRITE_AND_READ);

	/* Write the data */
	if (fifo_len == 8) {
		/* 8-bit data */
		for (j = 0; j < (p->len); j++)
			writel(p->buf[j], b->base + I2C_BCMBCA_DATAIN_0 + j*4);
	} else {
		/* 32-bit data */
		int num_dwords = ((p->len) + 3)/4;
		for (j = 0; j < num_dwords; j++) {
			u32 data = 0;

			data |= ((j * 4 + 3) < p->len ? p->buf[j * 4 + 3] : 0) << 24;
			data |= ((j * 4 + 2) < p->len ? p->buf[j * 4 + 2] : 0) << 16;
			data |= ((j * 4 + 1) < p->len ? p->buf[j * 4 + 1] : 0) << 8;
			data |= p->buf[j * 4];
			writel(data, b->base + I2C_BCMBCA_DATAIN_0 + j*4);
		}
	}

	/* Start the I2C transaction */
	writel(I2C_IIC_ENABLE, b->base + I2C_BCMBCA_IIC_ENABLE);

	/* Wait for completion */
	ret = bcmbca_i2c_wait_xfer_done(b);
	if (ret)
		/* Breaks on error */
		goto end;

	/* Read the data */
	if (fifo_len == 8) {
		/* 8-bit data */
		for (j = 0; j < (q->len); j++)
			q->buf[j] = (u8)readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4);
	} else {
		/* 32-bit data */
		int num_dwords = q->len / 4;
		int left_over = q->len % 4;
		u32 temp_data = 0;
		for (j = 0; j < num_dwords; j++) {
			if (!IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
				*((int *)&q->buf[j*4]) =
					readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4);
			} else {
				*((int *)&q->buf[j*4]) =
					swab32(readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4));
			}
		}
		if (left_over) {
			if (!IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
				temp_data =
					readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4);
			} else {
				temp_data =
					swab32(readl(b->base + I2C_BCMBCA_DATAOUT_0 + j*4));
			}
			memcpy((char*)&q->buf[j*4], (char*)&temp_data, left_over);

		}
        }

end:
	/* I2C bus transaction is complete, so set the IIC_Enable to zero */
	writel(0, b->base + I2C_BCMBCA_IIC_ENABLE);

	return ret;
}

static int bcmbca_xfer(struct i2c_adapter *adap,
		       struct i2c_msg *msgs, int num)
{
	struct i2c_msg *p, *q;
	int i, err = 0;
	u32 val;
	/* Determine the FIFO length for Read and Write Transactions */
	int fifo_len;
	struct i2c_bcmbca *b = i2c_get_adapdata(adap);

	val = readl(b->base + I2C_BCMBCA_CTL_HI);
	fifo_len = (val & I2C_CTLHI_DATA_SIZE) ? 32 : 8;

	if (num == 2) {
		p = &msgs[0]; q = &msgs[1];
		/*
		 * We may be able to use the read_then_write and write_then_read
		 * formats only when the length of each message is less than fifo_len
		 */
		if (p->len <= fifo_len && q->len <= fifo_len) {
			if ((p->flags == I2C_M_RD) && (!(q->flags & I2C_M_RD))) {
				err = bcmbca_i2c_read_then_write(b, msgs, fifo_len);
				return (err < 0) ? err : 2;
			} else if ((!(p->flags & I2C_M_RD)) && (q->flags == I2C_M_RD)) {
				err = bcmbca_i2c_write_then_read(b, msgs, fifo_len);
				return (err < 0) ? err : 2;
			}
		}
	}

	/*
	 * TBD:See if we can use NO_STOP b/w multiple read and write transactions
	 */
	for (i = 0; !err && i < num; i++) {
		p = &msgs[i];
		if (p->flags & I2C_M_RD)
			err = bcmbca_i2c_read(b, p, fifo_len);
		else
			err = bcmbca_i2c_write(b, p, fifo_len);
	}

	return (err < 0) ? err : i;
}

static u32 bcmbca_func(struct i2c_adapter *adap)
{
	/* TODO: I2C_FUNC_10BIT_ADDR, I2C_FUNC_PROTOCOL_MAGLING */
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm bcmbca_algo = {
	.master_xfer = bcmbca_xfer,
	.functionality = bcmbca_func,
};

static void bcmbca_i2c_bus_clk_setup(struct i2c_bcmbca *b)
{
	u32 freq;
	u32 val;
	u32 clk_sel;
	int ret;
	int clk_time = 1000;

	/* Set up the bus clock frequency */
	ret = device_property_read_u32(b->dev, "clock-frequency", &freq);
	if (ret) {
		dev_info(b->dev, "no clock frequency setting, using default\n");
		freq = 97500;
	}

	if (freq < 48000) {
		dev_info(b->dev, "set bus clock to 46875 Hz\n");
		clk_sel = CLK_46_875K;
	} else if (freq >= 48000 && freq < 92000) {
		dev_info(b->dev, "set bus clock to 50000 Hz\n");
		clk_sel = CLK_50K;
	} else if (freq >= 92000 && freq < 96000) {
		dev_info(b->dev, "set bus clock to 93750 Hz\n");
		clk_sel = CLK_93_75K;
	} else if (freq >= 96000 && freq < 150000) {
		dev_info(b->dev, "set bus clock to 97500 Hz\n");
		clk_sel = CLK_97_5K;
	} else if (freq >= 150000 && freq < 190000) {
		dev_info(b->dev, "set bus clock to 187500 Hz\n");
		clk_sel = CLK_187_5K;
	} else if (freq >= 190000 && freq < 250000) {
		dev_info(b->dev, "set bus clock to 200000 Hz\n");
		clk_sel = CLK_200K;
	} else {
		dev_info(b->dev, "set bus clock to 390000 Hz\n");
		clk_sel = CLK_390K;
	}

	/* TODO: look over this, M, mask, S, shift */
	val = (clk_sel & I2C_CTL_SCL_SEL_M) << I2C_CTL_SCL_SEL_S;
	if (clk_sel > CLK_200K)
		val |= (1 << I2C_CTL_DIV_CLK_S);

	writel(val, b->base + I2C_BCMBCA_CTL);

	switch (clk_sel) {
	case 0:
	case 1:
	case 4:
	case 5:
		/* approx clock time in uS */
		clk_time = CLK_375K_TIME * ((clk_sel>I2C_CTL_SCL_SEL_M)?4:1);
		break;
	case 2:
	case 3:
	case 6:
	case 7:
		/* approx clock time in uS */
		clk_time = CLK_187_5K_TIME * ((clk_sel>I2C_CTL_SCL_SEL_M)?4:1);
		break;
	}

	/* xfer_timeout in mS */
	b->xfer_timeout = TLOW_SEXT + TLOW_MEXT + clk_time * MAX_CLKS_PER_TRANSACTION;
}

static int bcmbca_i2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct i2c_bcmbca *b;
	struct i2c_adapter *adap;
	u32 val;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	platform_set_drvdata(pdev, b);
	b->dev = dev;
	init_completion(&b->completion);

	b->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(b->base))
		return PTR_ERR(b->base);

	adap = &b->adapter;
	i2c_set_adapdata(adap, b);
	adap->owner = THIS_MODULE;
	snprintf(adap->name, sizeof(adap->name), "%s", dev_name(dev));
	adap->algo = &bcmbca_algo;
	adap->dev.parent = dev;
	adap->dev.of_node = dev->of_node;

	bcmbca_i2c_bus_clk_setup(b);
	/*
	 * Set the data size field to 32-bit mode
	 *
	 * If this length is changed to 8, the write byte stream functions
	 * of client drivers may not work properly when writing more than
	 * 8 bytes as some of the i2c client devices require the first byte of
	 * each I2C write transaction as the offset
	 */
	val = I2C_CTLHI_DATA_SIZE;
	writel(val, b->base + I2C_BCMBCA_CTL_HI);

	dev_info(dev, "Added Broadcom BCMBCA I2C driver");

	return devm_i2c_add_adapter(dev, adap);
}

static const struct of_device_id bcmbca_i2c_of_match[] = {
	{ .compatible = "brcm,bcmbca-i2c" },
	{ },
};
MODULE_DEVICE_TABLE(of, bcmbca_i2c_of_match);

static struct platform_driver bcmbca_i2c_driver = {
	.probe = bcmbca_i2c_probe,
	.driver = {
		.name = "i2c-bcmbca",
		.of_match_table = bcmbca_i2c_of_match,
	},
};
module_platform_driver(bcmbca_i2c_driver);

MODULE_DESCRIPTION("I2C bus driver for the BCMBCA SoCs");
MODULE_LICENSE("GPL");
