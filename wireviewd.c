/*
 * wireviewd - Daemon for WireView Pro II hwmon integration
 *
 * Reads sensor data from the WireView Pro II device over serial and
 * writes it to /dev/wireview-hwmon for the wireview_hwmon kernel module.
 * Also exposes a Unix socket for bidirectional command relay from apps.
 *
 * Usage: wireviewd [-i interval_ms] [-d device_path]
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#define _GNU_SOURCE
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
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <linux/limits.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdarg.h>
#include "sha256.h"

#define WIREVIEW_VID "0483"
#define WIREVIEW_PID "5740"
#define HWMON_DEV    "/dev/wireview-hwmon"
#define SOCK_PATH    "/run/wireviewd.sock"
#define HTTP_PORT    9876

#define WIREVIEW_MAGIC   0x57565032
#define WIREVIEW_VERSION 2

#define MAX_CLIENTS 4

/* Firmware command bytes */
#define CMD_READ_VENDOR_DATA   0x01
#define CMD_READ_UID           0x02
#define CMD_READ_SENSOR_VALUES 0x04
#define CMD_READ_CONFIG        0x05
#define CMD_WRITE_CONFIG       0x06
#define CMD_SCREEN_CHANGE      0x0C
#define CMD_READ_BUILD_INFO    0x0D
#define CMD_CLEAR_FAULTS       0x0E
#define CMD_BOOTLOADER         0xF1
#define CMD_NVM_CONFIG         0xF2

/* Socket protocol command types */
#define WCMD_GET_DEVICE_INFO   0x01
#define WCMD_CLEAR_FAULTS      0x02
#define WCMD_READ_CONFIG       0x03
#define WCMD_WRITE_CONFIG      0x04
#define WCMD_SCREEN_CMD        0x05
#define WCMD_NVM_CMD           0x06
#define WCMD_READ_BUILD        0x07
#define WCMD_ENTER_BOOTLOADER  0x08

/* Response status */
#define RESP_OK            0
#define RESP_ERROR         1
#define RESP_NOT_CONNECTED 2

static volatile int running = 1;

/* Binary struct written to /dev/wireview-hwmon (must match kernel module) */
struct __attribute__((packed)) hwmon_data {
	uint32_t magic;
	uint32_t version;
	int32_t  voltage_mv[6];
	int32_t  current_ma[6];
	int64_t  total_power_uw;
	int32_t  temp_mc[4];
	int64_t  pin_power_uw[6];
	int32_t  total_current_ma;
	int32_t  avg_voltage_mv;
	int32_t  vdd_mv;
	uint8_t  fan_duty;
	uint8_t  psu_cap;
	uint16_t fault_status;
	uint16_t fault_log;
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

/* Device info read once on connect */
struct device_info {
	uint8_t  fw_version;
	uint8_t  config_version;  /* 0 if fw<=2, 1 if fw>2 */
	uint8_t  uid[12];
	char     build_string[64];
	int      valid;
};

static struct device_info dev_info;

/* Latest sensor frame, cached for the HTTP /sensors publisher. */
static struct sensor_struct g_last;
static int g_have_last;

/* Write-command auth + relay state for the HTTP POST /command endpoint. */
static char g_secret[128];      /* shared HMAC secret; empty => writes disabled */
static int  g_serial_fd = -1;   /* current serial fd, for HTTP command relay */
static int  g_http_enabled = 0; /* network listener off unless config enables it */
static int  g_log_retain_days = 14; /* days of audit logs to keep (config: log_days=) */

#define HTTP_MAX_BODY    8192
#define HTTP_AUTH_WINDOW 30     /* seconds of timestamp skew tolerated */

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
	char sysdir[PATH_MAX], resolved[PATH_MAX], check[PATH_MAX], vid[16], pid[16];
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
			char tmp[PATH_MAX];
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
		hd.pin_power_uw[i] = (int64_t)ss->pins[i].voltage *
				     (int64_t)ss->pins[i].current;
		power_sum += hd.pin_power_uw[i];
	}
	hd.total_power_uw = power_sum;

	for (i = 0; i < 4; i++) {
		int16_t raw = ss->ts[i];
		/* Disconnected sensors report out-of-range values */
		if (raw < -400 || raw > 2000)
			hd.temp_mc[i] = INT32_MIN;
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

/* ---- Device info queries ---- */

static int query_device_info(int serial_fd)
{
	uint8_t cmd, buf[128];

	memset(&dev_info, 0, sizeof(dev_info));

	/* CMD_READ_VENDOR_DATA (0x01): VendorDataStruct = 3 bytes
	 * (3 byte fields, Pack=4 doesn't pad since max field alignment is 1) */
	cmd = CMD_READ_VENDOR_DATA;
	tcflush(serial_fd, TCIFLUSH);
	if (write(serial_fd, &cmd, 1) != 1) return -1;
	if (read_exact(serial_fd, buf, 3, 1000) < 0) return -1;

	if (buf[0] != 0xEF || buf[1] != 0x05)
		return -1;

	dev_info.fw_version = buf[2];

	/* CMD_READ_UID (0x02): 12 bytes */
	cmd = CMD_READ_UID;
	tcflush(serial_fd, TCIFLUSH);
	if (write(serial_fd, &cmd, 1) != 1) return -1;
	if (read_exact(serial_fd, dev_info.uid, 12, 1000) < 0) return -1;

	/* CMD_READ_BUILD_INFO (0x0D): BuildStruct with Pack=4
	 * VendorData(3) + ProductName(32) + BuildInfo(32) + ProductNameLen(1) = 68 */
	cmd = CMD_READ_BUILD_INFO;
	tcflush(serial_fd, TCIFLUSH);
	if (write(serial_fd, &cmd, 1) != 1) goto done;
	if (read_exact(serial_fd, buf, 68, 1000) < 0) goto done;
	/* BuildInfo starts at offset 35 (3 + 32), 32 bytes max */
	memcpy(dev_info.build_string, buf + 35, 32);
	dev_info.build_string[32] = '\0';

	/* Read config version from the config struct's Version field (byte 2).
	 * Send CMD_READ_CONFIG, read first 4 bytes, extract version. */
	cmd = CMD_READ_CONFIG;
	tcflush(serial_fd, TCIFLUSH);
	if (write(serial_fd, &cmd, 1) != 1) goto done;
	if (read_exact(serial_fd, buf, 4, 1000) < 0) goto done;
	dev_info.config_version = buf[2];

done:
	dev_info.valid = 1;
	printf("wireviewd: FW v%d, config v%d\n",
	       dev_info.fw_version, dev_info.config_version);
	return 0;
}

/* ---- Unix socket ---- */

static int setup_socket(void)
{
	int fd;
	struct sockaddr_un addr;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) return -1;

	unlink(SOCK_PATH);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	chmod(SOCK_PATH, 0666);

	if (listen(fd, MAX_CLIENTS) < 0) {
		close(fd);
		unlink(SOCK_PATH);
		return -1;
	}

	return fd;
}

static void cleanup_socket(int sock_fd)
{
	if (sock_fd >= 0) close(sock_fd);
	unlink(SOCK_PATH);
}

static int send_response(int client_fd, uint8_t status,
			 const void *payload, uint16_t payload_len)
{
	uint8_t hdr[3];
	hdr[0] = status;
	hdr[1] = payload_len & 0xFF;
	hdr[2] = (payload_len >> 8) & 0xFF;

	if (write(client_fd, hdr, 3) != 3) return -1;
	if (payload_len > 0 && payload) {
		if (write(client_fd, payload, payload_len) != payload_len)
			return -1;
	}
	return 0;
}

static void handle_client_request(int client_fd, int serial_fd)
{
	uint8_t hdr[3];
	uint8_t cmd_type;
	uint16_t payload_len;
	uint8_t payload[512];

	if (read_exact(client_fd, hdr, 3, 2000) < 0) return;

	cmd_type = hdr[0];
	payload_len = hdr[1] | ((uint16_t)hdr[2] << 8);

	if (payload_len > sizeof(payload)) {
		send_response(client_fd, RESP_ERROR, NULL, 0);
		return;
	}

	if (payload_len > 0) {
		if (read_exact(client_fd, payload, payload_len, 2000) < 0)
			return;
	}

	if (serial_fd < 0) {
		send_response(client_fd, RESP_NOT_CONNECTED, NULL, 0);
		return;
	}

	switch (cmd_type) {
	case WCMD_GET_DEVICE_INFO: {
		if (!dev_info.valid) {
			send_response(client_fd, RESP_NOT_CONNECTED, NULL, 0);
			return;
		}
		uint8_t resp[2 + 12 + 64];
		int resp_len = 0;
		resp[resp_len++] = dev_info.fw_version;
		resp[resp_len++] = dev_info.config_version;
		memcpy(resp + resp_len, dev_info.uid, 12);
		resp_len += 12;
		int blen = (int)strlen(dev_info.build_string) + 1;
		memcpy(resp + resp_len, dev_info.build_string, blen);
		resp_len += blen;
		send_response(client_fd, RESP_OK, resp, (uint16_t)resp_len);
		break;
	}

	case WCMD_CLEAR_FAULTS: {
		if (payload_len < 4) {
			send_response(client_fd, RESP_ERROR, NULL, 0);
			return;
		}
		uint8_t cmd[5];
		cmd[0] = CMD_CLEAR_FAULTS;
		memcpy(cmd + 1, payload, 4);
		tcflush(serial_fd, TCIFLUSH);
		if (write(serial_fd, cmd, 5) == 5)
			send_response(client_fd, RESP_OK, NULL, 0);
		else
			send_response(client_fd, RESP_ERROR, NULL, 0);
		break;
	}

	case WCMD_READ_CONFIG: {
		int config_size;
		if (payload_len >= 2)
			config_size = payload[0] | ((int)payload[1] << 8);
		else
			config_size = dev_info.config_version == 0 ? 72 :
				      dev_info.config_version == 1 ? 74 : 96;

		if (config_size > 512 || config_size < 1) {
			send_response(client_fd, RESP_ERROR, NULL, 0);
			break;
		}

		uint8_t cmd = CMD_READ_CONFIG;
		uint8_t resp[1 + 512];
		tcflush(serial_fd, TCIFLUSH);
		if (write(serial_fd, &cmd, 1) != 1) {
			send_response(client_fd, RESP_ERROR, NULL, 0);
			break;
		}
		if (read_exact(serial_fd, resp + 1, config_size, 2000) < 0) {
			send_response(client_fd, RESP_ERROR, NULL, 0);
			break;
		}
		resp[0] = dev_info.config_version;
		send_response(client_fd, RESP_OK, resp, (uint16_t)(1 + config_size));
		break;
	}

	case WCMD_WRITE_CONFIG: {
		if (payload_len < 2) {
			send_response(client_fd, RESP_ERROR, NULL, 0);
			break;
		}
		/* payload[0] = config_version, payload[1..] = raw config bytes */
		int data_len = payload_len - 1;
		uint8_t frame[64];
		frame[0] = CMD_WRITE_CONFIG;

		tcflush(serial_fd, TCIFLUSH);

		int ok = 1;
		for (int offset = 0; offset < data_len && offset <= 255; offset += 62) {
			int bytes_to_write = data_len - offset;
			if (bytes_to_write > 62) bytes_to_write = 62;

			frame[1] = (uint8_t)offset;
			memcpy(frame + 2, payload + 1 + offset, bytes_to_write);

			if (write(serial_fd, frame, bytes_to_write + 2) !=
			    bytes_to_write + 2) {
				ok = 0;
				break;
			}
		}
		send_response(client_fd, ok ? RESP_OK : RESP_ERROR, NULL, 0);
		break;
	}

	case WCMD_SCREEN_CMD: {
		if (payload_len < 1) {
			send_response(client_fd, RESP_ERROR, NULL, 0);
			break;
		}
		uint8_t cmd[2] = { CMD_SCREEN_CHANGE, payload[0] };
		tcflush(serial_fd, TCIFLUSH);
		if (write(serial_fd, cmd, 2) == 2)
			send_response(client_fd, RESP_OK, NULL, 0);
		else
			send_response(client_fd, RESP_ERROR, NULL, 0);
		break;
	}

	case WCMD_NVM_CMD: {
		if (payload_len < 1) {
			send_response(client_fd, RESP_ERROR, NULL, 0);
			break;
		}
		uint8_t cmd[6] = { CMD_NVM_CONFIG, 0x55, 0xAA, 0x55, 0xAA,
				   payload[0] };
		tcflush(serial_fd, TCIFLUSH);
		if (write(serial_fd, cmd, 6) == 6)
			send_response(client_fd, RESP_OK, NULL, 0);
		else
			send_response(client_fd, RESP_ERROR, NULL, 0);
		break;
	}

	case WCMD_READ_BUILD: {
		int blen = (int)strlen(dev_info.build_string) + 1;
		send_response(client_fd, RESP_OK, dev_info.build_string,
			      (uint16_t)blen);
		break;
	}

	case WCMD_ENTER_BOOTLOADER: {
		uint8_t cmd = CMD_BOOTLOADER;
		tcflush(serial_fd, TCIFLUSH);
		if (write(serial_fd, &cmd, 1) == 1)
			send_response(client_fd, RESP_OK, NULL, 0);
		else
			send_response(client_fd, RESP_ERROR, NULL, 0);
		break;
	}

	default:
		send_response(client_fd, RESP_ERROR, NULL, 0);
		break;
	}
}

/* ---- HTTP /sensors publisher (read-only LAN exposure) ---- */

static int psu_cap_watts(uint8_t cap)
{
	switch (cap) {
	case 0: return 600;
	case 1: return 450;
	case 2: return 300;
	case 3: return 150;
	default: return 0;
	}
}

/*
 * Build the GET /sensors JSON body. Matches the WireViewSensorDto schema the
 * desktop app consumes (camelCase; UID is uppercase hex to match the app).
 * This daemon manages a single device.
 */
static int build_sensors_json(char *out, size_t cap)
{
	char host[64] = "wireview";
	gethostname(host, sizeof(host) - 1);

	char ts[32];
	time_t now = time(NULL);
	struct tm tmv;
	gmtime_r(&now, &tmv);
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);

	/* No device / no frame yet -> empty list so consumers drop it. */
	if (!g_have_last || !dev_info.valid)
		return snprintf(out, cap,
			"{\"host\":\"%s\",\"appVersion\":\"wireviewd\",\"devices\":[]}",
			host);

	char uid[25];
	for (int i = 0; i < 12; i++)
		snprintf(uid + i * 2, 3, "%02X", dev_info.uid[i]);

	double pv[6], pc[6], sum_p = 0, sum_c = 0;
	for (int i = 0; i < 6; i++) {
		pv[i] = g_last.pins[i].voltage / 1000.0;
		pc[i] = g_last.pins[i].current / 1000.0;
		sum_p += pv[i] * pc[i];
		sum_c += pc[i];
	}
	double t[4];
	for (int i = 0; i < 4; i++) {
		int16_t raw = g_last.ts[i];
		t[i] = (raw < -400 || raw > 2000) ? 0.0 : raw / 10.0;
	}

	return snprintf(out, cap,
		"{\"host\":\"%s\",\"appVersion\":\"wireviewd\",\"devices\":[{"
		"\"id\":\"%s\",\"name\":\"WireView Pro II\",\"connected\":true,"
		"\"hwRev\":\"\",\"fwVer\":\"%d\",\"buildString\":\"%s\",\"timestamp\":\"%s\","
		"\"pinVoltage\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f],"
		"\"pinCurrent\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f],"
		"\"tempInC\":%.1f,\"tempOutC\":%.1f,\"ext1C\":%.1f,\"ext2C\":%.1f,"
		"\"psuCapW\":%d,\"fan\":%d,\"faultStatus\":%u,\"faultLog\":%u,"
		"\"sumCurrentA\":%.3f,\"sumPowerW\":%.3f}]}",
		host, uid, dev_info.fw_version, dev_info.build_string, ts,
		pv[0], pv[1], pv[2], pv[3], pv[4], pv[5],
		pc[0], pc[1], pc[2], pc[3], pc[4], pc[5],
		t[0], t[1], t[2], t[3],
		psu_cap_watts(g_last.hpwr_cap), g_last.fan_duty,
		g_last.fault_status, g_last.fault_log,
		sum_c, sum_p);
}

static int setup_http(void)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) return -1;

	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(HTTP_PORT);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	if (listen(fd, 8) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* ---- Daily-rotating audit log ---- */
#define LOG_DIR "/var/log/wireview"

static FILE *g_logf;
static char  g_log_day[11];   /* YYYY-MM-DD of the currently open file */

/* Remove wireviewd-YYYY-MM-DD.log files older than g_log_retain_days (0 = keep all). */
static void prune_old_logs(void)
{
	if (g_log_retain_days <= 0) return;
	time_t cutoff_t = time(NULL) - (time_t)g_log_retain_days * 86400;
	struct tm tmv;
	char cutoff[11];
	localtime_r(&cutoff_t, &tmv);
	strftime(cutoff, sizeof(cutoff), "%Y-%m-%d", &tmv);

	DIR *d = opendir(LOG_DIR);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "wireviewd-", 10) != 0) continue;
		if (strncmp(e->d_name + 10, cutoff, 10) < 0) {
			char path[300];
			snprintf(path, sizeof(path), "%s/%s", LOG_DIR, e->d_name);
			unlink(path);
		}
	}
	closedir(d);
}

/* Append a timestamped line to today's log, rotating to a new file each day. */
static void wlog(const char *level, const char *fmt, ...)
{
	time_t now = time(NULL);
	struct tm tmv;
	localtime_r(&now, &tmv);
	char day[11];
	strftime(day, sizeof(day), "%Y-%m-%d", &tmv);

	if (!g_logf || strcmp(day, g_log_day) != 0) {
		if (g_logf) fclose(g_logf);
		mkdir(LOG_DIR, 0750);
		char path[300];
		snprintf(path, sizeof(path), "%s/wireviewd-%s.log", LOG_DIR, day);
		g_logf = fopen(path, "a");
		snprintf(g_log_day, sizeof(g_log_day), "%s", day);
		prune_old_logs();
	}
	if (!g_logf) return;

	char tsb[32];
	strftime(tsb, sizeof(tsb), "%Y-%m-%dT%H:%M:%S", &tmv);
	fprintf(g_logf, "%s [%s] ", tsb, level);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_logf, fmt, ap);
	va_end(ap);
	fputc('\n', g_logf);
	fflush(g_logf);
}

/* ---- Write-command auth + relay (HTTP POST /command) ---- */

static int truthy(const char *s)
{
	return s && (s[0] == '1' || s[0] == 't' || s[0] == 'T' || s[0] == 'y' || s[0] == 'Y');
}

/* Load the network-listener flag and shared HMAC secret from /etc/wireview/config.
 * The file holds "key=value" lines: "remote_enabled=0|1" gates the listener (default
 * OFF, so no port is opened unless explicitly enabled) and "secret=<passphrase>" sets
 * the write secret; a bare line is taken as the secret (backward compatible).
 * $WIREVIEW_LISTEN and $WIREVIEW_SECRET override the file. An empty secret leaves
 * writes refused (403) even when the listener is on. */
static void load_config(void)
{
	g_secret[0] = '\0';
	g_http_enabled = 0;

	FILE *f = fopen("/etc/wireview/config", "r");
	if (f) {
		char line[192];
		while (fgets(line, sizeof(line), f)) {
			size_t n = strlen(line);
			while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
				     line[n - 1] == ' ' || line[n - 1] == '\t'))
				line[--n] = '\0';

			char *p = line;
			while (*p == ' ' || *p == '\t') p++;
			if (*p == '#' || *p == '\0') continue;

			char *eq = strchr(p, '=');
			if (eq) {
				*eq = '\0';
				char *val = eq + 1;
				while (*val == ' ' || *val == '\t') val++;
				if (strcmp(p, "remote_enabled") == 0)
					g_http_enabled = truthy(val);
				else if (strcmp(p, "secret") == 0 && !g_secret[0])
					snprintf(g_secret, sizeof(g_secret), "%s", val);
				else if (strcmp(p, "log_days") == 0)
					g_log_retain_days = atoi(val);
			} else if (!g_secret[0]) {
				snprintf(g_secret, sizeof(g_secret), "%s", p);
			}
		}
		fclose(f);
	}

	/* Environment overrides win over the file. */
	const char *env = getenv("WIREVIEW_SECRET");
	if (env && env[0])
		snprintf(g_secret, sizeof(g_secret), "%s", env);
	const char *listen = getenv("WIREVIEW_LISTEN");
	if (listen)
		g_http_enabled = truthy(listen);
}

/* Case-insensitive HTTP header value extraction. Returns 1 if found. */
static int http_header(const char *req, const char *name, char *out, size_t outsz)
{
	char key[64];
	snprintf(key, sizeof(key), "\r\n%s:", name);
	const char *p = strcasestr(req, key);
	if (!p) return 0;
	p += strlen(key);
	while (*p == ' ' || *p == '\t') p++;
	size_t i = 0;
	while (*p && *p != '\r' && *p != '\n' && i < outsz - 1)
		out[i++] = *p++;
	out[i] = '\0';
	return 1;
}

/* ---- tiny JSON readers for our flat, controlled command schema ---- */
static const char *json_find(const char *json, const char *key)
{
	char k[48];
	snprintf(k, sizeof(k), "\"%s\"", key);
	const char *p = strstr(json, k);
	if (!p) return NULL;
	p += strlen(k);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	return p;
}
static int json_str(const char *json, const char *key, char *out, size_t n)
{
	const char *p = json_find(json, key);
	if (!p || *p != '"') return 0;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i < n - 1) {
		if (*p == '\\' && p[1]) p++;
		out[i++] = *p++;
	}
	out[i] = '\0';
	return 1;
}
static int json_int(const char *json, const char *key, long *out)
{
	const char *p = json_find(json, key);
	if (!p) return 0;
	char *end;
	long v = strtol(p, &end, 10);
	if (end == p) return 0;
	*out = v;
	return 1;
}

static int b64val(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}
static int b64_decode(const char *in, uint8_t *out, size_t outcap)
{
	size_t outlen = 0;
	uint32_t acc = 0;
	int bits = 0;
	for (const char *p = in; *p; p++) {
		if (*p == '=' || *p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')
			continue;
		int v = b64val(*p);
		if (v < 0) return -1;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (outlen >= outcap) return -1;
			out[outlen++] = (uint8_t)(acc >> bits);
		}
	}
	return (int)outlen;
}

/* ---- serial relay helpers (mirror the WCMD_* socket handlers) ---- */
static int relay_screen(uint8_t cmd)
{
	uint8_t c[2] = { CMD_SCREEN_CHANGE, cmd };
	tcflush(g_serial_fd, TCIFLUSH);
	return write(g_serial_fd, c, 2) == 2;
}
static int relay_nvm(uint8_t cmd)
{
	uint8_t c[6] = { CMD_NVM_CONFIG, 0x55, 0xAA, 0x55, 0xAA, cmd };
	tcflush(g_serial_fd, TCIFLUSH);
	return write(g_serial_fd, c, 6) == 6;
}
static int relay_clear_faults(uint16_t status, uint16_t log)
{
	uint8_t c[5] = { CMD_CLEAR_FAULTS, (uint8_t)(status & 0xFF),
			 (uint8_t)(status >> 8), (uint8_t)(log & 0xFF),
			 (uint8_t)(log >> 8) };
	tcflush(g_serial_fd, TCIFLUSH);
	return write(g_serial_fd, c, 5) == 5;
}
static int relay_write_config(const uint8_t *data, int data_len)
{
	uint8_t frame[64];
	frame[0] = CMD_WRITE_CONFIG;
	tcflush(g_serial_fd, TCIFLUSH);
	for (int off = 0; off < data_len && off <= 255; off += 62) {
		int n = data_len - off;
		if (n > 62) n = 62;
		frame[1] = (uint8_t)off;
		memcpy(frame + 2, data + off, n);
		if (write(g_serial_fd, frame, n + 2) != n + 2)
			return 0;
	}
	return 1;
}

/* Read the device config over serial into cfg[] (needs >= 512). Sets *version
 * and returns the config byte length, or -1 on error. */
static int relay_read_config(uint8_t *cfg, uint8_t *version)
{
	int size = dev_info.config_version == 0 ? 72 :
		   dev_info.config_version == 1 ? 74 : 96;
	uint8_t cmd = CMD_READ_CONFIG;
	tcflush(g_serial_fd, TCIFLUSH);
	if (write(g_serial_fd, &cmd, 1) != 1)
		return -1;
	if (read_exact(g_serial_fd, cfg, size, 2000) < 0)
		return -1;
	*version = dev_info.config_version;
	return size;
}

static void b64_encode(const uint8_t *in, int len, char *out)
{
	static const char T[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i, o = 0;
	for (i = 0; i + 2 < len; i += 3) {
		uint32_t n = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
		out[o++] = T[(n >> 18) & 63]; out[o++] = T[(n >> 12) & 63];
		out[o++] = T[(n >> 6) & 63];  out[o++] = T[n & 63];
	}
	if (i < len) {
		uint32_t n = (uint32_t)in[i] << 16;
		if (i + 1 < len) n |= (uint32_t)in[i + 1] << 8;
		out[o++] = T[(n >> 18) & 63];
		out[o++] = T[(n >> 12) & 63];
		out[o++] = (i + 1 < len) ? T[(n >> 6) & 63] : '=';
		out[o++] = '=';
	}
	out[o] = '\0';
}

static void http_respond(int cfd, int status, const char *text, const char *body)
{
	char hdr[256];
	int blen = body ? (int)strlen(body) : 0;
	int hn = snprintf(hdr, sizeof(hdr),
		"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
		"Connection: close\r\nContent-Length: %d\r\n\r\n",
		status, text, blen);
	(void)!write(cfd, hdr, hn);
	if (blen) (void)!write(cfd, body, blen);
}

/* Bounded recent-nonce ring for replay rejection within the auth window. */
#define NONCE_RING 64
static struct { char nonce[64]; long ts; } g_nonces[NONCE_RING];
static int g_nonce_idx;
static int nonce_replay(const char *nonce, long now)
{
	for (int i = 0; i < NONCE_RING; i++)
		if (g_nonces[i].ts && now - g_nonces[i].ts <= HTTP_AUTH_WINDOW &&
		    strcmp(g_nonces[i].nonce, nonce) == 0)
			return 1;
	snprintf(g_nonces[g_nonce_idx].nonce, sizeof(g_nonces[0].nonce), "%s", nonce);
	g_nonces[g_nonce_idx].ts = now;
	g_nonce_idx = (g_nonce_idx + 1) % NONCE_RING;
	return 0;
}

/* Authenticated write: verify HMAC, then relay the command to the device. */
static void handle_post_command(int cfd, const char *req, const char *body,
				const char *client_ip)
{
	if (g_secret[0] == '\0') {
		wlog("WARN", "command from %s rejected: writes disabled (no secret)", client_ip);
		http_respond(cfd, 403, "Forbidden", "{\"error\":\"writes disabled\"}");
		return;
	}

	char ts[24] = {0}, nonce[48] = {0}, sig[80] = {0};
	if (!http_header(req, "X-Auth-Ts", ts, sizeof(ts)) ||
	    !http_header(req, "X-Auth-Nonce", nonce, sizeof(nonce)) ||
	    !http_header(req, "X-Auth-Sig", sig, sizeof(sig))) {
		wlog("WARN", "command from %s rejected: missing auth headers", client_ip);
		http_respond(cfd, 401, "Unauthorized", "{\"error\":\"missing auth\"}");
		return;
	}

	long t = strtol(ts, NULL, 10);
	long now = (long)time(NULL);
	if (now - t > HTTP_AUTH_WINDOW || t - now > HTTP_AUTH_WINDOW) {
		wlog("WARN", "command from %s rejected: stale timestamp", client_ip);
		http_respond(cfd, 401, "Unauthorized", "{\"error\":\"stale\"}");
		return;
	}

	char msg[HTTP_MAX_BODY + 128];
	int mlen = snprintf(msg, sizeof(msg), "%s\n%s\n%s", ts, nonce, body);
	uint8_t mac[32];
	char machex[65];
	hmac_sha256((uint8_t *)g_secret, strlen(g_secret),
		    (uint8_t *)msg, (size_t)mlen, mac);
	hex_encode(mac, 32, machex);
	if (!ct_str_equal(machex, sig)) {
		wlog("WARN", "command from %s rejected: bad signature", client_ip);
		http_respond(cfd, 401, "Unauthorized", "{\"error\":\"bad signature\"}");
		return;
	}
	if (nonce_replay(nonce, now)) {
		wlog("WARN", "command from %s rejected: replay", client_ip);
		http_respond(cfd, 401, "Unauthorized", "{\"error\":\"replay\"}");
		return;
	}

	if (g_serial_fd < 0) {
		http_respond(cfd, 503, "Service Unavailable", "{\"error\":\"no device\"}");
		return;
	}

	char op[24] = {0};
	if (!json_str(body, "op", op, sizeof(op))) {
		http_respond(cfd, 400, "Bad Request", "{\"error\":\"no op\"}");
		return;
	}

	int ok = -1; /* -1 = unknown op */
	long c;
	if (strcmp(op, "screen") == 0) {
		ok = json_int(body, "cmd", &c) ? relay_screen((uint8_t)c) : 0;
	} else if (strcmp(op, "nvm") == 0) {
		ok = json_int(body, "cmd", &c) ? relay_nvm((uint8_t)c) : 0;
	} else if (strcmp(op, "clearFaults") == 0) {
		long s = 0xFFFF, l = 0xFFFF;
		json_int(body, "statusMask", &s);
		json_int(body, "logMask", &l);
		ok = relay_clear_faults((uint16_t)s, (uint16_t)l);
	} else if (strcmp(op, "writeConfig") == 0) {
		char data[4096] = {0};
		uint8_t cfg[1024];
		ok = 0;
		if (json_str(body, "data", data, sizeof(data))) {
			int n = b64_decode(data, cfg, sizeof(cfg));
			if (n > 0) ok = relay_write_config(cfg, n);
		}
	}

	if (ok < 0) {
		wlog("WARN", "command from %s: unknown op '%s'", client_ip, op);
		http_respond(cfd, 400, "Bad Request", "{\"error\":\"unknown op\"}");
	} else if (ok) {
		wlog("INFO", "command from %s: op=%s -> executed", client_ip, op);
		http_respond(cfd, 200, "OK", "{\"ok\":true}");
	} else {
		wlog("WARN", "command from %s: op=%s -> relay failed", client_ip, op);
		http_respond(cfd, 500, "Internal Server Error", "{\"error\":\"relay failed\"}");
	}
}

/* Accept one HTTP connection, route GET /sensors and POST /command, close. */
static void http_handle(int http_fd)
{
	struct sockaddr_in peer;
	socklen_t plen = sizeof(peer);
	int cfd = accept(http_fd, (struct sockaddr *)&peer, &plen);
	if (cfd < 0) return;
	char client_ip[INET_ADDRSTRLEN] = "?";
	inet_ntop(AF_INET, &peer.sin_addr, client_ip, sizeof(client_ip));

	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	static char buf[HTTP_MAX_BODY + 2048];
	size_t total = 0;
	char *hdr_end = NULL;
	while (total < sizeof(buf) - 1) {
		ssize_t r = recv(cfd, buf + total, sizeof(buf) - 1 - total, 0);
		if (r <= 0) break;
		total += (size_t)r;
		buf[total] = '\0';
		if ((hdr_end = strstr(buf, "\r\n\r\n")) != NULL) break;
	}
	if (!hdr_end) { close(cfd); return; }

	char method[8] = {0}, path[64] = {0};
	if (sscanf(buf, "%7s %63s", method, path) != 2) { close(cfd); return; }

	if (strcmp(method, "GET") == 0 && strcmp(path, "/sensors") == 0) {
		char body[2048];
		int bn = build_sensors_json(body, sizeof(body));
		char hdr[256];
		int hn = snprintf(hdr, sizeof(hdr),
			"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
			"Access-Control-Allow-Origin: *\r\nConnection: close\r\n"
			"Content-Length: %d\r\n\r\n", bn);
		(void)!write(cfd, hdr, hn);
		(void)!write(cfd, body, bn);
		close(cfd);
		return;
	}

	if (strcmp(method, "GET") == 0 && strcmp(path, "/config") == 0) {
		if (g_serial_fd < 0) {
			http_respond(cfd, 503, "Service Unavailable", "{\"error\":\"no device\"}");
			close(cfd);
			return;
		}
		uint8_t cfg[512], ver = 0;
		int n = relay_read_config(cfg, &ver);
		if (n < 0) {
			http_respond(cfd, 500, "Internal Server Error", "{\"error\":\"read failed\"}");
			close(cfd);
			return;
		}
		char uid[25];
		for (int i = 0; i < 12; i++)
			snprintf(uid + i * 2, 3, "%02X", dev_info.uid[i]);
		char b64[720];
		b64_encode(cfg, n, b64);
		char body[1024];
		snprintf(body, sizeof(body),
			"{\"deviceId\":\"%s\",\"version\":%d,\"data\":\"%s\"}", uid, ver, b64);
		wlog("INFO", "config read by %s", client_ip);
		http_respond(cfd, 200, "OK", body);
		close(cfd);
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/command") == 0) {
		char clbuf[16];
		size_t want = 0;
		if (http_header(buf, "Content-Length", clbuf, sizeof(clbuf)))
			want = (size_t)strtoul(clbuf, NULL, 10);
		if (want > HTTP_MAX_BODY) {
			http_respond(cfd, 413, "Payload Too Large", "{\"error\":\"too large\"}");
			close(cfd);
			return;
		}
		size_t body_off = (size_t)(hdr_end + 4 - buf);
		while (total - body_off < want && total < sizeof(buf) - 1) {
			ssize_t r = recv(cfd, buf + total, sizeof(buf) - 1 - total, 0);
			if (r <= 0) break;
			total += (size_t)r;
		}
		if (body_off + want < sizeof(buf))
			buf[body_off + want] = '\0';
		else
			buf[total] = '\0';
		handle_post_command(cfd, buf, buf + body_off, client_ip);
		close(cfd);
		return;
	}

	const char *resp = "HTTP/1.1 404 Not Found\r\n"
		"Connection: close\r\nContent-Length: 0\r\n\r\n";
	(void)!write(cfd, resp, strlen(resp));
	close(cfd);
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

	load_config();

	int http_fd = -1;
	if (g_http_enabled) {
		http_fd = setup_http();
		if (http_fd < 0)
			fprintf(stderr, "wireviewd: warning: network listener failed (port %d in use?)\n",
				HTTP_PORT);
		else
			printf("wireviewd: network listener ENABLED on :%d; remote writes %s\n",
			       HTTP_PORT, g_secret[0] ? "enabled (secret set)" : "disabled (no secret)");
	} else {
		printf("wireviewd: network listener disabled (set remote_enabled=1 in /etc/wireview/config to publish)\n");
	}

	wlog("INFO", "wireviewd started; listener %s, remote writes %s, log retention %d days",
	     g_http_enabled ? "enabled" : "disabled",
	     g_secret[0] ? "enabled" : "disabled", g_log_retain_days);

	printf("wireviewd: starting\n");

	while (running) {
		int serial_fd = -1;
		int hwmon_fd = -1;
		int sock_fd = -1;
		int client_fds[MAX_CLIENTS];
		int num_clients = 0;

		memset(client_fds, -1, sizeof(client_fds));

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

		tcflush(serial_fd, TCIFLUSH);

		/* Query device info */
		if (query_device_info(serial_fd) < 0) {
			fprintf(stderr, "wireviewd: device info query failed\n");
			close(hwmon_fd);
			close(serial_fd);
			dev_path[0] = '\0';
			sleep(2);
			continue;
		}

		/* Device is ready; allow HTTP command relay. */
		g_serial_fd = serial_fd;

		/* Set up command socket */
		sock_fd = setup_socket();
		if (sock_fd < 0)
			fprintf(stderr, "wireviewd: warning: could not create socket\n");

		printf("wireviewd: polling every %d ms\n", interval_ms);

		/* Event loop with poll() */
		struct timespec next_poll;
		clock_gettime(CLOCK_MONOTONIC, &next_poll);

		while (running) {
			struct pollfd pfds[2 + MAX_CLIENTS];
			int nfds = 0;

			/* Listening socket */
			if (sock_fd >= 0) {
				pfds[nfds].fd = sock_fd;
				pfds[nfds].events = POLLIN;
				nfds++;
			}

			/* HTTP /sensors listener */
			int http_pfd_idx = -1;
			if (http_fd >= 0) {
				http_pfd_idx = nfds;
				pfds[nfds].fd = http_fd;
				pfds[nfds].events = POLLIN;
				nfds++;
			}

			/* Client sockets */
			int client_pfd_start = nfds;
			for (int i = 0; i < num_clients; i++) {
				pfds[nfds].fd = client_fds[i];
				pfds[nfds].events = POLLIN;
				nfds++;
			}

			/* Time until next sensor poll */
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			long wait_ms = (next_poll.tv_sec - now.tv_sec) * 1000 +
				       (next_poll.tv_nsec - now.tv_nsec) / 1000000;
			if (wait_ms < 0) wait_ms = 0;

			int ret = poll(pfds, nfds, (int)wait_ms);
			if (ret < 0 && errno != EINTR)
				break;

			/* Accept new clients */
			if (sock_fd >= 0 && nfds > 0 && (pfds[0].revents & POLLIN)) {
				int new_fd = accept(sock_fd, NULL, NULL);
				if (new_fd >= 0) {
					if (num_clients < MAX_CLIENTS) {
						client_fds[num_clients++] = new_fd;
					} else {
						send_response(new_fd, RESP_ERROR,
							      NULL, 0);
						close(new_fd);
					}
				}
			}

			/* Serve an HTTP /sensors request */
			if (http_pfd_idx >= 0 &&
			    (pfds[http_pfd_idx].revents & POLLIN))
				http_handle(http_fd);

			/* Handle client requests */
			for (int i = 0; i < num_clients; ) {
				int idx = client_pfd_start + i;
				if (idx < nfds && pfds[idx].revents) {
					if (pfds[idx].revents & POLLIN) {
						handle_client_request(
							client_fds[i],
							serial_fd);
					}
					if (pfds[idx].revents &
					    (POLLHUP | POLLERR)) {
						close(client_fds[i]);
						client_fds[i] =
							client_fds[--num_clients];
						client_fds[num_clients] = -1;
						continue;
					}
				}
				i++;
			}

			/* Sensor poll */
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec > next_poll.tv_sec ||
			    (now.tv_sec == next_poll.tv_sec &&
			     now.tv_nsec >= next_poll.tv_nsec)) {

				struct sensor_struct ss;
				if (read_sensors(serial_fd, &ss) < 0) {
					fprintf(stderr,
						"wireviewd: read failed, reconnecting\n");
					break;
				}

				if (write_hwmon(hwmon_fd, &ss) < 0) {
					fprintf(stderr,
						"wireviewd: hwmon write failed\n");
					break;
				}

				memcpy(&g_last, &ss, sizeof(g_last));
				g_have_last = 1;

				next_poll.tv_sec = now.tv_sec;
				next_poll.tv_nsec = now.tv_nsec +
						    (long)interval_ms * 1000000;
				if (next_poll.tv_nsec >= 1000000000L) {
					next_poll.tv_sec +=
						next_poll.tv_nsec / 1000000000L;
					next_poll.tv_nsec %= 1000000000L;
				}
			}
		}

		/* Cleanup */
		for (int i = 0; i < num_clients; i++) {
			if (client_fds[i] >= 0) close(client_fds[i]);
		}
		cleanup_socket(sock_fd);

		g_serial_fd = -1;
		close(hwmon_fd);
		close(serial_fd);
		dev_path[0] = '\0';
		g_have_last = 0;

		if (running)
			sleep(2);
	}

	if (http_fd >= 0)
		close(http_fd);

	printf("wireviewd: stopped\n");
	return 0;
}
