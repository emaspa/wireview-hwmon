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

#define WIREVIEW_VID "0483"
#define WIREVIEW_PID "5740"
#define HWMON_DEV    "/dev/wireview-hwmon"
#define SOCK_PATH    "/run/wireviewd.sock"

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

		close(hwmon_fd);
		close(serial_fd);
		dev_path[0] = '\0';

		if (running)
			sleep(2);
	}

	printf("wireviewd: stopped\n");
	return 0;
}
