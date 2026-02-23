/*
 * wireviewd - Daemon for WireView Pro II hwmon integration
 *
 * Reads sensor data from the WireView Pro II device over serial and
 * writes it to /dev/wireview-hwmon for the wireview_hwmon kernel module.
 *
 * Usage: wireviewd [-i interval_ms] [-d device_path]
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <termios.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#define WIREVIEW_VID "0483"
#define WIREVIEW_PID "5740"
#define HWMON_DEV    "/dev/wireview-hwmon"
#define WELCOME_MSG  "Thermal Grizzly WireView Pro II"

#define WIREVIEW_MAGIC   0x57565032
#define WIREVIEW_VERSION 2

#define CMD_READ_SENSOR_VALUES 0x04

static volatile int running = 1;

/* Binary struct written to /dev/wireview-hwmon (must match kernel module) */
struct __attribute__((packed)) hwmon_data {
	uint32_t magic;
	uint32_t version;
	int32_t  voltage_mv[6];       /* per-pin voltages (mV) */
	int32_t  current_ma[6];       /* per-pin currents (mA) */
	int64_t  total_power_uw;      /* total power (uW) */
	int32_t  temp_mc[4];          /* temperatures (millidegrees C) */
	int64_t  pin_power_uw[6];     /* per-pin power (uW) */
	int32_t  total_current_ma;    /* total current (mA) */
	int32_t  avg_voltage_mv;      /* average voltage (mV) */
	int32_t  vdd_mv;              /* supply voltage (mV) */
	uint8_t  fan_duty;            /* fan duty 0-100% */
	uint8_t  psu_cap;             /* PSU capability enum */
	uint16_t fault_status;        /* active fault bitmask */
	uint16_t fault_log;           /* historical fault bitmask */
	uint16_t _pad;
};

_Static_assert(sizeof(struct hwmon_data) == 148, "hwmon_data size mismatch");

/*
 * SensorStruct from the WireView firmware (Pack=4, little-endian).
 *
 * PowerSensor is 10 bytes of data but with Pack=4 alignment becomes 12 bytes.
 */
struct __attribute__((packed)) power_sensor {
	int16_t  voltage;    /* mV */
	uint16_t _pad;
	uint32_t current;    /* mA */
	uint32_t power;      /* mW */
};

struct __attribute__((packed)) sensor_struct {
	int16_t  ts[4];      /* temperatures in 0.1 degC */
	uint16_t vdd;        /* supply voltage mV */
	uint8_t  fan_duty;   /* fan duty % */
	uint8_t  _pad1;
	struct power_sensor pins[6];
	uint32_t total_power;   /* mW */
	uint32_t total_current; /* mA */
	uint16_t avg_voltage;   /* mV */
	uint8_t  hpwr_cap;
	uint8_t  _pad2;
	uint16_t fault_status;
	uint16_t fault_log;
};

static void sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

/* Read exactly n bytes from fd with timeout. Returns 0 on success, -1 on failure. */
static int read_exact(int fd, void *buf, size_t n, int timeout_ms)
{
	size_t off = 0;
	struct timespec start, now;

	clock_gettime(CLOCK_MONOTONIC, &start);

	while (off < n) {
		ssize_t r = read(fd, (char *)buf + off, n - off);
		if (r > 0) {
			off += r;
		} else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
			return -1;
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
			       (now.tv_nsec - start.tv_nsec) / 1000000;
		if (elapsed > timeout_ms)
			return -1;

		if (off < n)
			usleep(1000);
	}
	return 0;
}

/* Find WireView device by scanning /sys/class/tty/ttyACM* */
static int find_device(char *path, size_t path_len)
{
	DIR *d = opendir("/sys/class/tty");
	struct dirent *ent;
	char sysdir[1024], resolved[1024], check[1024], vid[16], pid[16];
	ssize_t len;

	if (!d)
		return -1;

	while ((ent = readdir(d)) != NULL) {
		if (strncmp(ent->d_name, "ttyACM", 6) != 0)
			continue;

		snprintf(sysdir, sizeof(sysdir), "/sys/class/tty/%s", ent->d_name);
		len = readlink(sysdir, resolved, sizeof(resolved) - 1);
		if (len < 0)
			continue;
		resolved[len] = '\0';

		/* Make absolute path */
		if (resolved[0] != '/') {
			char tmp[1024];
			snprintf(tmp, sizeof(tmp), "/sys/class/tty/%s", resolved);
			char *rp = realpath(tmp, resolved);
			if (!rp)
				continue;
		}

		/* Walk up sysfs to find idVendor/idProduct */
		char *p = resolved;
		while (p && *p && strcmp(p, "/") != 0) {
			FILE *f;

			snprintf(check, sizeof(check), "%s/idVendor", p);
			f = fopen(check, "r");
			if (f) {
				if (fgets(vid, sizeof(vid), f))
					vid[strcspn(vid, "\n")] = '\0';
				fclose(f);

				snprintf(check, sizeof(check), "%s/idProduct", p);
				f = fopen(check, "r");
				if (f) {
					if (fgets(pid, sizeof(pid), f))
						pid[strcspn(pid, "\n")] = '\0';
					fclose(f);

					if (strcasecmp(vid, WIREVIEW_VID) == 0 &&
					    strcasecmp(pid, WIREVIEW_PID) == 0) {
						snprintf(path, path_len, "/dev/%s",
							 ent->d_name);
						closedir(d);
						return 0;
					}
				}
			}

			/* Move up one directory */
			char *slash = strrchr(p, '/');
			if (slash && slash != p)
				*slash = '\0';
			else
				break;
		}
	}

	closedir(d);
	return -1;
}

/* Open serial port with WireView settings: 115200 8N1 */
static int open_serial(const char *path)
{
	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -1;

	/* Clear non-blocking for normal I/O */
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

	struct termios tio;
	if (tcgetattr(fd, &tio) < 0) {
		close(fd);
		return -1;
	}

	cfmakeraw(&tio);
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS8;
	tio.c_cflag &= ~(PARENB | CSTOPB);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 10; /* 1 second timeout */

	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		close(fd);
		return -1;
	}

	tcflush(fd, TCIOFLUSH);
	return fd;
}

/* Read sensor data from the device */
static int read_sensors(int fd, struct sensor_struct *ss)
{
	uint8_t cmd = CMD_READ_SENSOR_VALUES;

	tcflush(fd, TCIFLUSH);

	if (write(fd, &cmd, 1) != 1)
		return -1;

	if (read_exact(fd, ss, sizeof(*ss), 1000) < 0)
		return -1;

	return 0;
}

/* Convert sensor data to hwmon format and write to kernel module */
static int write_hwmon(int hwmon_fd, const struct sensor_struct *ss)
{
	struct hwmon_data hd;
	int64_t power_sum = 0;
	int i;

	memset(&hd, 0, sizeof(hd));
	hd.magic = WIREVIEW_MAGIC;
	hd.version = WIREVIEW_VERSION;

	for (i = 0; i < 6; i++) {
		hd.voltage_mv[i] = ss->pins[i].voltage;
		hd.current_ma[i] = (int32_t)ss->pins[i].current;
		/* mV * mA = microwatts */
		hd.pin_power_uw[i] = (int64_t)ss->pins[i].voltage *
				     (int64_t)ss->pins[i].current;
		power_sum += hd.pin_power_uw[i];
	}
	hd.total_power_uw = power_sum;

	for (i = 0; i < 4; i++) {
		int16_t raw = ss->ts[i];
		/* 0.1 degC -> millidegrees: multiply by 100 */
		/* Invalid temps (disconnected sensor) read as ~-32768 */
		if (raw < -1000 || raw > 2000)
			hd.temp_mc[i] = 0;
		else
			hd.temp_mc[i] = (int32_t)raw * 100;
	}

	hd.total_current_ma = (int32_t)ss->total_current;
	hd.avg_voltage_mv = (int32_t)ss->avg_voltage;
	hd.vdd_mv = (int32_t)ss->vdd;
	hd.fan_duty = ss->fan_duty;
	hd.psu_cap = ss->hpwr_cap;
	hd.fault_status = ss->fault_status;
	hd.fault_log = ss->fault_log;

	if (write(hwmon_fd, &hd, sizeof(hd)) != sizeof(hd))
		return -1;

	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-i interval_ms] [-d /dev/ttyACMx]\n", prog);
	fprintf(stderr, "  -i  Poll interval in milliseconds (default: 1000)\n");
	fprintf(stderr, "  -d  Serial device path (default: auto-detect)\n");
	exit(1);
}

int main(int argc, char **argv)
{
	int interval_ms = 1000;
	char dev_path[256] = "";
	int opt;

	while ((opt = getopt(argc, argv, "i:d:h")) != -1) {
		switch (opt) {
		case 'i':
			interval_ms = atoi(optarg);
			if (interval_ms < 100 || interval_ms > 10000) {
				fprintf(stderr, "Interval must be 100-10000 ms\n");
				return 1;
			}
			break;
		case 'd':
			snprintf(dev_path, sizeof(dev_path), "%s", optarg);
			break;
		default:
			usage(argv[0]);
		}
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	printf("wireviewd: starting\n");

	while (running) {
		int serial_fd = -1;
		int hwmon_fd = -1;

		/* Find device if not specified */
		if (dev_path[0] == '\0') {
			if (find_device(dev_path, sizeof(dev_path)) < 0) {
				fprintf(stderr, "wireviewd: device not found, retrying in 5s\n");
				sleep(5);
				dev_path[0] = '\0';
				continue;
			}
		}

		/* Check hwmon module is loaded */
		if (access(HWMON_DEV, W_OK) < 0) {
			fprintf(stderr, "wireviewd: %s not available (load wireview_hwmon module)\n",
				HWMON_DEV);
			sleep(5);
			continue;
		}

		printf("wireviewd: using %s\n", dev_path);

		serial_fd = open_serial(dev_path);
		if (serial_fd < 0) {
			fprintf(stderr, "wireviewd: failed to open %s: %s\n",
				dev_path, strerror(errno));
			dev_path[0] = '\0';
			sleep(5);
			continue;
		}

		hwmon_fd = open(HWMON_DEV, O_WRONLY);
		if (hwmon_fd < 0) {
			fprintf(stderr, "wireviewd: failed to open %s: %s\n",
				HWMON_DEV, strerror(errno));
			close(serial_fd);
			sleep(5);
			continue;
		}

		printf("wireviewd: polling every %d ms\n", interval_ms);

		while (running) {
			struct sensor_struct ss;

			if (read_sensors(serial_fd, &ss) < 0) {
				fprintf(stderr, "wireviewd: read failed, reconnecting\n");
				break;
			}

			if (write_hwmon(hwmon_fd, &ss) < 0) {
				fprintf(stderr, "wireviewd: hwmon write failed\n");
				break;
			}

			usleep(interval_ms * 1000);
		}

		close(hwmon_fd);
		close(serial_fd);
		dev_path[0] = '\0';

		if (running)
			sleep(2);
	}

	printf("wireviewd: stopped\n");
	return 0;
}
