// SPDX-License-Identifier: GPL-2.0
/*
 * gpio3a1.c - GPIO3_A1 character device driver for TaishanPi RK3566
 *
 * Device tree node (see ../../dts/gpio3a1-overlay.dts):
 *
 *     gpio3a1: gpio3a1 {
 *             compatible = "work,gpio3a1";
 *             gpios = <&gpio3 RK_PA1 GPIO_ACTIVE_HIGH>;
 *     };
 *
 * Userspace interface /dev/gpio3a1:
 *   read  : returns "0\n" or "1\n"
 *   write : accepts "0", "1", "on", "off", "toggle" (optional trailing
 *           whitespace/newline is ignored)
 */

#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define DRIVER_NAME	"gpio3a1"
#define DEVICE_NAME	"gpio3a1"
#define CMD_BUF_SIZE	16

struct gpio3a1_dev {
	struct gpio_desc *gpiod;
	struct miscdevice miscdev;
};

static struct gpio3a1_dev *gpio3a1_from_file(struct file *file)
{
	return container_of(file->private_data, struct gpio3a1_dev, miscdev);
}

static ssize_t gpio3a1_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct gpio3a1_dev *priv = gpio3a1_from_file(file);
	char kbuf[2];
	int val;

	val = gpiod_get_value_cansleep(priv->gpiod);
	if (val < 0)
		return val;

	kbuf[0] = val ? '1' : '0';
	kbuf[1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, kbuf, sizeof(kbuf));
}

static ssize_t gpio3a1_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct gpio3a1_dev *priv = gpio3a1_from_file(file);
	char kbuf[CMD_BUF_SIZE];
	size_t len = min(count, sizeof(kbuf) - 1);

	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;

	kbuf[len] = '\0';
	strim(kbuf);

	if (!strcmp(kbuf, "0") || !strcmp(kbuf, "off")) {
		gpiod_set_value_cansleep(priv->gpiod, 0);
	} else if (!strcmp(kbuf, "1") || !strcmp(kbuf, "on")) {
		gpiod_set_value_cansleep(priv->gpiod, 1);
	} else if (!strcmp(kbuf, "toggle")) {
		int val = gpiod_get_value_cansleep(priv->gpiod);

		if (val < 0)
			return val;
		gpiod_set_value_cansleep(priv->gpiod, !val);
	} else {
		return -EINVAL;
	}

	return count;
}

static const struct file_operations gpio3a1_fops = {
	.owner		= THIS_MODULE,
	.read		= gpio3a1_read,
	.write		= gpio3a1_write,
	.llseek		= no_llseek,
};

static int gpio3a1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio3a1_dev *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->gpiod = devm_gpiod_get(dev, NULL, GPIOD_OUT_LOW);
	if (IS_ERR(priv->gpiod)) {
		ret = PTR_ERR(priv->gpiod);
		dev_err(dev, "failed to get GPIO3_A1: %d\n", ret);
		return ret;
	}
	gpiod_set_consumer_name(priv->gpiod, "gpio3a1");

	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	priv->miscdev.name = DEVICE_NAME;
	priv->miscdev.fops = &gpio3a1_fops;
	priv->miscdev.parent = dev;

	ret = misc_register(&priv->miscdev);
	if (ret) {
		dev_err(dev, "failed to register /dev/%s: %d\n",
			DEVICE_NAME, ret);
		return ret;
	}

	platform_set_drvdata(pdev, priv);
	dev_info(dev, "GPIO3_A1 ready: /dev/%s, current level %d\n",
		 DEVICE_NAME, gpiod_get_value_cansleep(priv->gpiod));

	return 0;
}

static int gpio3a1_remove(struct platform_device *pdev)
{
	struct gpio3a1_dev *priv = platform_get_drvdata(pdev);

	misc_deregister(&priv->miscdev);
	return 0;
}

static const struct of_device_id gpio3a1_of_match[] = {
	{ .compatible = "work,gpio3a1" },
	{ }
};
MODULE_DEVICE_TABLE(of, gpio3a1_of_match);

static struct platform_driver gpio3a1_driver = {
	.probe	= gpio3a1_probe,
	.remove	= gpio3a1_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= gpio3a1_of_match,
	},
};
module_platform_driver(gpio3a1_driver);

MODULE_AUTHOR("TaishanPi work");
MODULE_DESCRIPTION("GPIO3_A1 character device driver (TaishanPi RK3566)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
