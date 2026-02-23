// SPDX-License-Identifier: GPL-2.0
/*
 * wireview_hwmon - Virtual hwmon driver for WireView Pro II
 *
 * Exposes GPU power monitoring data (voltage, current, power, temperature)
 * from the WireView Pro II USB device through the Linux hwmon subsystem.
 *
 * A userspace daemon (wireviewd) reads sensor data from the device over
 * serial and writes it to /dev/wireview-hwmon as a packed binary struct.
 * This module makes that data available via /sys/class/hwmon/ for tools
 * like lm-sensors, Grafana, conky, btop, etc.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>

#define WIREVIEW_MAGIC   0x57565032  /* "WVP2" */
#define WIREVIEW_VERSION 1
#define WIREVIEW_STALE_MS 5000

struct wireview_hwmon_data {
	__u32 magic;
	__u32 version;
	__s32 voltage_mv[6];
	__s32 current_ma[6];
	__s64 power_uw;
	__s32 temp_mc[4];
	__u64 _reserved;
} __packed;

static_assert(sizeof(struct wireview_hwmon_data) == 88, "struct size mismatch");

struct wireview_priv {
	struct mutex lock;
	struct wireview_hwmon_data data;
	bool data_valid;
	ktime_t last_update;
	struct miscdevice misc;
};

static struct wireview_priv *wireview_global;

/* ---- misc device: /dev/wireview-hwmon ---- */

static ssize_t wireview_misc_write(struct file *filp, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct wireview_priv *priv = wireview_global;
	struct wireview_hwmon_data tmp;

	if (!priv)
		return -ENODEV;

	if (count != sizeof(tmp))
		return -EINVAL;

	if (copy_from_user(&tmp, buf, sizeof(tmp)))
		return -EFAULT;

	if (tmp.magic != WIREVIEW_MAGIC)
		return -EINVAL;

	if (tmp.version != WIREVIEW_VERSION)
		return -EINVAL;

	mutex_lock(&priv->lock);
	priv->data = tmp;
	priv->data_valid = true;
	priv->last_update = ktime_get();
	mutex_unlock(&priv->lock);

	return count;
}

static const struct file_operations wireview_misc_fops = {
	.owner = THIS_MODULE,
	.write = wireview_misc_write,
};

/* ---- hwmon callbacks ---- */

static const char * const voltage_labels[] = {
	"Pin 1", "Pin 2", "Pin 3", "Pin 4", "Pin 5", "Pin 6"
};

static const char * const current_labels[] = {
	"Pin 1", "Pin 2", "Pin 3", "Pin 4", "Pin 5", "Pin 6"
};

static const char * const temp_labels[] = {
	"Onboard In", "Onboard Out", "External 1", "External 2"
};

static umode_t wireview_is_visible(const void *drvdata,
				   enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	switch (type) {
	case hwmon_in:
		if (attr == hwmon_in_input || attr == hwmon_in_label)
			return 0444;
		break;
	case hwmon_curr:
		if (attr == hwmon_curr_input || attr == hwmon_curr_label)
			return 0444;
		break;
	case hwmon_power:
		if (attr == hwmon_power_input || attr == hwmon_power_label)
			return 0444;
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_input || attr == hwmon_temp_label)
			return 0444;
		break;
	default:
		break;
	}
	return 0;
}

static int wireview_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct wireview_priv *priv = dev_get_drvdata(dev);
	ktime_t age;

	mutex_lock(&priv->lock);

	if (!priv->data_valid) {
		mutex_unlock(&priv->lock);
		return -ENODATA;
	}

	age = ktime_sub(ktime_get(), priv->last_update);
	if (ktime_to_ms(age) > WIREVIEW_STALE_MS) {
		mutex_unlock(&priv->lock);
		return -ENODATA;
	}

	switch (type) {
	case hwmon_in:
		*val = priv->data.voltage_mv[channel];
		break;
	case hwmon_curr:
		*val = priv->data.current_ma[channel];
		break;
	case hwmon_power:
		*val = priv->data.power_uw;
		break;
	case hwmon_temp:
		*val = priv->data.temp_mc[channel];
		break;
	default:
		mutex_unlock(&priv->lock);
		return -EOPNOTSUPP;
	}

	mutex_unlock(&priv->lock);
	return 0;
}

static int wireview_read_string(struct device *dev,
				enum hwmon_sensor_types type,
				u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_in:
		*str = voltage_labels[channel];
		break;
	case hwmon_curr:
		*str = current_labels[channel];
		break;
	case hwmon_power:
		*str = "Total";
		break;
	case hwmon_temp:
		*str = temp_labels[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static const struct hwmon_ops wireview_ops = {
	.is_visible = wireview_is_visible,
	.read = wireview_read,
	.read_string = wireview_read_string,
};

static const struct hwmon_channel_info * const wireview_info[] = {
	HWMON_CHANNEL_INFO(in,
		HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
		HWMON_C_INPUT | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_LABEL,
		HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power,
		HWMON_P_INPUT | HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_chip_info wireview_chip_info = {
	.ops = &wireview_ops,
	.info = wireview_info,
};

/* ---- platform driver ---- */

static int wireview_probe(struct platform_device *pdev)
{
	struct wireview_priv *priv;
	struct device *hwmon_dev;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	platform_set_drvdata(pdev, priv);

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
							 "wireview",
							 priv,
							 &wireview_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	priv->misc.minor = MISC_DYNAMIC_MINOR;
	priv->misc.name = "wireview-hwmon";
	priv->misc.fops = &wireview_misc_fops;

	ret = misc_register(&priv->misc);
	if (ret)
		return ret;

	wireview_global = priv;

	dev_info(&pdev->dev, "WireView hwmon driver loaded\n");
	return 0;
}

static void wireview_remove(struct platform_device *pdev)
{
	struct wireview_priv *priv = platform_get_drvdata(pdev);

	wireview_global = NULL;
	misc_deregister(&priv->misc);
}

static struct platform_driver wireview_driver = {
	.driver = {
		.name = "wireview_hwmon",
	},
	.probe = wireview_probe,
	.remove = wireview_remove,
};

static struct platform_device *wireview_pdev;

static int __init wireview_init(void)
{
	int ret;

	wireview_pdev = platform_device_register_simple("wireview_hwmon",
							-1, NULL, 0);
	if (IS_ERR(wireview_pdev))
		return PTR_ERR(wireview_pdev);

	ret = platform_driver_register(&wireview_driver);
	if (ret) {
		platform_device_unregister(wireview_pdev);
		return ret;
	}

	return 0;
}

static void __exit wireview_exit(void)
{
	platform_driver_unregister(&wireview_driver);
	platform_device_unregister(wireview_pdev);
}

module_init(wireview_init);
module_exit(wireview_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("WireView Linux Project");
MODULE_DESCRIPTION("Virtual hwmon driver for WireView Pro II power monitor");
