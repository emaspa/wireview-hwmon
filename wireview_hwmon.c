// SPDX-License-Identifier: GPL-2.0
/*
 * wireview_hwmon - Virtual hwmon driver for WireView Pro II
 *
 * Exposes GPU power monitoring data from the WireView Pro II USB device
 * through the Linux hwmon subsystem.
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
#include <linux/version.h>

#define WIREVIEW_MAGIC   0x57565032  /* "WVP2" */
#define WIREVIEW_VERSION 2
#define WIREVIEW_STALE_MS 5000

struct wireview_hwmon_data {
	__u32 magic;
	__u32 version;
	__s32 voltage_mv[6];       /* per-pin voltages (mV) */
	__s32 current_ma[6];       /* per-pin currents (mA) */
	__s64 total_power_uw;      /* total power (uW) */
	__s32 temp_mc[4];          /* temperatures (millidegrees C) */
	__s64 pin_power_uw[6];     /* per-pin power (uW) */
	__s32 total_current_ma;    /* total current (mA) */
	__s32 avg_voltage_mv;      /* average voltage (mV) */
	__s32 vdd_mv;              /* supply voltage (mV) */
	__u8  fan_duty;            /* fan duty 0-100% */
	__u8  psu_cap;             /* PSU capability enum */
	__u16 fault_status;        /* active fault bitmask */
	__u16 fault_log;           /* historical fault bitmask */
	__u16 _pad;
} __packed;

static_assert(sizeof(struct wireview_hwmon_data) == 148, "struct size mismatch");

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

/* ---- hwmon labels ---- */

/*
 * Voltages: in0-in5 = Pin 1-6, in6 = Average, in7 = Vdd
 * Currents: curr1-curr6 = Pin 1-6, curr7 = Total
 * Power:    power1 = Total, power2-power7 = Pin 1-6
 * Temps:    temp1-temp4 = Onboard In, Onboard Out, External 1, External 2
 */

static const char * const voltage_labels[] = {
	"Pin 1", "Pin 2", "Pin 3", "Pin 4", "Pin 5", "Pin 6",
	"Average", "Vdd"
};

static const char * const current_labels[] = {
	"Pin 1", "Pin 2", "Pin 3", "Pin 4", "Pin 5", "Pin 6",
	"Total"
};

static const char * const power_labels[] = {
	"Total",
	"Pin 1", "Pin 2", "Pin 3", "Pin 4", "Pin 5", "Pin 6"
};

static const char * const temp_labels[] = {
	"Onboard In", "Onboard Out", "External 1", "External 2"
};

/* ---- extra sysfs attributes ---- */

static ssize_t intrusion0_label_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "Fault Status\n");
}

static ssize_t intrusion1_label_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "Fault Log\n");
}

static ssize_t fault_status_raw_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct wireview_priv *priv = dev_get_drvdata(dev);
	u16 val;

	mutex_lock(&priv->lock);
	if (!priv->data_valid) {
		mutex_unlock(&priv->lock);
		return -ENODATA;
	}
	val = priv->data.fault_status;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t fault_log_raw_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct wireview_priv *priv = dev_get_drvdata(dev);
	u16 val;

	mutex_lock(&priv->lock);
	if (!priv->data_valid) {
		mutex_unlock(&priv->lock);
		return -ENODATA;
	}
	val = priv->data.fault_log;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t psu_cap_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct wireview_priv *priv = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&priv->lock);
	if (!priv->data_valid) {
		mutex_unlock(&priv->lock);
		return -ENODATA;
	}
	val = priv->data.psu_cap;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%u\n", val);
}

static DEVICE_ATTR_RO(intrusion0_label);
static DEVICE_ATTR_RO(intrusion1_label);
static DEVICE_ATTR_RO(fault_status_raw);
static DEVICE_ATTR_RO(fault_log_raw);
static DEVICE_ATTR_RO(psu_cap);

static struct attribute *wireview_extra_attrs[] = {
	&dev_attr_intrusion0_label.attr,
	&dev_attr_intrusion1_label.attr,
	&dev_attr_fault_status_raw.attr,
	&dev_attr_fault_log_raw.attr,
	&dev_attr_psu_cap.attr,
	NULL
};

static const struct attribute_group wireview_extra_group = {
	.attrs = wireview_extra_attrs,
};

static const struct attribute_group *wireview_extra_groups[] = {
	&wireview_extra_group,
	NULL
};

/* ---- hwmon callbacks ---- */

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
	case hwmon_fan:
		if (attr == hwmon_fan_input)
			return 0444;
		break;
	case hwmon_intrusion:
		if (attr == hwmon_intrusion_alarm)
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
		if (channel < 6)
			*val = priv->data.voltage_mv[channel];
		else if (channel == 6)
			*val = priv->data.avg_voltage_mv;
		else /* channel 7 */
			*val = priv->data.vdd_mv;
		break;
	case hwmon_curr:
		if (channel < 6)
			*val = priv->data.current_ma[channel];
		else /* channel 6 = curr7 */
			*val = priv->data.total_current_ma;
		break;
	case hwmon_power:
		if (channel == 0)
			*val = priv->data.total_power_uw;
		else /* channels 1-6 = power2-power7 */
			*val = priv->data.pin_power_uw[channel - 1];
		break;
	case hwmon_temp:
		if (priv->data.temp_mc[channel] == S32_MIN) {
			mutex_unlock(&priv->lock);
			return -ENODATA;
		}
		*val = priv->data.temp_mc[channel];
		break;
	case hwmon_fan:
		/* Report fan duty as "RPM" scaled 0-100 for visibility */
		*val = priv->data.fan_duty;
		break;
	case hwmon_intrusion:
		if (channel == 0)
			*val = priv->data.fault_status != 0 ? 1 : 0;
		else
			*val = priv->data.fault_log != 0 ? 1 : 0;
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
		*str = power_labels[channel];
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
		HWMON_I_INPUT | HWMON_I_LABEL,   /* in0: Pin 1 */
		HWMON_I_INPUT | HWMON_I_LABEL,   /* in1: Pin 2 */
		HWMON_I_INPUT | HWMON_I_LABEL,   /* in2: Pin 3 */
		HWMON_I_INPUT | HWMON_I_LABEL,   /* in3: Pin 4 */
		HWMON_I_INPUT | HWMON_I_LABEL,   /* in4: Pin 5 */
		HWMON_I_INPUT | HWMON_I_LABEL,   /* in5: Pin 6 */
		HWMON_I_INPUT | HWMON_I_LABEL,   /* in6: Average */
		HWMON_I_INPUT | HWMON_I_LABEL),  /* in7: Vdd */
	HWMON_CHANNEL_INFO(curr,
		HWMON_C_INPUT | HWMON_C_LABEL,   /* curr1: Pin 1 */
		HWMON_C_INPUT | HWMON_C_LABEL,   /* curr2: Pin 2 */
		HWMON_C_INPUT | HWMON_C_LABEL,   /* curr3: Pin 3 */
		HWMON_C_INPUT | HWMON_C_LABEL,   /* curr4: Pin 4 */
		HWMON_C_INPUT | HWMON_C_LABEL,   /* curr5: Pin 5 */
		HWMON_C_INPUT | HWMON_C_LABEL,   /* curr6: Pin 6 */
		HWMON_C_INPUT | HWMON_C_LABEL),  /* curr7: Total */
	HWMON_CHANNEL_INFO(power,
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power1: Total */
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power2: Pin 1 */
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power3: Pin 2 */
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power4: Pin 3 */
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power5: Pin 4 */
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power6: Pin 5 */
		HWMON_P_INPUT | HWMON_P_LABEL),  /* power7: Pin 6 */
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT | HWMON_T_LABEL,   /* temp1: Onboard In */
		HWMON_T_INPUT | HWMON_T_LABEL,   /* temp2: Onboard Out */
		HWMON_T_INPUT | HWMON_T_LABEL,   /* temp3: External 1 */
		HWMON_T_INPUT | HWMON_T_LABEL),  /* temp4: External 2 */
	HWMON_CHANNEL_INFO(fan,
		HWMON_F_INPUT),                  /* fan1: duty % */
	HWMON_CHANNEL_INFO(intrusion,
		HWMON_INTRUSION_ALARM,           /* intrusion0: fault status */
		HWMON_INTRUSION_ALARM),          /* intrusion1: fault log */
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
							 wireview_extra_groups);
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

/*
 * platform_driver::remove() returned int before Linux 6.11 and void from
 * 6.11 onwards (commit 0edb555a65d1). Match the running kernel's signature so
 * the module builds on both older (e.g. RHEL 9, 5.14-based) and newer kernels.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int wireview_remove(struct platform_device *pdev)
#else
static void wireview_remove(struct platform_device *pdev)
#endif
{
	struct wireview_priv *priv = platform_get_drvdata(pdev);

	wireview_global = NULL;
	misc_deregister(&priv->misc);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
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
