/*
 * wireviewd - Daemon for WireView Pro II hwmon integration
 *
 * Reads sensor data from the WireView Pro II device over serial and
 * writes it to /dev/wireview-hwmon for the wireview_hwmon kernel module.
 * Also exposes a Unix socket for bidirectional command relay from apps.
 *
 * Usage: wireviewd [-i interval_ms] [-d device_path] [-V]
 *
 * The command socket (/run/wireviewd.sock) is world-connectable, but commands
 * that can alter or brick the device (bootloader, NVM, config write, serial
 * handover) are only accepted from root or members of the "wireview" group
 * (see WIREVIEW_GROUP); other peers get RESP_DENIED.
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
#include <stddef.h>
#include <time.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <linux/limits.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdarg.h>
#include <pwd.h>
#include <grp.h>
#include "sha256.h"

#define WIREVIEW_VID "0483"
#define WIREVIEW_PID "5740"
/* Overridable (-D) only so test builds can run beside the live daemon. */
#ifndef HWMON_DEV
#define HWMON_DEV    "/dev/wireview-hwmon"
#endif
#ifndef SOCK_PATH
#define SOCK_PATH    "/run/wireviewd.sock"
#endif
#ifndef CONFIG_PATH
#define CONFIG_PATH  "/etc/wireview/config"
#endif
#define HTTP_PORT    9876

/* Members of this group (and root) may send privileged socket commands. */
#define WIREVIEW_GROUP "wireview"

/* Package version, injected by the Makefile (-DWIREVIEW_PKG_VERSION=\"x.y.z\"). */
#ifndef WIREVIEW_PKG_VERSION
#define WIREVIEW_PKG_VERSION "unknown"
#endif

#define WIREVIEW_MAGIC   0x57565032
#define WIREVIEW_VERSION 3

#define MAX_CLIENTS 4

/* A socket request (3-byte header + payload) must arrive in full within
 * this many ms of its first byte, or the client is disconnected. */
#define CLIENT_REQ_TIMEOUT_MS 2000
#define CLIENT_MAX_PAYLOAD    512

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

/* Socket protocol command types. Commands marked [priv] are refused with
 * RESP_DENIED unless the peer is root or in WIREVIEW_GROUP (primary or
 * supplementary); the rest are open to any local user. */
#define WCMD_GET_DEVICE_INFO   0x01
#define WCMD_CLEAR_FAULTS      0x02
#define WCMD_READ_CONFIG       0x03
#define WCMD_WRITE_CONFIG      0x04	/* [priv] */
#define WCMD_SCREEN_CMD        0x05
#define WCMD_NVM_CMD           0x06	/* [priv] store / factory reset */
#define WCMD_READ_BUILD        0x07
#define WCMD_ENTER_BOOTLOADER  0x08	/* [priv] */
/* Serial handover: the GUI asks the daemon to stop all serial I/O for N
 * seconds so it can drive the port directly (SPI-flash log reads, theme
 * asset transfers). The daemon keeps its fd open but goes quiet; sensor
 * polling resumes automatically at the deadline or on RESUME. */
#define WCMD_SUSPEND_SERIAL    0x09	/* [priv] */
#define WCMD_RESUME_SERIAL     0x0A	/* [priv] */

/* Response status */
#define RESP_OK            0
#define RESP_ERROR         1
#define RESP_NOT_CONNECTED 2
#define RESP_DENIED        3	/* privileged command from unprivileged peer */

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
	/* v3: energy since daemon start, microjoules, saturating at INT64_MAX */
	int64_t  energy_uj;
};

_Static_assert(sizeof(struct hwmon_data) == 156, "hwmon_data size mismatch");

/* A v2 record is the v3 one without energy_uj. A module from before v3
 * rejects the longer record with EINVAL; write_hwmon() then falls back. */
#define HWMON_V2_SIZE offsetof(struct hwmon_data, energy_uj)
_Static_assert(HWMON_V2_SIZE == 148, "hwmon_data v2 prefix mismatch");

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

/* UID (uppercase hex) of the last device that connected; kept across
 * disconnects as the /metrics device label. Empty until the first. */
static char g_dev_uid[25];

/* Energy integrated from accepted frames since daemon start (never reset
 * on reconnect). g_energy_frac carries the sub-microjoule remainder in
 * uW*ns so no rounding accumulates; g_energy_ts is the CLOCK_MONOTONIC
 * time of the previous accepted frame, valid while g_energy_ts_valid. */
static int64_t g_energy_uj;
static int64_t g_energy_frac;
static struct timespec g_energy_ts;
static int g_energy_ts_valid;

/* Set when the loaded module rejected a v3 record: write v2 (no energy)
 * until the next connect. */
static int g_hwmon_v2;

/* Write-command auth + relay state for the HTTP POST /command endpoint. */
static char g_secret[128];      /* shared HMAC secret; empty => writes disabled */
static int  g_serial_fd = -1;   /* current serial fd, for HTTP command relay */
static struct timespec g_suspend_until; /* CLOCK_MONOTONIC deadline; 0 = active */

static void wlog(const char *level, const char *fmt, ...);

static int serial_suspended(void)
{
	struct timespec now;
	if (g_suspend_until.tv_sec == 0)
		return 0;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (now.tv_sec > g_suspend_until.tv_sec ||
	    (now.tv_sec == g_suspend_until.tv_sec &&
	     now.tv_nsec >= g_suspend_until.tv_nsec)) {
		g_suspend_until.tv_sec = 0;
		g_suspend_until.tv_nsec = 0;
		return 0;
	}
	return 1;
}
static int  g_http_enabled = 0; /* network listener off unless config enables it */
static int  g_http_port = HTTP_PORT; /* listener port (config: port=) */
static char g_bind_addr[192];   /* numeric listen address (config: bind=); empty = all */
static int  g_log_retain_days = 14; /* days of audit logs to keep (config: log_days=) */

#define HTTP_MAX_BODY    8192
#define HTTP_MAX_HEADER  8192   /* request line + headers, before the blank line */
#define HTTP_DEADLINE_MS 3000   /* whole-connection budget for reading the request */
#define HTTP_AUTH_WINDOW 30     /* seconds of timestamp skew tolerated */

static void sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

/* Set *deadline to CLOCK_MONOTONIC now + ms. */
static void deadline_in(struct timespec *deadline, long ms)
{
	clock_gettime(CLOCK_MONOTONIC, deadline);
	deadline->tv_sec += ms / 1000;
	deadline->tv_nsec += (ms % 1000) * 1000000L;
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000L;
	}
}

/* Milliseconds left until a CLOCK_MONOTONIC deadline (<= 0 once passed). */
static long ms_until(const struct timespec *deadline)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (deadline->tv_sec - now.tv_sec) * 1000L +
	       (deadline->tv_nsec - now.tv_nsec) / 1000000L;
}

/* Read exactly n bytes from fd with timeout. Returns 0 on success, -1 on
 * failure (EOF, error or timeout).
 *
 * Used on the serial fd, which is blocking with VMIN=0/VTIME=10: read()
 * returns what has arrived, or 0 after 1 s of silence (treated as
 * failure, as before). On a non-blocking fd read() returns EAGAIN
 * instead, and we poll() for the time remaining rather than spin. */
static int read_exact(int fd, void *buf, size_t n, int timeout_ms)
{
	size_t off = 0;
	struct timespec deadline;

	deadline_in(&deadline, timeout_ms);

	while (off < n) {
		ssize_t r = read(fd, (char *)buf + off, n - off);
		int wait = 0;

		if (r > 0)
			off += (size_t)r;
		else if (r == 0)
			return -1;
		else if (errno == EAGAIN || errno == EWOULDBLOCK)
			wait = 1;
		else if (errno != EINTR)
			return -1;

		if (off == n)
			break;
		long left = ms_until(&deadline);
		if (left <= 0)
			return -1;
		if (wait) {
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			if (poll(&pfd, 1, (int)left) == 0)
				return -1;
		}
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

/* The serial protocol has no framing or CRC, so a desynced read corrupts
 * arbitrary fields for one poll (a real corrupt frame in the field carried
 * fan=105, pad1=122, status=0x0607). Real frames always have zero padding
 * and a fan duty <= 100; anything else is discarded, and the next poll's
 * tcflush realigns the stream. */
static int frame_is_sane(const struct sensor_struct *ss)
{
	return ss->fan_duty <= 100 && !ss->_pad1 && !ss->_pad2;
}

/* Total power of a frame in uW: sum of the pins' mV * mA, as published
 * to hwmon. */
static int64_t frame_power_uw(const struct sensor_struct *ss)
{
	int64_t sum = 0;

	for (int i = 0; i < 6; i++)
		sum += (int64_t)ss->pins[i].voltage *
		       (int64_t)ss->pins[i].current;
	return sum;
}

#define NSEC_PER_SEC 1000000000LL

/* energy_uj += p_uw * dt_ns / 1e9, exactly (the remainder carries over in
 * g_energy_frac), saturating at INT64_MAX. Plain 64-bit arithmetic with
 * overflow checks, since 32-bit targets have no __int128:
 *   p * dt / 1e9 = p * s + a * n + (b * n) / 1e9
 * with dt = s * 1e9 + n and p = a * 1e9 + b, where b * n < 1e18. */
static void energy_add(int64_t p_uw, int64_t dt_ns)
{
	int64_t s = dt_ns / NSEC_PER_SEC, n = dt_ns % NSEC_PER_SEC;
	int64_t a = p_uw / NSEC_PER_SEC, b = p_uw % NSEC_PER_SEC;
	int64_t add, t, bn;
	int ovf = 0;

	if (p_uw <= 0 || dt_ns <= 0 || g_energy_uj == INT64_MAX)
		return;
	ovf |= __builtin_mul_overflow(p_uw, s, &add);
	ovf |= __builtin_mul_overflow(a, n, &t);
	ovf |= __builtin_add_overflow(add, t, &add);
	bn = b * n + g_energy_frac;
	g_energy_frac = bn % NSEC_PER_SEC;
	ovf |= __builtin_add_overflow(add, bn / NSEC_PER_SEC, &add);
	ovf |= __builtin_add_overflow(g_energy_uj, add, &g_energy_uj);
	if (ovf)
		g_energy_uj = INT64_MAX;
}

/* Integrate an accepted frame's power over the time since the previous
 * accepted frame. After a gap longer than max_gap_ms (serial handover,
 * reconnect, system suspend) the step is skipped rather than attributing
 * the whole gap to this frame's power; the frame only restarts the clock. */
static void energy_accumulate(const struct sensor_struct *ss, long max_gap_ms)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (g_energy_ts_valid) {
		int64_t dt_ns = (int64_t)(now.tv_sec - g_energy_ts.tv_sec) *
				NSEC_PER_SEC + (now.tv_nsec - g_energy_ts.tv_nsec);

		if (dt_ns <= (int64_t)max_gap_ms * 1000000)
			energy_add(frame_power_uw(ss), dt_ns);
	}
	g_energy_ts = now;
	g_energy_ts_valid = 1;
}

/* Convert sensor data to hwmon format and write to kernel module */
static int write_hwmon(int hwmon_fd, const struct sensor_struct *ss)
{
	struct hwmon_data hd;
	int i;

	memset(&hd, 0, sizeof(hd));
	hd.magic = WIREVIEW_MAGIC;
	hd.version = WIREVIEW_VERSION;

	for (i = 0; i < 6; i++) {
		hd.voltage_mv[i] = ss->pins[i].voltage;
		hd.current_ma[i] = (int32_t)ss->pins[i].current;
		hd.pin_power_uw[i] = (int64_t)ss->pins[i].voltage *
				     (int64_t)ss->pins[i].current;
	}
	hd.total_power_uw = frame_power_uw(ss);

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
	hd.energy_uj = g_energy_uj;

	if (!g_hwmon_v2) {
		if (write(hwmon_fd, &hd, sizeof(hd)) == sizeof(hd))
			return 0;
		if (errno != EINVAL)
			return -1;
		/* A module older than v3 is still loaded (e.g. upgraded
		 * package, no reboot yet): keep it fed with v2 records. */
		g_hwmon_v2 = 1;
		printf("wireviewd: hwmon module does not accept v3 records, writing v2 (no energy)\n");
		wlog("WARN", "hwmon module rejected a v3 record; writing v2 until the next connect (reload wireview_hwmon for energy)");
	}
	hd.version = 2;
	if (write(hwmon_fd, &hd, HWMON_V2_SIZE) != (ssize_t)HWMON_V2_SIZE)
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
	/* Zero everything after the first NUL so no device garbage lingers
	 * past the string. */
	{
		size_t bl = strnlen(dev_info.build_string, 32);

		memset(dev_info.build_string + bl, 0,
		       sizeof(dev_info.build_string) - bl);
	}

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

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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

/* ---- Command socket peer privileges ---- */

/* One connected command-socket client. privileged is decided once at
 * accept time from SO_PEERCRED. The fd is non-blocking: request bytes are
 * accumulated in req[] as they arrive, and the request is dispatched only
 * once complete, so a slow or stalled peer never blocks the poll loop. */
struct client {
	int   fd;
	int   privileged;
	uid_t uid;
	size_t have;			/* bytes of the current request in req[] */
	struct timespec req_deadline;	/* valid while have > 0 */
	uint8_t req[3 + CLIENT_MAX_PAYLOAD];
};

static gid_t g_wireview_gid;
static int   g_have_wireview_gid;	/* 0 => group missing, only root is privileged */

/* Resolve WIREVIEW_GROUP once at startup. */
static void resolve_wireview_group(void)
{
	struct group *gr = getgrnam(WIREVIEW_GROUP);

	if (gr) {
		g_wireview_gid = gr->gr_gid;
		g_have_wireview_gid = 1;
		return;
	}
	g_have_wireview_gid = 0;
	printf("wireviewd: group '%s' not found; privileged socket commands limited to root\n",
	       WIREVIEW_GROUP);
	wlog("INFO", "group '%s' not found; privileged socket commands (bootloader, NVM, "
	     "config write, serial suspend/resume) limited to root", WIREVIEW_GROUP);
}

/* Decide whether a peer (uid, primary gid) is privileged, given the
 * wireview group gid (have_gid == 0 => group does not exist). Supplementary
 * groups come from the group database for the uid's user name. */
static int peer_is_privileged(uid_t uid, gid_t gid, int have_gid, gid_t wv_gid)
{
	if (uid == 0)
		return 1;
	if (!have_gid)
		return 0;
	if (gid == wv_gid)
		return 1;

	struct passwd *pw = getpwuid(uid);
	if (!pw || !pw->pw_name)
		return 0;

	gid_t stackbuf[64];
	gid_t *groups = stackbuf;
	int ngroups = (int)(sizeof(stackbuf) / sizeof(stackbuf[0]));
	int ok = 0;

	if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0) {
		/* ngroups now holds the required size */
		if (ngroups <= 0 || ngroups > 65536)
			return 0;
		groups = malloc((size_t)ngroups * sizeof(gid_t));
		if (!groups)
			return 0;
		if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0) {
			free(groups);
			return 0;
		}
	}
	for (int i = 0; i < ngroups; i++) {
		if (groups[i] == wv_gid) {
			ok = 1;
			break;
		}
	}
	if (groups != stackbuf)
		free(groups);
	return ok;
}

/* Fetch the peer's credentials and classify it. Unknown peers are
 * unprivileged. */
static void client_init(struct client *c, int fd)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);

	c->fd = fd;
	c->privileged = 0;
	c->uid = (uid_t)-1;
	c->have = 0;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
	    len == sizeof(cred)) {
		c->uid = cred.uid;
		c->privileged = peer_is_privileged(cred.uid, cred.gid,
						   g_have_wireview_gid,
						   g_wireview_gid);
	}
}

static int cmd_is_privileged(uint8_t cmd_type)
{
	switch (cmd_type) {
	case WCMD_ENTER_BOOTLOADER:
	case WCMD_NVM_CMD:
	case WCMD_WRITE_CONFIG:
	case WCMD_SUSPEND_SERIAL:
	case WCMD_RESUME_SERIAL:
		return 1;
	default:
		return 0;
	}
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

static long client_uid(const struct client *c)
{
	return c->uid == (uid_t)-1 ? -1L : (long)c->uid;
}

/* Execute the complete request held in c->req. */
static void handle_client_request(const struct client *c, int serial_fd)
{
	int client_fd = c->fd;
	uint8_t cmd_type = c->req[0];
	uint16_t payload_len = c->req[1] | ((uint16_t)c->req[2] << 8);
	const uint8_t *payload = c->req + 3;

	if (cmd_is_privileged(cmd_type) && !c->privileged) {
		wlog("WARN", "socket cmd 0x%02x from uid %ld denied: not root or in group '%s'",
		     cmd_type, client_uid(c), WIREVIEW_GROUP);
		send_response(client_fd, RESP_DENIED, NULL, 0);
		return;
	}

	if (serial_fd < 0) {
		send_response(client_fd, RESP_NOT_CONNECTED, NULL, 0);
		return;
	}

	if (cmd_type == WCMD_SUSPEND_SERIAL) {
		int secs = 60;
		if (payload_len >= 2)
			secs = payload[0] | ((int)payload[1] << 8);
		if (secs < 1) secs = 1;
		if (secs > 300) secs = 300;
		clock_gettime(CLOCK_MONOTONIC, &g_suspend_until);
		g_suspend_until.tv_sec += secs;
		wlog("INFO", "serial suspended for %d s (GUI direct access)", secs);
		send_response(client_fd, RESP_OK, NULL, 0);
		return;
	}
	if (cmd_type == WCMD_RESUME_SERIAL) {
		g_suspend_until.tv_sec = 0;
		g_suspend_until.tv_nsec = 0;
		tcflush(serial_fd, TCIFLUSH);
		wlog("INFO", "serial resumed");
		send_response(client_fd, RESP_OK, NULL, 0);
		return;
	}
	/* While the GUI owns the port, refuse serial-touching commands; the
	 * cached GET_DEVICE_INFO is still fine. */
	if (serial_suspended() && cmd_type != WCMD_GET_DEVICE_INFO) {
		wlog("WARN", "relay cmd 0x%02x rejected: serial suspended", cmd_type);
		send_response(client_fd, RESP_ERROR, NULL, 0);
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
		ssize_t wr = write(serial_fd, cmd, 2);
		wlog("INFO", "screen relay 0x%02x -> write=%zd", payload[0], wr);
		if (wr == 2)
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

/* Bytes the current request needs in total: the header, plus the payload
 * once the header is in. */
static size_t client_need(const struct client *c)
{
	if (c->have < 3)
		return 3;
	return 3 + (size_t)(c->req[1] | ((uint16_t)c->req[2] << 8));
}

/* Called on POLLIN: read what the client has sent without blocking and
 * dispatch the request once it is complete. The request deadline starts
 * at its first POLLIN; main() drops clients that miss it. Returns -1 if
 * the client should be closed (EOF or read error), else 0. */
static int client_read(struct client *c, int serial_fd)
{
	if (c->have == 0)
		deadline_in(&c->req_deadline, CLIENT_REQ_TIMEOUT_MS);

	while (c->have < client_need(c)) {
		ssize_t r = read(c->fd, c->req + c->have,
				 client_need(c) - c->have);
		if (r == 0)
			return -1;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;	/* rest comes on a later POLLIN */
			return -1;
		}
		c->have += (size_t)r;

		if (c->have == 3 && client_need(c) - 3 > CLIENT_MAX_PAYLOAD) {
			/* Oversized: refuse without reading the payload. */
			c->have = 0;
			send_response(c->fd, RESP_ERROR, NULL, 0);
			return 0;
		}
	}

	handle_client_request(c, serial_fd);
	c->have = 0;
	return 0;
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
 * Escape a string for use inside a JSON string literal. Escapes '"' and '\\',
 * writes control characters (< 0x20) as \\u00XX, and replaces every byte
 * >= 0x80 with '?'. The inputs (hostname, firmware build string) are ASCII
 * in practice, so dropping non-ASCII is simpler than validating UTF-8 and
 * still guarantees valid JSON. Output is always NUL-terminated and never
 * ends in a partial escape sequence.
 */
static void json_escape(const char *in, char *out, size_t cap)
{
	size_t o = 0;

	if (cap == 0)
		return;
	for (; *in; in++) {
		unsigned char c = (unsigned char)*in;
		char esc[8];
		size_t n;

		if (c == '"' || c == '\\') {
			esc[0] = '\\';
			esc[1] = (char)c;
			n = 2;
		} else if (c < 0x20) {
			snprintf(esc, sizeof(esc), "\\u%04x", c);
			n = 6;
		} else if (c >= 0x80) {
			esc[0] = '?';
			n = 1;
		} else {
			esc[0] = (char)c;
			n = 1;
		}
		if (o + n >= cap)
			break;
		memcpy(out + o, esc, n);
		o += n;
	}
	out[o] = '\0';
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
	char host_js[sizeof(host) * 6];
	json_escape(host, host_js, sizeof(host_js));

	char ts[32];
	time_t now = time(NULL);
	struct tm tmv;
	gmtime_r(&now, &tmv);
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);

	/* No device / no frame yet -> empty list so consumers drop it. */
	if (!g_have_last || !dev_info.valid)
		return snprintf(out, cap,
			"{\"host\":\"%s\",\"appVersion\":\"wireviewd\",\"devices\":[]}",
			host_js);

	char uid[25];
	for (int i = 0; i < 12; i++)
		snprintf(uid + i * 2, 3, "%02X", dev_info.uid[i]);

	char build_js[33 * 6];
	json_escape(dev_info.build_string, build_js, sizeof(build_js));

	double pv[6], pc[6], sum_p = 0, sum_c = 0;
	for (int i = 0; i < 6; i++) {
		pv[i] = g_last.pins[i].voltage / 1000.0;
		pc[i] = g_last.pins[i].current / 1000.0;
		sum_p += pv[i] * pc[i];
		sum_c += pc[i];
	}
	/* Out-of-range readings (sensor disconnected) are sent as 0.0, not
	 * null: the consumer's WireViewSensorDto declares TempInC/TempOutC/
	 * Ext1C/Ext2C as plain (non-nullable) double, so null would fail
	 * deserialization there. */
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
		"\"sumCurrentA\":%.3f,\"sumPowerW\":%.3f,\"energyJ\":%.3f}]}",
		host_js, uid, dev_info.fw_version, build_js, ts,
		pv[0], pv[1], pv[2], pv[3], pv[4], pv[5],
		pc[0], pc[1], pc[2], pc[3], pc[4], pc[5],
		t[0], t[1], t[2], t[3],
		psu_cap_watts(g_last.hpwr_cap), g_last.fan_duty,
		g_last.fault_status, g_last.fault_log,
		sum_c, sum_p, g_energy_uj / 1e6);
}

/* ---- HTTP /metrics (Prometheus text exposition format 0.0.4) ---- */

/* Fixed-size output buffer; once a write does not fit, overflow is set
 * and later writes are ignored, so the caller never sends a torn body. */
struct outbuf {
	char   *p;
	size_t  cap;
	size_t  len;
	int     overflow;
};

static void __attribute__((format(printf, 2, 3)))
ob_printf(struct outbuf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (b->overflow)
		return;
	va_start(ap, fmt);
	n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= b->cap - b->len) {
		b->overflow = 1;
		return;
	}
	b->len += (size_t)n;
}

/*
 * Escape a Prometheus label value: backslash, double quote and newline
 * are escaped as the format requires; other control characters and every
 * byte >= 0x80 become '?', as json_escape() does. Output is always
 * NUL-terminated and never ends in a partial escape sequence.
 */
static void prom_escape(const char *in, char *out, size_t cap)
{
	size_t o = 0;

	if (cap == 0)
		return;
	for (; *in; in++) {
		unsigned char c = (unsigned char)*in;
		char esc[2];
		size_t n = 1;

		if (c == '"' || c == '\\') {
			esc[0] = '\\';
			esc[1] = (char)c;
			n = 2;
		} else if (c == '\n') {
			esc[0] = '\\';
			esc[1] = 'n';
			n = 2;
		} else if (c < 0x20 || c >= 0x80) {
			esc[0] = '?';
		} else {
			esc[0] = (char)c;
		}
		if (o + n >= cap)
			break;
		memcpy(out + o, esc, n);
		o += n;
	}
	out[o] = '\0';
}

static void prom_family(struct outbuf *b, const char *name, const char *type,
			const char *help)
{
	ob_printf(b, "# HELP %s %s\n# TYPE %s %s\n", name, help, name, type);
}

/* Fault bits 0..5 of fault_status, as wireview_fault_active{fault=...}. */
static const char * const fault_names[] = {
	"chip_over_temp", "sensor_over_temp", "over_current",
	"wire_over_current", "over_power", "current_imbalance",
};

/* Build the GET /metrics body from the latest accepted frame. Returns 0,
 * or -1 if the body did not fit. Without a device (or before its first
 * frame) only wireview_up 0 is exposed, labelled with the last device
 * seen if there was one, so the series stays the same across a blip. */
static int build_metrics(struct outbuf *b)
{
	static const char * const temp_names[4] = {
		"onboard_in", "onboard_out", "external_1", "external_2",
	};
	int up = g_serial_fd >= 0 && g_have_last && dev_info.valid;
	char dev[40];
	int i;

	if (g_dev_uid[0])
		snprintf(dev, sizeof(dev), "device=\"%s\"", g_dev_uid);
	else
		dev[0] = '\0';

	prom_family(b, "wireview_up", "gauge",
		    "1 if the device is connected and reporting, else 0.");
	if (!up) {
		if (dev[0])
			ob_printf(b, "wireview_up{%s} 0\n", dev);
		else
			ob_printf(b, "wireview_up 0\n");
		return b->overflow ? -1 : 0;
	}
	ob_printf(b, "wireview_up{%s} 1\n", dev);

	prom_family(b, "wireview_pin_voltage_volts", "gauge",
		    "Voltage per 12V-2x6 pin.");
	for (i = 0; i < 6; i++)
		ob_printf(b, "wireview_pin_voltage_volts{%s,pin=\"%d\"} %.3f\n",
			  dev, i + 1, g_last.pins[i].voltage / 1000.0);
	prom_family(b, "wireview_pin_current_amps", "gauge",
		    "Current per 12V-2x6 pin.");
	for (i = 0; i < 6; i++)
		ob_printf(b, "wireview_pin_current_amps{%s,pin=\"%d\"} %.3f\n",
			  dev, i + 1, g_last.pins[i].current / 1000.0);
	prom_family(b, "wireview_pin_power_watts", "gauge",
		    "Power per 12V-2x6 pin.");
	for (i = 0; i < 6; i++)
		ob_printf(b, "wireview_pin_power_watts{%s,pin=\"%d\"} %.3f\n",
			  dev, i + 1,
			  (double)g_last.pins[i].voltage *
			  (double)g_last.pins[i].current / 1e6);

	prom_family(b, "wireview_power_watts", "gauge",
		    "Total power, sum of the pins.");
	ob_printf(b, "wireview_power_watts{%s} %.3f\n",
		  dev, (double)frame_power_uw(&g_last) / 1e6);
	prom_family(b, "wireview_current_amps", "gauge",
		    "Total current reported by the device.");
	ob_printf(b, "wireview_current_amps{%s} %.3f\n",
		  dev, g_last.total_current / 1000.0);
	prom_family(b, "wireview_voltage_average_volts", "gauge",
		    "Average pin voltage reported by the device.");
	ob_printf(b, "wireview_voltage_average_volts{%s} %.3f\n",
		  dev, g_last.avg_voltage / 1000.0);
	prom_family(b, "wireview_vdd_volts", "gauge",
		    "Device supply voltage.");
	ob_printf(b, "wireview_vdd_volts{%s} %.3f\n", dev, g_last.vdd / 1000.0);

	prom_family(b, "wireview_temperature_celsius", "gauge",
		    "Temperatures; disconnected sensors are omitted.");
	for (i = 0; i < 4; i++) {
		int16_t raw = g_last.ts[i];

		/* Disconnected sensors report out-of-range values */
		if (raw < -400 || raw > 2000)
			continue;
		ob_printf(b, "wireview_temperature_celsius{%s,sensor=\"%s\"} %.1f\n",
			  dev, temp_names[i], raw / 10.0);
	}

	prom_family(b, "wireview_fan_duty_ratio", "gauge",
		    "Fan duty cycle, 0 to 1.");
	ob_printf(b, "wireview_fan_duty_ratio{%s} %.2f\n",
		  dev, g_last.fan_duty / 100.0);
	prom_family(b, "wireview_psu_cap_watts", "gauge",
		    "PSU power capability from the sideband pins (0 = unknown).");
	ob_printf(b, "wireview_psu_cap_watts{%s} %d\n",
		  dev, psu_cap_watts(g_last.hpwr_cap));

	prom_family(b, "wireview_fault_status", "gauge",
		    "Active fault bitmask (debounced).");
	ob_printf(b, "wireview_fault_status{%s} %u\n", dev, g_last.fault_status);
	prom_family(b, "wireview_fault_log", "gauge",
		    "Latched fault bitmask (debounced).");
	ob_printf(b, "wireview_fault_log{%s} %u\n", dev, g_last.fault_log);
	prom_family(b, "wireview_fault_active", "gauge",
		    "1 while the fault is active (fault_status bits 0-5).");
	for (i = 0; i < 6; i++)
		ob_printf(b, "wireview_fault_active{%s,fault=\"%s\"} %d\n",
			  dev, fault_names[i], (g_last.fault_status >> i) & 1);

	prom_family(b, "wireview_energy_joules_total", "counter",
		    "Energy delivered since wireviewd started.");
	ob_printf(b, "wireview_energy_joules_total{%s} %.6f\n",
		  dev, (double)g_energy_uj / 1e6);

	char build[33 * 2 + 1];
	prom_escape(dev_info.build_string, build, sizeof(build));
	prom_family(b, "wireview_firmware_info", "gauge",
		    "Firmware version and build string.");
	ob_printf(b, "wireview_firmware_info{%s,version=\"%d\",build=\"%s\"} 1\n",
		  dev, dev_info.fw_version, build);

	return b->overflow ? -1 : 0;
}

/* Open a listening socket on sa. Returns the fd, or -1 with errno set. */
static int listen_on(const struct sockaddr *sa, socklen_t len)
{
	int fd = socket(sa->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;

	int yes = 1, no = 0;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	/* Dual-stack: IPv4 peers of an IPv6 socket arrive v4-mapped. */
	if (sa->sa_family == AF_INET6)
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

	if (bind(fd, sa, len) < 0 || listen(fd, 8) < 0) {
		int e = errno;

		close(fd);
		errno = e;
		return -1;
	}
	return fd;
}

/*
 * Open the LAN listener on bind= (a numeric IPv4 or IPv6 address) or, by
 * default, on every address: [::] dual-stack, falling back to 0.0.0.0 when
 * the host has no IPv6. desc receives the address for messages. Returns
 * the fd, or -1 with errno set. A bind= value that is not a numeric
 * address fails the listener rather than widening it to all addresses.
 */
static int setup_http(char *desc, size_t desc_len)
{
	int fd;

	if (g_bind_addr[0]) {
		struct addrinfo hints, *res;
		char port[8];
		int rc;

		if (strchr(g_bind_addr, ':'))
			snprintf(desc, desc_len, "[%s]:%d", g_bind_addr, g_http_port);
		else
			snprintf(desc, desc_len, "%s:%d", g_bind_addr, g_http_port);

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_PASSIVE;
		snprintf(port, sizeof(port), "%d", g_http_port);
		rc = getaddrinfo(g_bind_addr, port, &hints, &res);
		if (rc != 0) {
			fprintf(stderr, "wireviewd: bind=%s is not a numeric IPv4/IPv6 address (%s)\n",
				g_bind_addr, gai_strerror(rc));
			errno = EINVAL;
			return -1;
		}
		fd = listen_on(res->ai_addr, res->ai_addrlen);
		freeaddrinfo(res);
		return fd;
	}

	struct sockaddr_in6 a6;
	memset(&a6, 0, sizeof(a6));
	a6.sin6_family = AF_INET6;
	a6.sin6_addr = in6addr_any;
	a6.sin6_port = htons((uint16_t)g_http_port);
	snprintf(desc, desc_len, "[::]:%d", g_http_port);
	fd = listen_on((struct sockaddr *)&a6, sizeof(a6));
	if (fd >= 0 || (errno != EAFNOSUPPORT && errno != EPROTONOSUPPORT &&
			errno != EADDRNOTAVAIL))
		return fd;

	/* No IPv6 on this host (ipv6.disable=1 or a sandbox without
	 * AF_INET6): IPv4 only. */
	struct sockaddr_in a4;
	memset(&a4, 0, sizeof(a4));
	a4.sin_family = AF_INET;
	a4.sin_addr.s_addr = htonl(INADDR_ANY);
	a4.sin_port = htons((uint16_t)g_http_port);
	snprintf(desc, desc_len, "0.0.0.0:%d", g_http_port);
	return listen_on((struct sockaddr *)&a4, sizeof(a4));
}

/* Numeric peer address for the audit log. IPv4 clients of the dual-stack
 * listener arrive v4-mapped (::ffff:a.b.c.d) and are shown as plain IPv4. */
static void peer_ip(const struct sockaddr_storage *ss, socklen_t len,
		    char *out, size_t outlen)
{
	snprintf(out, outlen, "?");
	if (ss->ss_family == AF_INET6) {
		const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)ss;

		if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
			if (!inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12],
				       out, (socklen_t)outlen))
				snprintf(out, outlen, "?");
			return;
		}
	}
	if (getnameinfo((const struct sockaddr *)ss, len, out, (socklen_t)outlen,
			NULL, 0, NI_NUMERICHOST) != 0)
		snprintf(out, outlen, "?");
}

/* ---- Daily-rotating audit log ---- */
#ifndef LOG_DIR
#define LOG_DIR "/var/log/wireview"
#endif

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

/* Load the network-listener flag and shared HMAC secret from CONFIG_PATH
 * (/etc/wireview/config). The file holds "key=value" lines: "remote_enabled=0|1" gates
 * the listener (default OFF, so no port is opened unless explicitly enabled),
 * "port=" and "bind=" (numeric address, default all) place it, and "secret=<passphrase>"
 * sets the write secret; a bare line is taken as the secret (backward compatible).
 * $WIREVIEW_LISTEN and $WIREVIEW_SECRET override the file. An empty secret leaves
 * writes refused (403) even when the listener is on. */
static void load_config(void)
{
	g_secret[0] = '\0';
	g_http_enabled = 0;

	FILE *f = fopen(CONFIG_PATH, "r");
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
				/* "key = value": trim blanks on both sides of '='. */
				char *kend = eq;
				while (kend > p && (kend[-1] == ' ' || kend[-1] == '\t'))
					kend--;
				*kend = '\0';
				char *val = eq + 1;
				while (*val == ' ' || *val == '\t') val++;
				if (strcmp(p, "remote_enabled") == 0)
					g_http_enabled = truthy(val);
				else if (strcmp(p, "secret") == 0 && !g_secret[0])
					snprintf(g_secret, sizeof(g_secret), "%s", val);
				else if (strcmp(p, "bind") == 0)
					snprintf(g_bind_addr, sizeof(g_bind_addr), "%s", val);
				else if (strcmp(p, "log_days") == 0)
					g_log_retain_days = atoi(val);
				else if (strcmp(p, "port") == 0) {
					int pp = atoi(val);
					if (pp > 0 && pp < 65536) g_http_port = pp;
				}
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

static const char *json_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return p;
}

/* p is at an opening '"'. Returns the byte after the closing quote, or
 * NULL if the string is not terminated. A backslash escapes the next
 * byte, whatever it is. */
static const char *json_skip_str(const char *p)
{
	for (p++; *p; p++) {
		if (*p == '\\') {
			if (!*++p)
				return NULL;
		} else if (*p == '"') {
			return p + 1;
		}
	}
	return NULL;
}

/* Skip the value at p: a string, an object or array (by depth, with the
 * strings inside skipped, so no brace or quote in them counts), or a
 * scalar up to the next ',', '}', ']' or blank. Returns the byte after
 * it, or NULL if the input ends first. */
static const char *json_skip_value(const char *p)
{
	int depth = 0;

	for (;;) {
		char c = *p;

		if (c == '\0')
			return NULL;
		if (c == '"') {
			p = json_skip_str(p);
			if (!p || depth == 0)
				return p;
			continue;
		}
		if (c == '{' || c == '[') {
			depth++;
		} else if (c == '}' || c == ']') {
			if (depth == 0)
				return p;	/* ends the enclosing object */
			if (--depth == 0)
				return p + 1;
		} else if (depth == 0 && (c == ',' || c == ' ' || c == '\t' ||
					  c == '\r' || c == '\n')) {
			return p;
		}
		p++;
	}
}

/* Value of a top-level member of the JSON object in json (blanks before
 * it skipped), or NULL. Walks the object member by member, so only keys
 * at depth 1 match: key text inside a string value or a nested object or
 * array is never taken for a key, and cannot shadow the real one. Keys
 * are compared as written (no unescaping); the first match wins. NULL as
 * well if the object is malformed or truncated before the key. */
static const char *json_find(const char *json, const char *key)
{
	size_t klen = strlen(key);
	const char *p = json_ws(json);

	if (*p != '{')
		return NULL;
	p = json_ws(p + 1);
	while (*p == '"') {
		const char *k = p + 1;
		const char *kend = json_skip_str(p);

		if (!kend)
			return NULL;
		int match = (size_t)(kend - 1 - k) == klen &&
			    memcmp(k, key, klen) == 0;
		p = json_ws(kend);
		if (*p != ':')
			return NULL;
		p = json_ws(p + 1);
		if (match)
			return *p ? p : NULL;
		p = json_skip_value(p);
		if (!p)
			return NULL;
		p = json_ws(p);
		if (*p != ',')
			return NULL;
		p = json_ws(p + 1);
	}
	return NULL;
}

/* String member key, unescaped into out (at most n - 1 bytes, always
 * NUL-terminated): a backslash takes the next byte literally. Returns 0
 * if key is missing, not a string, or the string is not terminated. */
static int json_str(const char *json, const char *key, char *out, size_t n)
{
	const char *p = json_find(json, key);
	if (!p || *p != '"' || !json_skip_str(p) || n == 0)
		return 0;
	p++;
	size_t i = 0;
	while (*p != '"' && i < n - 1) {
		if (*p == '\\')
			p++;
		out[i++] = *p++;
	}
	out[i] = '\0';
	return 1;
}

/* Integer member key. Returns 0, leaving *out alone, if key is missing
 * or its value is not a whole decimal integer ending the member (so
 * "5.5", "5x" and a value cut off by the end of input are refused). */
static int json_int(const char *json, const char *key, long *out)
{
	const char *p = json_find(json, key);
	if (!p) return 0;
	char *end;
	errno = 0;
	long v = strtol(p, &end, 10);
	if (end == p || errno == ERANGE) return 0;
	const char *q = json_ws(end);
	if (*q != ',' && *q != '}') return 0;
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
	if (serial_suspended())
		return 0;
	uint8_t c[2] = { CMD_SCREEN_CHANGE, cmd };
	tcflush(g_serial_fd, TCIFLUSH);
	return write(g_serial_fd, c, 2) == 2;
}
static int relay_nvm(uint8_t cmd)
{
	if (serial_suspended())
		return 0;
	uint8_t c[6] = { CMD_NVM_CONFIG, 0x55, 0xAA, 0x55, 0xAA, cmd };
	tcflush(g_serial_fd, TCIFLUSH);
	return write(g_serial_fd, c, 6) == 6;
}
/* status/log are keep-masks: the firmware does fault &= mask. */
static int relay_clear_faults(uint16_t status, uint16_t log)
{
	if (serial_suspended())
		return 0;
	uint8_t c[5] = { CMD_CLEAR_FAULTS, (uint8_t)(status & 0xFF),
			 (uint8_t)(status >> 8), (uint8_t)(log & 0xFF),
			 (uint8_t)(log >> 8) };
	tcflush(g_serial_fd, TCIFLUSH);
	return write(g_serial_fd, c, 5) == 5;
}
static int relay_write_config(const uint8_t *data, int data_len)
{
	if (serial_suspended())
		return 0;
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
	if (serial_suspended())
		return -1;
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
		/* Keep-masks, as the firmware and the GUI use them: fault &= mask,
		 * so 0 clears everything and ~(1 << bit) clears one fault. */
		long s = 0, l = 0;
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

/* recv() that never waits past the deadline. Returns bytes read, 0 on
 * orderly close, -1 on error, or -2 once the deadline has passed. */
static ssize_t recv_deadline(int fd, void *buf, size_t len,
			     const struct timespec *deadline)
{
	for (;;) {
		long left = ms_until(deadline);
		if (left <= 0)
			return -2;

		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, (int)left);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (pr == 0)
			return -2;

		ssize_t r = recv(fd, buf, len, MSG_DONTWAIT);
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			      errno == EINTR))
			continue;
		return r;
	}
}

/* Accept one HTTP connection, route GET /sensors, /metrics, /config and
 * POST /command, close.
 * Reading the request is bounded by HTTP_DEADLINE_MS in total (not per
 * recv), so a slow client cannot stall sensor polling. */
static void http_handle(int http_fd)
{
	struct sockaddr_storage peer;
	socklen_t plen = sizeof(peer);
	/* Left blocking: recv_deadline() polls before every recv and
	 * uses MSG_DONTWAIT, so reads never block. */
	int cfd = accept4(http_fd, (struct sockaddr *)&peer, &plen, SOCK_CLOEXEC);
	if (cfd < 0) return;
	char client_ip[64];
	peer_ip(&peer, plen, client_ip, sizeof(client_ip));

	struct timespec deadline;
	deadline_in(&deadline, HTTP_DEADLINE_MS);

	/* Room for a maximal header block plus a maximal body. */
	static char buf[HTTP_MAX_HEADER + 4 + HTTP_MAX_BODY + 1];
	size_t total = 0;
	char *hdr_end = NULL;
	while (total < HTTP_MAX_HEADER + 4) {
		ssize_t r = recv_deadline(cfd, buf + total,
					  sizeof(buf) - 1 - total, &deadline);
		if (r == -2)
			wlog("WARN", "http from %s: request deadline exceeded", client_ip);
		if (r <= 0) break;
		total += (size_t)r;
		buf[total] = '\0';
		if ((hdr_end = strstr(buf, "\r\n\r\n")) != NULL) break;
	}
	if (!hdr_end || (size_t)(hdr_end - buf) > HTTP_MAX_HEADER) {
		close(cfd);
		return;
	}

	char method[8] = {0}, path[64] = {0};
	if (sscanf(buf, "%7s %63s", method, path) != 2) { close(cfd); return; }

	if (strcmp(method, "GET") == 0 && strcmp(path, "/sensors") == 0) {
		char body[2048];
		int bn = build_sensors_json(body, sizeof(body));
		/* snprintf returns the untruncated length; never send more
		 * than the buffer actually holds. */
		if (bn < 0)
			bn = 0;
		if ((size_t)bn > sizeof(body) - 1)
			bn = (int)(sizeof(body) - 1);
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

	/* Prometheus scrapes: read-only, and not logged, like /sensors. */
	if (strcmp(method, "GET") == 0 && strcmp(path, "/metrics") == 0) {
		static char body[16384];
		struct outbuf ob = { .p = body, .cap = sizeof(body) };

		if (build_metrics(&ob) < 0) {
			http_respond(cfd, 500, "Internal Server Error",
				     "{\"error\":\"metrics overflow\"}");
			close(cfd);
			return;
		}
		char hdr[256];
		int hn = snprintf(hdr, sizeof(hdr),
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
			"Connection: close\r\nContent-Length: %zu\r\n\r\n",
			ob.len);
		(void)!write(cfd, hdr, hn);
		(void)!write(cfd, body, ob.len);
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
			ssize_t r = recv_deadline(cfd, buf + total,
						  sizeof(buf) - 1 - total, &deadline);
			if (r == -2) {
				wlog("WARN", "http from %s: request deadline exceeded", client_ip);
				close(cfd);
				return;
			}
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

/* Try to bring the device up: locate and open the serial port and the
 * hwmon node, then read the device info. Returns 0 with *serial_fd and
 * *hwmon_fd open, or the number of ms to wait before the next attempt.
 * Never sleeps, so the caller's poll loop keeps serving the command
 * socket and the HTTP listener while the device is absent. */
static long device_connect(const char *user_dev_path, char *dev_path,
			   size_t dev_path_len, int *serial_fd, int *hwmon_fd)
{
	int sfd, hfd;

	/* Use the -d path every time; otherwise auto-detect. Clearing
	 * dev_path below only forgets an auto-detected path. */
	if (user_dev_path[0] != '\0') {
		snprintf(dev_path, dev_path_len, "%s", user_dev_path);
	} else if (dev_path[0] == '\0') {
		if (find_device(dev_path, dev_path_len) < 0) {
			fprintf(stderr, "wireviewd: device not found, retrying in 5s\n");
			dev_path[0] = '\0';
			return 5000;
		}
	}

	/* Check hwmon module is loaded */
	if (access(HWMON_DEV, W_OK) < 0) {
		fprintf(stderr, "wireviewd: %s not available (load wireview_hwmon module)\n",
			HWMON_DEV);
		return 5000;
	}

	printf("wireviewd: using %s\n", dev_path);

	sfd = open_serial(dev_path);
	if (sfd < 0) {
		fprintf(stderr, "wireviewd: failed to open %s: %s\n",
			dev_path, strerror(errno));
		dev_path[0] = '\0';
		return 5000;
	}

	hfd = open(HWMON_DEV, O_WRONLY);
	if (hfd < 0) {
		fprintf(stderr, "wireviewd: failed to open %s: %s\n",
			HWMON_DEV, strerror(errno));
		close(sfd);
		return 5000;
	}

	tcflush(sfd, TCIFLUSH);

	/* Query device info */
	if (query_device_info(sfd) < 0) {
		fprintf(stderr, "wireviewd: device info query failed\n");
		close(hfd);
		close(sfd);
		dev_path[0] = '\0';
		return 2000;
	}

	*serial_fd = sfd;
	*hwmon_fd = hfd;
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-i interval_ms] [-d /dev/ttyACMx] [-V]\n", prog);
	fprintf(stderr, "  -i  Poll interval in milliseconds (default: 1000)\n");
	fprintf(stderr, "  -d  Serial device path (default: auto-detect)\n");
	fprintf(stderr, "  -V  Print version and exit\n");
	exit(1);
}

int main(int argc, char **argv)
{
	int interval_ms = 1000;
	char user_dev_path[256] = "";	/* -d: re-used on every reconnect */
	char dev_path[256] = "";	/* path in use this connection */
	int opt;

	/* stdout is block-buffered when not a tty (systemd); make status
	 * lines reach the journal promptly and in order with stderr. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	while ((opt = getopt(argc, argv, "i:d:hV")) != -1) {
		switch (opt) {
		case 'i':
			interval_ms = atoi(optarg);
			if (interval_ms < 100 || interval_ms > 10000) {
				fprintf(stderr, "Interval must be 100-10000 ms\n");
				return 1;
			}
			break;
		case 'd':
			snprintf(user_dev_path, sizeof(user_dev_path), "%s", optarg);
			break;
		case 'V':
			printf("wireviewd %s\n", WIREVIEW_PKG_VERSION);
			return 0;
		default:
			usage(argv[0]);
		}
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	/* A peer that hangs up before its reply must not kill the daemon
	 * (systemd already ignores SIGPIPE; this covers manual runs). */
	signal(SIGPIPE, SIG_IGN);

	load_config();
	resolve_wireview_group();

	int http_fd = -1;
	char listen_desc[256] = "";
	if (g_http_enabled) {
		http_fd = setup_http(listen_desc, sizeof(listen_desc));
		if (http_fd < 0) {
			fprintf(stderr, "wireviewd: warning: network listener on %s failed: %s\n",
				listen_desc, strerror(errno));
			wlog("WARN", "network listener on %s failed: %s",
			     listen_desc, strerror(errno));
		} else {
			printf("wireviewd: network listener ENABLED on %s; remote writes %s\n",
			       listen_desc, g_secret[0] ? "enabled (secret set)" : "disabled (no secret)");
		}
	} else {
		printf("wireviewd: network listener disabled (set remote_enabled=1 in %s to publish)\n",
		       CONFIG_PATH);
	}

	wlog("INFO", "wireviewd %s started; listener %s%s%s, remote writes %s, log retention %d days",
	     WIREVIEW_PKG_VERSION,
	     !g_http_enabled ? "disabled" : http_fd >= 0 ? "enabled" : "failed",
	     http_fd >= 0 ? " on " : "", http_fd >= 0 ? listen_desc : "",
	     g_secret[0] ? "enabled" : "disabled", g_log_retain_days);

	printf("wireviewd %s: starting\n", WIREVIEW_PKG_VERSION);

	/* The command socket lives for the whole run: a device disconnect
	 * no longer drops it, so clients (the GUI's long-lived connection
	 * in particular) stay connected and get RESP_NOT_CONNECTED until
	 * the device is back. */
	int sock_fd = setup_socket();
	if (sock_fd < 0)
		fprintf(stderr, "wireviewd: warning: could not create socket\n");

	struct client clients[MAX_CLIENTS];
	int num_clients = 0;
	int serial_fd = -1;
	int hwmon_fd = -1;

	/* Fault bits seen in the previous frame (debounce state),
	 * reset on every (re)connect. */
	uint16_t prev_status = 0, prev_log = 0;

	/* next_poll is the next sensor read while connected, next_reconnect
	 * the next connection attempt while not. First attempt: now. */
	struct timespec next_poll, next_reconnect;

	memset(clients, 0, sizeof(clients));
	clock_gettime(CLOCK_MONOTONIC, &next_reconnect);
	next_poll = next_reconnect;

	/* Single event loop with poll(), device present or not */
	while (running) {
		if (serial_fd < 0 && ms_until(&next_reconnect) <= 0) {
			long retry = device_connect(user_dev_path, dev_path,
						    sizeof(dev_path),
						    &serial_fd, &hwmon_fd);
			if (retry > 0) {
				deadline_in(&next_reconnect, retry);
			} else {
				/* Device is ready; allow HTTP command relay. */
				g_serial_fd = serial_fd;
				for (int i = 0; i < 12; i++)
					snprintf(g_dev_uid + i * 2, 3, "%02X",
						 dev_info.uid[i]);
				prev_status = 0;
				prev_log = 0;
				g_hwmon_v2 = 0;
				printf("wireviewd: polling every %d ms\n", interval_ms);
				clock_gettime(CLOCK_MONOTONIC, &next_poll);
			}
			if (!running)
				break;
		}

		struct pollfd pfds[2 + MAX_CLIENTS];
		int nfds = 0;

		/* Listening socket */
		int sock_pfd_idx = -1;
		if (sock_fd >= 0) {
			sock_pfd_idx = nfds;
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
			pfds[nfds].fd = clients[i].fd;
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		/* Time until the next sensor poll, or the next connection
		 * attempt while the device is absent ... */
		long wait_ms = ms_until(serial_fd >= 0 ? &next_poll
						       : &next_reconnect);
		/* ... or until the earliest partial request expires */
		for (int i = 0; i < num_clients; i++) {
			if (clients[i].have > 0) {
				long left = ms_until(&clients[i].req_deadline);
				if (left < wait_ms)
					wait_ms = left;
			}
		}
		if (wait_ms < 0) wait_ms = 0;

		int ret = poll(pfds, nfds, (int)wait_ms);
		if (ret < 0) {
			/* revents are not valid after a failed poll(). EINTR
			 * is a signal (SIGTERM ends the loop via running);
			 * anything else is unexpected, so do not spin on it. */
			if (errno != EINTR) {
				fprintf(stderr, "wireviewd: poll: %s\n", strerror(errno));
				usleep(100000);
			}
			continue;
		}

		/* Accept new clients */
		if (sock_pfd_idx >= 0 && (pfds[sock_pfd_idx].revents & POLLIN)) {
			int new_fd = accept4(sock_fd, NULL, NULL,
					     SOCK_NONBLOCK | SOCK_CLOEXEC);
			if (new_fd >= 0) {
				if (num_clients < MAX_CLIENTS) {
					client_init(&clients[num_clients++],
						    new_fd);
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

		/* Handle client requests (serial_fd is -1 while the device
		 * is absent, and requests get RESP_NOT_CONNECTED). Walk
		 * backwards: removing client i swaps the last client into
		 * slot i, and that one has already been handled (or was
		 * accepted this round and has no pfd), so slot i is never
		 * re-read against the removed client's revents. */
		for (int i = num_clients - 1; i >= 0; i--) {
			int idx = client_pfd_start + i;
			if (idx >= nfds || !pfds[idx].revents)
				continue;
			int drop = 0;
			if (pfds[idx].revents & POLLIN)
				drop = client_read(&clients[i],
						   serial_fd) < 0;
			if (drop || (pfds[idx].revents &
				     (POLLHUP | POLLERR))) {
				close(clients[i].fd);
				clients[i] = clients[--num_clients];
				clients[num_clients].fd = -1;
			}
		}

		/* Drop clients sitting on an incomplete request */
		for (int i = 0; i < num_clients; ) {
			if (clients[i].have > 0 &&
			    ms_until(&clients[i].req_deadline) <= 0) {
				wlog("WARN", "socket client uid %ld: request incomplete after %d ms (%zu bytes), disconnecting",
				     client_uid(&clients[i]),
				     CLIENT_REQ_TIMEOUT_MS,
				     clients[i].have);
				close(clients[i].fd);
				clients[i] = clients[--num_clients];
				clients[num_clients].fd = -1;
				continue;
			}
			i++;
		}

		if (serial_fd < 0)
			continue;

		/* Sensor poll */
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > next_poll.tv_sec ||
		    (now.tv_sec == next_poll.tv_sec &&
		     now.tv_nsec >= next_poll.tv_nsec)) {

			static int was_suspended;
			if (serial_suspended()) {
				was_suspended = 1;
				next_poll.tv_sec = now.tv_sec;
				next_poll.tv_nsec = now.tv_nsec +
						    (long)interval_ms * 1000000;
				if (next_poll.tv_nsec >= 1000000000L) {
					next_poll.tv_sec +=
						next_poll.tv_nsec / 1000000000L;
					next_poll.tv_nsec %= 1000000000L;
				}
				continue;
			}
			if (was_suspended) {
				/* Drop whatever the GUI's traffic left in the
				 * line discipline before reading frames. */
				tcflush(serial_fd, TCIFLUSH);
				was_suspended = 0;
			}

			struct sensor_struct ss;
			if (read_sensors(serial_fd, &ss) < 0) {
				fprintf(stderr,
					"wireviewd: read failed, reconnecting\n");
				goto disconnect;
			}

			/* A desynced read (no framing or CRC on the wire)
			 * is handled in two layers:
			 *
			 * 1. Discard frames that fail frame_is_sane().
			 * 2. Debounce fault bits - real faults persist (the
			 *    device latches them in fault_log until
			 *    cleared), so publish a bit only if it was set
			 *    in both the previous and the current frame.
			 *    fault_status and fault_log are debounced
			 *    independently and per bit, so a bit already
			 *    latched in the log cannot let a corrupt
			 *    status word (or vice versa) through. */
			if (!frame_is_sane(&ss)) {
				wlog("WARN",
				     "discarded corrupt sensor frame (fan=%u pad1=%u pad2=%u status=0x%04x log=0x%04x)",
				     ss.fan_duty, ss._pad1, ss._pad2,
				     ss.fault_status, ss.fault_log);
			} else {
				uint16_t raw_status = ss.fault_status;
				uint16_t raw_log = ss.fault_log;
				uint16_t new_status = raw_status & ~prev_status;
				uint16_t new_log = raw_log & ~prev_log;

				if (new_status || new_log)
					wlog("WARN",
					     "suppressed unconfirmed fault bits: status=0x%04x log=0x%04x",
					     new_status, new_log);
				ss.fault_status = raw_status & prev_status;
				ss.fault_log = raw_log & prev_log;
				prev_status = raw_status;
				prev_log = raw_log;

				/* Allow a few missed polls, never less than 5 s. */
				energy_accumulate(&ss, interval_ms * 3 > 5000 ?
						       interval_ms * 3 : 5000);

				if (write_hwmon(hwmon_fd, &ss) < 0) {
					fprintf(stderr,
						"wireviewd: hwmon write failed\n");
					goto disconnect;
				}

				memcpy(&g_last, &ss, sizeof(g_last));
				g_have_last = 1;
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
		continue;

disconnect:
		/* Device gone: close it, keep the command socket and its
		 * clients, and retry from the poll loop in 2 s. */
		g_serial_fd = -1;
		close(hwmon_fd);
		close(serial_fd);
		hwmon_fd = -1;
		serial_fd = -1;
		dev_path[0] = '\0';
		g_have_last = 0;
		dev_info.valid = 0;
		/* The next device's first frame restarts the energy clock. */
		g_energy_ts_valid = 0;
		/* A serial handover belonged to the old connection; do not
		 * let it hold off polling on the next one. */
		g_suspend_until.tv_sec = 0;
		g_suspend_until.tv_nsec = 0;
		deadline_in(&next_reconnect, 2000);
	}

	/* Cleanup */
	for (int i = 0; i < num_clients; i++) {
		if (clients[i].fd >= 0) close(clients[i].fd);
	}
	cleanup_socket(sock_fd);

	if (serial_fd >= 0) {
		g_serial_fd = -1;
		close(hwmon_fd);
		close(serial_fd);
	}

	if (http_fd >= 0)
		close(http_fd);

	printf("wireviewd: stopped\n");
	return 0;
}
