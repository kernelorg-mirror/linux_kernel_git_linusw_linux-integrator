// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/gpio/consumer.h>

/**
 * enum ls_gpio - the GPIO lines on the low-speed connector
 */
enum ls_gpio {
	LS_GPIO_A = 0,
	LS_GPIO_B,
	LS_GPIO_C,
	LS_GPIO_D,
	LS_GPIO_E,
	LS_GPIO_F,
	LS_GPIO_G,
	LS_GPIO_H,
	LS_GPIO_I,
	LS_GPIO_J,
	LS_GPIO_K,
	LS_GPIO_L,
};

/*
 * We try to use the most simplistic device model: here is the LS
 * connector, it is a custom bus with its own type of devices and drivers
 * on it.
 */

extern struct bus_type ls_bus_type;

/**
 * struct ls_connector - the connector per se
 * @dev: parent device (platform device in the device tree case)
 * @i2c0: upward i2c0 I2C bus
 * @i2c1: upward i2c1 I2C bus
 * @spi: upward SPI bus
 */
struct ls_connector {
	struct device *dev;
	struct i2c_adapter *i2c0;
	struct i2c_adapter *i2c1;
	struct spi_controller *spi;
};

struct ls_device {
	struct device dev;
	const char *compatible;
	int id;
	struct i2c_adapter *i2c0;
	struct i2c_adapter *i2c1;
	struct spi_controller *spi;
};

struct ls_driver {
	struct device_driver drv;
	struct dev_ext_attribute ext_attr;
	int (*probe)(struct ls_device *);
	void (*remove)(struct ls_device *);
};

#define to_ls_device(d) \
	container_of(d, struct ls_device, dev)
#define to_ls_driver(d) \
	container_of(d, struct ls_driver, drv)

extern int ls_driver_register(struct ls_driver *);
extern void ls_driver_unregister(struct ls_driver *);

struct gpio_desc *ls_get_gpiod(struct ls_device *ls,
			       enum ls_gpio pin,
			       const char *consumer_name,
			       enum gpiod_flags flags);
void ls_put_gpiod(struct ls_device *ls,
		  struct gpio_desc *gpiod);
