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
 *
 * Supported on the kernels the packaged channels ship: Linux 6.8 (Ubuntu
 * 24.04) and newer. Older kernels are not tested.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/version.h>

#define WIREVIEW_MAGIC   0x57565032  /* "WVP2" */
#define WIREVIEW_VERSION 3          /* v3 appends energy_uj */
#define WIREVIEW_VERSION_V2 2       /* still accepted, without energy */
#define WIREVIEW_STALE_MS 5000

/* energy_uj of a v2 frame; any negative value means "not available" */
#define WIREVIEW_ENERGY_NA S64_MIN

/* Package version, injected by the build (-DWIREVIEW_PKG_VERSION="x.y.z"). */
#ifndef WIREVIEW_PKG_VERSION
#define WIREVIEW_PKG_VERSION "unknown"
#endif

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
	/* v3 only: energy (uJ), monotonic since daemon start */
	__s64 energy_uj;
} __packed;

/* A v2 frame is the v3 struct without the trailing energy_uj. */
#define WIREVIEW_DATA_V2_SIZE offsetof(struct wireview_hwmon_data, energy_uj)

static_assert(WIREVIEW_DATA_V2_SIZE == 148, "v2 struct size mismatch");
static_assert(sizeof(struct wireview_hwmon_data) == 156, "v3 struct size mismatch");

struct wireview_priv {
	struct mutex lock;
	struct wireview_hwmon_data data;
	bool data_valid;
	ktime_t last_update;
	struct miscdevice misc;
};

/*
 * True if a frame has arrived and is recent enough to report, so readers
 * stop serving the last frame once the daemon/device goes away.
 * Caller holds priv->lock.
 */
static bool wireview_data_fresh(struct wireview_priv *priv)
{
	lockdep_assert_held(&priv->lock);

	if (!priv->data_valid)
		return false;

	return ktime_ms_delta(ktime_get(), priv->last_update) <= WIREVIEW_STALE_MS;
}

/* ---- misc device: /dev/wireview-hwmon ---- */

static ssize_t wireview_misc_write(struct file *filp, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	/* misc_open() stores our struct miscdevice in private_data. */
	struct wireview_priv *priv = container_of(filp->private_data,
						  struct wireview_priv, misc);
	struct wireview_hwmon_data tmp;

	if (count != WIREVIEW_DATA_V2_SIZE && count != sizeof(tmp))
		return -EINVAL;

	if (copy_from_user(&tmp, buf, count))
		return -EFAULT;

	if (tmp.magic != WIREVIEW_MAGIC)
		return -EINVAL;

	/* The version must match the size actually written. */
	if (tmp.version == WIREVIEW_VERSION_V2 &&
	    count == WIREVIEW_DATA_V2_SIZE)
		tmp.energy_uj = WIREVIEW_ENERGY_NA;
	else if (tmp.version != WIREVIEW_VERSION || count != sizeof(tmp))
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
 * Voltages:  in0-in5 = Pin 1-6, in6 = Average, in7 = Vdd
 * Currents:  curr1-curr6 = Pin 1-6, curr7 = Total; currN_alarm on all
 * Power:     power1 = Total, power2-power7 = Pin 1-6;
 *            power1_cap = PSU capability, power1_alarm = over-power
 * Temps:     temp1-temp4 = Onboard In, Onboard Out, External 1, External 2;
 *            tempN_alarm on all
 * Energy:    energy1 = Total (from v3 frames only)
 * Fan:       pwm1 = fan duty 0-255
 * Intrusion: intrusion0_alarm = any active fault,
 *            intrusion1_alarm = any logged fault
 * Extra:     fault_status_raw, fault_log_raw = raw fault bitmasks
 */

/*
 * fault_status bits, numbered like the GUI's FAULT enum (bit index = enum
 * value), and the alarm attributes each one raises:
 *
 *   bit  fault                    meaning                  alarms
 *   0    FAULT_OTP_TCHIP          chip over-temperature    temp1, temp2
 *   1    FAULT_OTP_TS             sensor over-temperature  temp3, temp4
 *   2    FAULT_OCP                over-current (total)     curr7
 *   3    FAULT_WIRE_OCP           per-wire over-current    curr1-curr6
 *   4    FAULT_OPP                over-power               power1
 *   5    FAULT_CURRENT_IMBALANCE  current imbalance        curr1-curr7
 *
 * The device does not say which wire tripped FAULT_WIRE_OCP, so all six
 * per-pin alarms report it. Alarms follow fault_status (active faults);
 * the fault log is only exposed through intrusion1 and fault_log_raw.
 */
#define WIREVIEW_FAULT_OTP_TCHIP		BIT(0)
#define WIREVIEW_FAULT_OTP_TS			BIT(1)
#define WIREVIEW_FAULT_OCP			BIT(2)
#define WIREVIEW_FAULT_WIRE_OCP			BIT(3)
#define WIREVIEW_FAULT_OPP			BIT(4)
#define WIREVIEW_FAULT_CURRENT_IMBALANCE	BIT(5)

/* power1_cap in microwatts, indexed by the device's psu_cap enum */
static const long psu_cap_uw[] = {
	600000000,	/* 0: 600 W */
	450000000,	/* 1: 450 W */
	300000000,	/* 2: 300 W */
	150000000,	/* 3: 150 W */
};

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

static const char * const energy_labels[] = {
	"Total"
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

enum wireview_raw_attr {
	WIREVIEW_RAW_FAULT_STATUS,
	WIREVIEW_RAW_FAULT_LOG,
};

static ssize_t wireview_raw_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct wireview_priv *priv = dev_get_drvdata(dev);
	unsigned int val;

	mutex_lock(&priv->lock);
	if (!wireview_data_fresh(priv)) {
		mutex_unlock(&priv->lock);
		return -ENODATA;
	}

	if (to_sensor_dev_attr(attr)->index == WIREVIEW_RAW_FAULT_STATUS)
		val = priv->data.fault_status;
	else /* WIREVIEW_RAW_FAULT_LOG */
		val = priv->data.fault_log;
	mutex_unlock(&priv->lock);

	return sysfs_emit(buf, "%u\n", val);
}

static DEVICE_ATTR_RO(intrusion0_label);
static DEVICE_ATTR_RO(intrusion1_label);
static SENSOR_DEVICE_ATTR_RO(fault_status_raw, wireview_raw,
			     WIREVIEW_RAW_FAULT_STATUS);
static SENSOR_DEVICE_ATTR_RO(fault_log_raw, wireview_raw, WIREVIEW_RAW_FAULT_LOG);

static struct attribute *wireview_extra_attrs[] = {
	&dev_attr_intrusion0_label.attr,
	&dev_attr_intrusion1_label.attr,
	&sensor_dev_attr_fault_status_raw.dev_attr.attr,
	&sensor_dev_attr_fault_log_raw.dev_attr.attr,
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
		if (attr == hwmon_curr_input || attr == hwmon_curr_label ||
		    attr == hwmon_curr_alarm)
			return 0444;
		break;
	case hwmon_power:
		if (attr == hwmon_power_input || attr == hwmon_power_label ||
		    attr == hwmon_power_cap || attr == hwmon_power_alarm)
			return 0444;
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_input || attr == hwmon_temp_label ||
		    attr == hwmon_temp_alarm)
			return 0444;
		break;
	case hwmon_energy:
		if (attr == hwmon_energy_input || attr == hwmon_energy_label)
			return 0444;
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_input)
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

/* The fault_status bits that raise the alarm of this channel. */
static u16 wireview_alarm_mask(enum hwmon_sensor_types type, int channel)
{
	switch (type) {
	case hwmon_temp:
		/* temp1/temp2 onboard, temp3/temp4 external sensors */
		return channel < 2 ? WIREVIEW_FAULT_OTP_TCHIP :
				     WIREVIEW_FAULT_OTP_TS;
	case hwmon_curr:
		/* curr1-curr6 per pin, curr7 total */
		return (channel < 6 ? WIREVIEW_FAULT_WIRE_OCP :
				      WIREVIEW_FAULT_OCP) |
		       WIREVIEW_FAULT_CURRENT_IMBALANCE;
	case hwmon_power:
		return WIREVIEW_FAULT_OPP;	/* power1 only */
	default:
		return 0;
	}
}

static int wireview_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct wireview_priv *priv = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&priv->lock);

	if (!wireview_data_fresh(priv)) {
		mutex_unlock(&priv->lock);
		return -ENODATA;
	}

	if ((type == hwmon_temp && attr == hwmon_temp_alarm) ||
	    (type == hwmon_curr && attr == hwmon_curr_alarm) ||
	    (type == hwmon_power && attr == hwmon_power_alarm)) {
		*val = !!(priv->data.fault_status &
			  wireview_alarm_mask(type, channel));
		goto out;
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
		if (attr == hwmon_power_cap) {
			if (priv->data.psu_cap < ARRAY_SIZE(psu_cap_uw))
				*val = psu_cap_uw[priv->data.psu_cap];
			else
				ret = -ENODATA;
		} else if (channel == 0) {
			*val = priv->data.total_power_uw;
		} else { /* channels 1-6 = power2-power7 */
			*val = priv->data.pin_power_uw[channel - 1];
		}
		break;
	case hwmon_temp:
		if (priv->data.temp_mc[channel] == S32_MIN)
			ret = -ENODATA;
		else
			*val = priv->data.temp_mc[channel];
		break;
	case hwmon_energy:
		/* long is 32 bits on 32-bit kernels: saturate, don't wrap */
		if (priv->data.energy_uj < 0)
			ret = -ENODATA;
		else
			*val = min_t(s64, priv->data.energy_uj, LONG_MAX);
		break;
	case hwmon_pwm:
		*val = DIV_ROUND_CLOSEST(min_t(unsigned int,
					       priv->data.fan_duty, 100) * 255,
					 100);
		break;
	case hwmon_intrusion:
		if (channel == 0)
			*val = priv->data.fault_status != 0 ? 1 : 0;
		else
			*val = priv->data.fault_log != 0 ? 1 : 0;
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

out:
	mutex_unlock(&priv->lock);
	return ret;
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
	case hwmon_energy:
		*str = energy_labels[channel];
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
		HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_ALARM,   /* curr1: Pin 1 */
		HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_ALARM,   /* curr2: Pin 2 */
		HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_ALARM,   /* curr3: Pin 3 */
		HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_ALARM,   /* curr4: Pin 4 */
		HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_ALARM,   /* curr5: Pin 5 */
		HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_ALARM,   /* curr6: Pin 6 */
		HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_ALARM),  /* curr7: Total */
	HWMON_CHANNEL_INFO(power,
		HWMON_P_INPUT | HWMON_P_LABEL |
		HWMON_P_CAP | HWMON_P_ALARM,     /* power1: Total */
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power2: Pin 1 */
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power3: Pin 2 */
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power4: Pin 3 */
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power5: Pin 4 */
		HWMON_P_INPUT | HWMON_P_LABEL,   /* power6: Pin 5 */
		HWMON_P_INPUT | HWMON_P_LABEL),  /* power7: Pin 6 */
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_ALARM,   /* temp1: Onboard In */
		HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_ALARM,   /* temp2: Onboard Out */
		HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_ALARM,   /* temp3: External 1 */
		HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_ALARM),  /* temp4: External 2 */
	HWMON_CHANNEL_INFO(energy,
		HWMON_E_INPUT | HWMON_E_LABEL),  /* energy1: Total */
	HWMON_CHANNEL_INFO(pwm,
		HWMON_PWM_INPUT),                /* pwm1: duty 0-255 */
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

	/*
	 * hwmon first: until misc_register() nothing can write a frame, and
	 * the hwmon callbacks only need the lock and data_valid (false), both
	 * set up above. The misc device goes last because a write can arrive
	 * the moment it is registered.
	 */
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

	misc_deregister(&priv->misc);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
}

static struct platform_driver wireview_driver = {
	.driver = {
		.name = "wireview_hwmon",
		/*
		 * An open /dev/wireview-hwmon holds a pointer into priv and pins
		 * the module, but not the binding: a sysfs unbind would free
		 * priv under the daemon's open file. Only module unload may
		 * remove the device.
		 */
		.suppress_bind_attrs = true,
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
MODULE_VERSION(WIREVIEW_PKG_VERSION);
MODULE_AUTHOR("WireView Linux Project");
MODULE_DESCRIPTION("Virtual hwmon driver for WireView Pro II power monitor");
