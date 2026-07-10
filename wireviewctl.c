/*
 * wireviewctl - CLI tool for the WireView Pro II daemon
 *
 * Connects to the wireviewd Unix socket to send commands,
 * or reads hwmon sysfs attributes for sensor data.
 *
 * Usage: wireviewctl <command> [args]
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <linux/limits.h>

#define SOCK_PATH "/run/wireviewd.sock"

/* Socket protocol command types (must match wireviewd) */
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

static int sock_connect(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (errno == ENOENT || errno == ECONNREFUSED)
			fprintf(stderr, "wireviewctl: cannot connect to daemon at %s\n"
				"Is wireviewd running?\n", SOCK_PATH);
		else
			perror("connect");
		close(fd);
		return -1;
	}

	/* Set 3 second timeout */
	struct timeval tv = { .tv_sec = 3 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	return fd;
}

/* Send request and receive response. Returns 0 on success.
 * On success, *resp_buf is malloc'd and must be freed, *resp_len is set.
 * On failure, *resp_buf is NULL. */
static int sock_command(uint8_t cmd, const void *payload, uint16_t payload_len,
			uint8_t **resp_buf, uint16_t *resp_len)
{
	int fd = sock_connect();
	if (fd < 0)
		return -1;

	/* Send: [cmd:1][len:2 LE][payload] */
	uint8_t hdr[3] = { cmd, payload_len & 0xFF, payload_len >> 8 };
	if (write(fd, hdr, 3) != 3 ||
	    (payload_len > 0 && write(fd, payload, payload_len) != payload_len)) {
		perror("write");
		close(fd);
		return -1;
	}

	/* Receive: [status:1][len:2 LE][data] */
	uint8_t rhdr[3];
	ssize_t n = 0, off = 0;
	while (off < 3) {
		n = read(fd, rhdr + off, 3 - off);
		if (n <= 0) {
			perror("read header");
			close(fd);
			return -1;
		}
		off += n;
	}

	uint8_t status = rhdr[0];
	uint16_t rlen = rhdr[1] | (rhdr[2] << 8);

	uint8_t *data = NULL;
	if (rlen > 0) {
		data = malloc(rlen);
		if (!data) {
			close(fd);
			return -1;
		}
		off = 0;
		while (off < rlen) {
			n = read(fd, data + off, rlen - off);
			if (n <= 0) {
				perror("read data");
				free(data);
				close(fd);
				return -1;
			}
			off += n;
		}
	}

	close(fd);

	if (status == RESP_NOT_CONNECTED) {
		fprintf(stderr, "wireviewctl: device not connected\n");
		free(data);
		return -1;
	}
	if (status == RESP_ERROR) {
		fprintf(stderr, "wireviewctl: command failed\n");
		free(data);
		return -1;
	}

	*resp_buf = data;
	*resp_len = rlen;
	return 0;
}

/* ---------- Subcommands ---------- */

static int cmd_info(void)
{
	uint8_t *data = NULL;
	uint16_t len = 0;

	if (sock_command(WCMD_GET_DEVICE_INFO, NULL, 0, &data, &len) < 0)
		return 1;

	if (len < 14) {
		fprintf(stderr, "wireviewctl: unexpected response length %u\n", len);
		free(data);
		return 1;
	}

	uint8_t fw = data[0];
	uint8_t cfg_ver = data[1];
	printf("firmware: %u\n", fw);
	printf("config_version: %u\n", cfg_ver);

	printf("uid: ");
	for (int i = 0; i < 12; i++)
		printf("%02x", data[2 + i]);
	printf("\n");

	if (len > 14) {
		/* Build string follows UID, null-terminated */
		printf("build: %.*s\n", len - 14, (char *)(data + 14));
	}

	free(data);
	return 0;
}

static int cmd_clear_faults(void)
{
	/* Mask 0 = clear all faults */
	uint8_t payload[4] = { 0, 0, 0, 0 };
	uint8_t *data = NULL;
	uint16_t len = 0;

	if (sock_command(WCMD_CLEAR_FAULTS, payload, 4, &data, &len) < 0)
		return 1;

	printf("faults cleared\n");
	free(data);
	return 0;
}

static int cmd_read_config(void)
{
	uint8_t *data = NULL;
	uint16_t len = 0;

	if (sock_command(WCMD_READ_CONFIG, NULL, 0, &data, &len) < 0)
		return 1;

	if (len < 2) {
		fprintf(stderr, "wireviewctl: empty config response\n");
		free(data);
		return 1;
	}

	/* First byte is config_version, rest is raw config */
	uint8_t cfg_ver = data[0];
	fprintf(stderr, "config_version: %u, size: %u bytes\n", cfg_ver, len - 1);

	for (uint16_t i = 1; i < len; i++)
		printf("%02x", data[i]);
	printf("\n");

	free(data);
	return 0;
}

static int cmd_write_config(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		perror(path);
		return 1;
	}

	/* Read hex string from file */
	char hexbuf[2048];
	if (!fgets(hexbuf, sizeof(hexbuf), f)) {
		fprintf(stderr, "wireviewctl: empty config file\n");
		fclose(f);
		return 1;
	}
	fclose(f);

	/* Strip trailing whitespace */
	size_t slen = strlen(hexbuf);
	while (slen > 0 && (hexbuf[slen - 1] == '\n' || hexbuf[slen - 1] == '\r' ||
			     hexbuf[slen - 1] == ' '))
		hexbuf[--slen] = '\0';

	if (slen == 0 || slen % 2 != 0) {
		fprintf(stderr, "wireviewctl: invalid hex data (length %zu)\n", slen);
		return 1;
	}

	size_t nbytes = slen / 2;
	/* payload: [config_version:1][config_data] */
	/* Determine config version from data size.
	 * V0 config = 72 bytes, V1 config = 74 bytes. */
	uint8_t cfg_ver;
	if (nbytes == 72)
		cfg_ver = 0;
	else if (nbytes == 74)
		cfg_ver = 1;
	else if (nbytes == 96)
		cfg_ver = 2;
	else {
		fprintf(stderr, "wireviewctl: unexpected config size %zu bytes "
			"(expected 72 for v0, 74 for v1, or 96 for v2)\n", nbytes);
		return 1;
	}

	uint8_t *payload = malloc(1 + nbytes);
	if (!payload)
		return 1;

	payload[0] = cfg_ver;
	for (size_t i = 0; i < nbytes; i++) {
		unsigned int byte;
		if (sscanf(hexbuf + i * 2, "%2x", &byte) != 1) {
			fprintf(stderr, "wireviewctl: invalid hex at offset %zu\n", i * 2);
			free(payload);
			return 1;
		}
		payload[1 + i] = (uint8_t)byte;
	}

	uint8_t *resp = NULL;
	uint16_t rlen = 0;
	int rc = sock_command(WCMD_WRITE_CONFIG, payload, 1 + nbytes, &resp, &rlen);
	free(payload);
	free(resp);

	if (rc < 0)
		return 1;

	printf("config written (%zu bytes, version %u)\n", nbytes, cfg_ver);
	return 0;
}

struct name_val {
	const char *name;
	uint8_t val;
};

static const struct name_val screen_cmds[] = {
	{ "main",    0xE0 },
	{ "simple",  0xE1 },
	{ "current", 0xE2 },
	{ "temp",    0xE3 },
	{ "status",  0xE4 },
	{ "same",    0xEF },
	{ "pause",   0xF0 },
	{ "resume",  0xF1 },
	{ NULL, 0 }
};

static int cmd_screen(const char *name)
{
	for (const struct name_val *s = screen_cmds; s->name; s++) {
		if (strcmp(name, s->name) == 0) {
			uint8_t payload = s->val;
			uint8_t *resp = NULL;
			uint16_t rlen = 0;
			if (sock_command(WCMD_SCREEN_CMD, &payload, 1, &resp, &rlen) < 0)
				return 1;
			free(resp);
			printf("screen: %s\n", name);
			return 0;
		}
	}
	fprintf(stderr, "wireviewctl: unknown screen command '%s'\n"
		"Valid: main, simple, current, temp, status, same, pause, resume\n", name);
	return 1;
}

static const struct name_val nvm_cmds[] = {
	{ "load",              1 },
	{ "store",             2 },
	{ "reset",             3 },
	{ "load-cal",          4 },
	{ "store-cal",         5 },
	{ "load-cal-factory",  6 },
	{ "store-cal-factory", 7 },
	{ NULL, 0 }
};

static int cmd_nvm(const char *name)
{
	for (const struct name_val *s = nvm_cmds; s->name; s++) {
		if (strcmp(name, s->name) == 0) {
			uint8_t payload = s->val;
			uint8_t *resp = NULL;
			uint16_t rlen = 0;
			if (sock_command(WCMD_NVM_CMD, &payload, 1, &resp, &rlen) < 0)
				return 1;
			free(resp);
			printf("nvm: %s\n", name);
			return 0;
		}
	}
	fprintf(stderr, "wireviewctl: unknown nvm command '%s'\n"
		"Valid: load, store, reset, load-cal, store-cal, "
		"load-cal-factory, store-cal-factory\n", name);
	return 1;
}

static int cmd_build(void)
{
	uint8_t *data = NULL;
	uint16_t len = 0;

	if (sock_command(WCMD_READ_BUILD, NULL, 0, &data, &len) < 0)
		return 1;

	if (len > 0)
		printf("build: %.*s\n", len, (char *)data);
	else
		printf("build: (empty)\n");

	free(data);
	return 0;
}

static int cmd_bootloader(void)
{
	uint8_t *resp = NULL;
	uint16_t rlen = 0;

	if (sock_command(WCMD_ENTER_BOOTLOADER, NULL, 0, &resp, &rlen) < 0)
		return 1;

	free(resp);
	printf("device entering bootloader mode\n");
	return 0;
}

/* ---------- Firmware flashing (DFU via dfu-util) ---------- */

/* Load .hex (Intel HEX) or raw .bin into a flat image. Returns image length,
 * sets *base_out to the image base address (0x08000000 assumed for .bin) and
 * *version_out to the BuildStruct firmware version byte (-1 if unknown). */
static long load_firmware(const char *path, uint8_t **img_out, uint32_t *base_out,
			  int *version_out, char *build_out, size_t build_cap)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "wireviewctl: cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	int c = fgetc(f);
	rewind(f);
	*version_out = -1;
	if (build_out && build_cap) build_out[0] = '\0';

	uint8_t *img = NULL;
	long len = -1;
	uint32_t base = 0x08000000u;

	if (c == ':') {
		/* Intel HEX: two passes (find range, then fill). */
		char line[600];
		uint32_t upper = 0, minaddr = 0xFFFFFFFFu, maxaddr = 0;
		int linear = 0;
		for (int pass = 0; pass < 2; pass++) {
			rewind(f);
			upper = 0; linear = 0;
			while (fgets(line, sizeof(line), f)) {
				char *t = line;
				while (*t == ' ' || *t == '\r' || *t == '\n') t++;
				if (*t != ':' || strlen(t) < 11) continue;
				unsigned cnt, addr, typ;
				if (sscanf(t + 1, "%2x%4x%2x", &cnt, &addr, &typ) != 3) continue;
				if (typ == 1) break;
				if (typ == 2 || typ == 4) {
					unsigned v;
					if (sscanf(t + 9, "%4x", &v) != 1) continue;
					upper = (typ == 4) ? (uint32_t)v << 16 : (uint32_t)v << 4;
					linear = (typ == 4);
					(void)linear;
					continue;
				}
				if (typ != 0) continue;
				for (unsigned i = 0; i < cnt; i++) {
					unsigned b;
					if (sscanf(t + 9 + i * 2, "%2x", &b) != 1) break;
					uint32_t a = upper + addr + i;
					if (pass == 0) {
						if (a < minaddr) minaddr = a;
						if (a > maxaddr) maxaddr = a;
					} else {
						img[a - minaddr] = (uint8_t)b;
					}
				}
			}
			if (pass == 0) {
				if (minaddr > maxaddr || maxaddr - minaddr >= 4u * 1024 * 1024) {
					fprintf(stderr, "wireviewctl: invalid or oversized hex image\n");
					fclose(f);
					return -1;
				}
				len = (long)(maxaddr - minaddr + 1);
				img = malloc((size_t)len);
				if (!img) { fclose(f); return -1; }
				memset(img, 0xFF, (size_t)len);
				base = minaddr;
			}
		}
	} else {
		fseek(f, 0, SEEK_END);
		len = ftell(f);
		rewind(f);
		if (len <= 0 || len >= 4L * 1024 * 1024) {
			fprintf(stderr, "wireviewctl: invalid firmware size\n");
			fclose(f);
			return -1;
		}
		img = malloc((size_t)len);
		if (!img || fread(img, 1, (size_t)len, f) != (size_t)len) {
			fprintf(stderr, "wireviewctl: read failed\n");
			free(img);
			fclose(f);
			return -1;
		}
	}
	fclose(f);

	/* BuildStruct: version byte at image+194, 32-byte build string at +227. */
	if (len > 194 + 1)
		*version_out = img[194];
	if (build_out && build_cap && len > 227 + 32) {
		size_t n = 0;
		while (n < 32 && n + 1 < build_cap && img[227 + n] != 0) {
			build_out[n] = (char)img[227 + n];
			n++;
		}
		build_out[n] = '\0';
	}

	*img_out = img;
	*base_out = base;
	return len;
}

static int dfu_device_present(void)
{
	FILE *p = popen("dfu-util -l 2>/dev/null", "r");
	if (!p) return 0;
	char line[512];
	int found = 0;
	while (fgets(line, sizeof(line), p))
		if (strstr(line, "[0483:df11]")) found = 1;
	pclose(p);
	return found;
}

/* Firmware image bundled with the wireview-hwmon package ("make install"
 * and all distro packages place it here). Used when "flash" is given no
 * file argument, so "wireviewctl flash -y" is a complete headless update. */
#define DEFAULT_FIRMWARE_PATH "/usr/share/wireview/TG-WV-PRO2-FW.hex"

static int cmd_flash(const char *path, int yes)
{
	if (system("dfu-util --version >/dev/null 2>&1") != 0) {
		fprintf(stderr, "wireviewctl: dfu-util not found; install the dfu-util package\n");
		return 1;
	}

	uint8_t *img = NULL;
	uint32_t base = 0;
	int version = -1;
	char build[40];
	long len = load_firmware(path, &img, &base, &version, build, sizeof(build));
	if (len < 0)
		return 1;

	printf("firmware image: %s (%ld bytes, base 0x%08X)\n", path, len, base);
	if (version >= 0)
		printf("image version:  v%02d%s%s%s\n", version,
		       build[0] ? " (" : "", build, build[0] ? ")" : "");

	if (!yes) {
		printf("Unofficial tool, not affiliated with Thermal Grizzly: flash at your own risk.\n"
		       "Flash this image to the device? Do not power off during the update. [y/N] ");
		fflush(stdout);
		char answer[8];
		if (!fgets(answer, sizeof(answer), stdin)
		    || (answer[0] != 'y' && answer[0] != 'Y')) {
			fprintf(stderr, "aborted\n");
			free(img);
			return 1;
		}
	}

	if (!dfu_device_present()) {
		uint8_t *resp = NULL;
		uint16_t rlen = 0;
		printf("entering bootloader...\n");
		if (sock_command(WCMD_ENTER_BOOTLOADER, NULL, 0, &resp, &rlen) < 0)
			fprintf(stderr, "warning: could not reach wireviewd; waiting for a "
					"manually started DFU bootloader\n");
		free(resp);

		int waited = 0;
		while (!dfu_device_present() && waited < 25) {
			sleep(1);
			waited++;
		}
		if (!dfu_device_present()) {
			fprintf(stderr, "wireviewctl: DFU bootloader (0483:df11) did not appear; "
					"check the udev rules and connection\n");
			free(img);
			return 1;
		}
	}

	char tmp[] = "/tmp/wireviewctl-fw-XXXXXX";
	int fd = mkstemp(tmp);
	if (fd < 0 || write(fd, img, (size_t)len) != (ssize_t)len) {
		fprintf(stderr, "wireviewctl: temp file write failed\n");
		if (fd >= 0) { close(fd); unlink(tmp); }
		free(img);
		return 1;
	}
	close(fd);
	free(img);

	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "dfu-util -d 0483:df11 -a 0 -s 0x%08X:leave -D %s 2>&1", base, tmp);
	printf("running: %s\n", cmd);

	/* dfu-util exits non-zero when the device detaches right after ":leave"
	 * even though the download finished; scan its output for the success
	 * marker instead of trusting the exit code. */
	FILE *proc = popen(cmd, "r");
	if (!proc) {
		fprintf(stderr, "wireviewctl: failed to run dfu-util\n");
		unlink(tmp);
		return 1;
	}
	char line[512];
	int downloaded = 0;
	while (fgets(line, sizeof(line), proc)) {
		fputs(line, stdout);
		if (strstr(line, "File downloaded successfully"))
			downloaded = 1;
	}
	int rc = pclose(proc);
	unlink(tmp);

	if (downloaded) {
		printf("flash complete; the device is rebooting into the new firmware\n");
		return 0;
	}
	fprintf(stderr, "wireviewctl: flash failed (dfu-util exit status %d)\n", rc);
	return 1;
}

/* ---------- Sensors (sysfs, no daemon needed) ---------- */

static int find_hwmon_path(char *buf, size_t bufsize)
{
	DIR *dir = opendir("/sys/class/hwmon");
	if (!dir)
		return -1;

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char namepath[PATH_MAX];
		snprintf(namepath, sizeof(namepath), "/sys/class/hwmon/%s/name", ent->d_name);

		FILE *f = fopen(namepath, "r");
		if (!f)
			continue;

		char name[64];
		if (fgets(name, sizeof(name), f)) {
			/* Strip newline */
			size_t l = strlen(name);
			if (l > 0 && name[l - 1] == '\n')
				name[l - 1] = '\0';

			if (strcmp(name, "wireview") == 0) {
				snprintf(buf, bufsize, "/sys/class/hwmon/%s", ent->d_name);
				fclose(f);
				closedir(dir);
				return 0;
			}
		}
		fclose(f);
	}

	closedir(dir);
	return -1;
}

static int read_sysfs_int(const char *hwmon, const char *attr, long *val)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", hwmon, attr);

	FILE *f = fopen(path, "r");
	if (!f)
		return -1;

	int rc = (fscanf(f, "%ld", val) == 1) ? 0 : -1;
	fclose(f);
	return rc;
}

static int cmd_sensors(void)
{
	char hwmon[PATH_MAX];
	if (find_hwmon_path(hwmon, sizeof(hwmon)) < 0) {
		fprintf(stderr, "wireviewctl: wireview hwmon device not found\n"
			"Is the wireview_hwmon module loaded?\n");
		return 1;
	}

	long val;

	/* Voltages: in0-in5 (pins), in6 (avg), in7 (vdd) */
	const char *vlabels[] = {
		"pin1_voltage_mv", "pin2_voltage_mv", "pin3_voltage_mv",
		"pin4_voltage_mv", "pin5_voltage_mv", "pin6_voltage_mv",
		"avg_voltage_mv", "vdd_mv"
	};
	for (int i = 0; i < 8; i++) {
		char attr[32];
		snprintf(attr, sizeof(attr), "in%d_input", i);
		if (read_sysfs_int(hwmon, attr, &val) == 0)
			printf("%s: %ld\n", vlabels[i], val);
	}

	/* Currents: curr1-curr6 (pins), curr7 (total) */
	const char *clabels[] = {
		"pin1_current_ma", "pin2_current_ma", "pin3_current_ma",
		"pin4_current_ma", "pin5_current_ma", "pin6_current_ma",
		"total_current_ma"
	};
	for (int i = 0; i < 7; i++) {
		char attr[32];
		snprintf(attr, sizeof(attr), "curr%d_input", i + 1);
		if (read_sysfs_int(hwmon, attr, &val) == 0)
			printf("%s: %ld\n", clabels[i], val);
	}

	/* Power: power1 (total), power2-power7 (pins) */
	if (read_sysfs_int(hwmon, "power1_input", &val) == 0)
		printf("total_power_uw: %ld\n", val);
	const char *plabels[] = {
		"pin1_power_uw", "pin2_power_uw", "pin3_power_uw",
		"pin4_power_uw", "pin5_power_uw", "pin6_power_uw"
	};
	for (int i = 0; i < 6; i++) {
		char attr[32];
		snprintf(attr, sizeof(attr), "power%d_input", i + 2);
		if (read_sysfs_int(hwmon, attr, &val) == 0)
			printf("%s: %ld\n", plabels[i], val);
	}

	/* Temperatures */
	const char *tlabels[] = {
		"temp_onboard_in_mc", "temp_onboard_out_mc",
		"temp_external1_mc", "temp_external2_mc"
	};
	for (int i = 0; i < 4; i++) {
		char attr[32];
		snprintf(attr, sizeof(attr), "temp%d_input", i + 1);
		if (read_sysfs_int(hwmon, attr, &val) == 0)
			printf("%s: %ld\n", tlabels[i], val);
	}

	/* Fan duty */
	if (read_sysfs_int(hwmon, "fan1_input", &val) == 0)
		printf("fan_duty: %ld\n", val);

	/* Raw fault/log/psu from extended sysfs attrs */
	if (read_sysfs_int(hwmon, "fault_status_raw", &val) == 0)
		printf("fault_status: %ld\n", val);
	else if (read_sysfs_int(hwmon, "intrusion0_alarm", &val) == 0)
		printf("fault_status: %ld\n", val);

	if (read_sysfs_int(hwmon, "fault_log_raw", &val) == 0)
		printf("fault_log: %ld\n", val);
	else if (read_sysfs_int(hwmon, "intrusion1_alarm", &val) == 0)
		printf("fault_log: %ld\n", val);

	if (read_sysfs_int(hwmon, "psu_cap", &val) == 0) {
		const char *psu_names[] = { "600W", "450W", "300W", "150W" };
		if (val >= 0 && val <= 3)
			printf("psu_cap: %s\n", psu_names[val]);
		else
			printf("psu_cap: %ld\n", val);
	}

	return 0;
}

/* ---------- Usage ---------- */

/* ---------- top: live monitor (local sysfs + remote /sensors) ---------- */

#define WV_MAXDEV   32
#define WV_MAXHOST  32
#define TEMP_NA     -999.0

/* UTF-8 glyphs + ANSI (explicit bytes so any compiler is happy) */
#define ESC   "\033"
#define BLK   "\xe2\x96\x88"   /* full block  */
#define SHADE "\xe2\x96\x91"   /* light shade */
#define TL    "\xe2\x95\xad"   /* round corner top-left */
#define BL    "\xe2\x95\xb0"   /* round corner bottom-left */
#define HR    "\xe2\x94\x80"   /* horizontal */
#define VB    "\xe2\x94\x82"   /* vertical */
#define DEG   "\xc2\xb0"       /* degree */

struct wv_snap {
	char     source[72];   /* "local" or "host[:port]" */
	char     name[40];
	char     fw[12];
	int      ok;
	double   pin_v[6], pin_c[6];
	double   temp[4];      /* TEMP_NA if absent */
	int      psu_cap_w;
	int      fan;          /* duty %, -1 if unavailable */
	unsigned fault_status, fault_log;
	double   sum_w, sum_a;
};

/* Best-effort: ask the daemon (if running) for the local device's fw version.
 * Silent on any failure so it never disturbs the TUI; sysfs has no fw attribute. */
static void get_local_fw(char *out, size_t n)
{
	out[0] = '\0';
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
		uint8_t hdr[3] = { WCMD_GET_DEVICE_INFO, 0, 0 };
		uint8_t rh[3];
		if (write(fd, hdr, 3) == 3 && read(fd, rh, 3) == 3 &&
		    rh[0] == RESP_OK && (rh[1] | (rh[2] << 8)) >= 1) {
			uint8_t fw;
			if (read(fd, &fw, 1) == 1)
				snprintf(out, n, "%u", fw);
		}
	}
	close(fd);
}

static int read_local(struct wv_snap *s)
{
	char hwmon[PATH_MAX];
	if (find_hwmon_path(hwmon, sizeof(hwmon)) < 0)
		return -1;

	memset(s, 0, sizeof(*s));
	snprintf(s->source, sizeof(s->source), "local");
	snprintf(s->name, sizeof(s->name), "WireView Pro II");

	long v;
	char a[24];
	for (int i = 0; i < 6; i++) {
		snprintf(a, sizeof(a), "in%d_input", i);
		s->pin_v[i] = read_sysfs_int(hwmon, a, &v) == 0 ? v / 1000.0 : 0;
		snprintf(a, sizeof(a), "curr%d_input", i + 1);
		s->pin_c[i] = read_sysfs_int(hwmon, a, &v) == 0 ? v / 1000.0 : 0;
	}
	s->sum_a = read_sysfs_int(hwmon, "curr7_input", &v) == 0 ? v / 1000.0 : 0;
	s->sum_w = read_sysfs_int(hwmon, "power1_input", &v) == 0 ? v / 1000000.0 : 0;
	for (int i = 0; i < 4; i++) {
		snprintf(a, sizeof(a), "temp%d_input", i + 1);
		s->temp[i] = read_sysfs_int(hwmon, a, &v) == 0 ? v / 1000.0 : TEMP_NA;
	}
	s->fault_status = read_sysfs_int(hwmon, "fault_status_raw", &v) == 0 ? (unsigned)v : 0;
	s->fault_log    = read_sysfs_int(hwmon, "fault_log_raw", &v) == 0 ? (unsigned)v : 0;
	int cap = read_sysfs_int(hwmon, "psu_cap", &v) == 0 ? (int)v : -1;
	static const int capw[] = { 600, 450, 300, 150 };
	s->psu_cap_w = (cap >= 0 && cap <= 3) ? capw[cap] : 0;
	s->fan = read_sysfs_int(hwmon, "fan1_input", &v) == 0 ? (int)v : -1;
	get_local_fw(s->fw, sizeof(s->fw));
	s->ok = 1;
	return 0;
}

/* Blocking-with-timeout HTTP GET; body (headers stripped) left in out. */
static int http_get_sensors(const char *hostport, char *out, size_t cap)
{
	char host[128];
	snprintf(host, sizeof(host), "%s", hostport);
	int port = 9876;
	char *colon = strrchr(host, ':');
	if (colon) { *colon = '\0'; port = atoi(colon + 1); }
	char portstr[8];
	snprintf(portstr, sizeof(portstr), "%d", port);

	struct addrinfo hints = {0}, *res = NULL;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, portstr, &hints, &res) != 0)
		return -1;

	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	int rc = -1;
	if (fd >= 0) {
		fcntl(fd, F_SETFL, O_NONBLOCK);
		int cr = connect(fd, res->ai_addr, res->ai_addrlen);
		if (cr < 0 && errno == EINPROGRESS) {
			struct pollfd pfd = { .fd = fd, .events = POLLOUT };
			if (poll(&pfd, 1, 1500) == 1) {
				int err = 0; socklen_t el = sizeof(err);
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
				cr = err ? -1 : 0;
			}
		}
		if (cr == 0) {
			fcntl(fd, F_SETFL, 0);
			struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			char req[256];
			int n = snprintf(req, sizeof(req),
				"GET /sensors HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
			if (write(fd, req, n) == n) {
				size_t total = 0;
				ssize_t r;
				while (total < cap - 1 && (r = read(fd, out + total, cap - 1 - total)) > 0)
					total += (size_t)r;
				out[total] = '\0';
				rc = 0;
			}
		}
		close(fd);
	}
	freeaddrinfo(res);
	if (rc < 0)
		return -1;

	char *body = strstr(out, "\r\n\r\n");
	if (body)
		memmove(out, body + 4, strlen(body + 4) + 1);
	return 0;
}

/* ---- minimal JSON readers for the flat /sensors schema ---- */
static const char *j_find(const char *j, const char *key)
{
	char k[40];
	snprintf(k, sizeof(k), "\"%s\"", key);
	const char *p = strstr(j, k);
	if (!p) return NULL;
	p += strlen(k);
	while (*p == ' ' || *p == ':') p++;
	return p;
}
static double j_num(const char *j, const char *key)
{
	const char *p = j_find(j, key);
	return p ? strtod(p, NULL) : 0;
}
static void j_str(const char *j, const char *key, char *out, size_t n)
{
	const char *p = j_find(j, key);
	out[0] = '\0';
	if (!p || *p != '"') return;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
	out[i] = '\0';
}
static void j_arr6(const char *j, const char *key, double out[6])
{
	const char *p = j_find(j, key);
	if (!p || *p != '[') return;
	p++;
	char *end;
	for (int i = 0; i < 6; i++) {
		out[i] = strtod(p, &end);
		if (end == p) break;
		p = end;
		while (*p == ',' || *p == ' ') p++;
	}
}

static int parse_remote(const char *hostport, const char *body,
			struct wv_snap *snaps, int max)
{
	const char *devs = strstr(body, "\"devices\"");
	if (!devs) return 0;
	const char *p = strchr(devs, '[');
	if (!p) return 0;
	int n = 0;
	while (n < max && (p = strchr(p, '{')) != NULL) {
		const char *end = strchr(p, '}');     /* device objects hold no nested {} */
		if (!end) break;
		size_t len = (size_t)(end - p + 1);
		char obj[2048];
		if (len >= sizeof(obj)) len = sizeof(obj) - 1;
		memcpy(obj, p, len);
		obj[len] = '\0';

		struct wv_snap *s = &snaps[n];
		memset(s, 0, sizeof(*s));
		snprintf(s->source, sizeof(s->source), "%s", hostport);
		j_str(obj, "name", s->name, sizeof(s->name));
		j_str(obj, "fwVer", s->fw, sizeof(s->fw));
		j_arr6(obj, "pinVoltage", s->pin_v);
		j_arr6(obj, "pinCurrent", s->pin_c);
		s->temp[0] = j_num(obj, "tempInC");
		s->temp[1] = j_num(obj, "tempOutC");
		/* Disconnected externals read 0.0 (daemon clamp) or a deeply negative
		 * sentinel (~-100, app publisher) — treat both as "not present". */
		double e1 = j_num(obj, "ext1C"), e2 = j_num(obj, "ext2C");
		s->temp[2] = (e1 == 0.0 || e1 <= -40.0) ? TEMP_NA : e1;
		s->temp[3] = (e2 == 0.0 || e2 <= -40.0) ? TEMP_NA : e2;
		s->psu_cap_w = (int)j_num(obj, "psuCapW");
		s->fault_status = (unsigned)j_num(obj, "faultStatus");
		s->fault_log = (unsigned)j_num(obj, "faultLog");
		s->sum_a = j_num(obj, "sumCurrentA");
		s->sum_w = j_num(obj, "sumPowerW");
		const char *fp = j_find(obj, "fan");
		s->fan = fp ? (int)strtod(fp, NULL) : -1;
		if (s->name[0] == '\0') snprintf(s->name, sizeof(s->name), "WireView");
		s->ok = 1;
		n++;
		p = end + 1;
	}
	return n;
}

static const char *bar_color(double frac)
{
	if (frac >= 0.85) return ESC "[91m";
	if (frac >= 0.60) return ESC "[93m";
	return ESC "[92m";
}
static void print_bar(double frac, int width)
{
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	int fill = (int)(frac * width + 0.5);
	fputs(bar_color(frac), stdout);
	for (int i = 0; i < fill; i++) fputs(BLK, stdout);
	fputs(ESC "[90m", stdout);
	for (int i = fill; i < width; i++) fputs(SHADE, stdout);
	fputs(ESC "[0m", stdout);
}

static void draw_panel(const struct wv_snap *s)
{
	if (!s->ok) {
		printf(ESC "[91m" TL HR " %s " HR " offline" ESC "[0m\n\n", s->source);
		return;
	}

	printf(ESC "[96m" TL HR " " ESC "[1m%s" ESC "[0;96m " HR " %s", s->source, s->name);
	if (s->fw[0]) printf(" " HR " fw%s", s->fw);
	if (s->psu_cap_w) printf(" " HR " cap %dW", s->psu_cap_w);
	printf(ESC "[0m\n");

	double cap = s->psu_cap_w > 0 ? s->psu_cap_w : 300.0;
	printf(ESC "[96m" VB ESC "[0m Power   ");
	print_bar(s->sum_w / cap, 28);
	printf("  %7.1f W\n", s->sum_w);
	printf(ESC "[96m" VB ESC "[0m Current ");
	print_bar(s->sum_a / (cap / 12.0), 28);
	printf("  %7.2f A\n", s->sum_a);

	/* per-pin breakdown, one metric per row so each is easy to scan/compare */
	printf(ESC "[96m" VB ESC "[0;90m Pin   ");
	for (int p = 0; p < 6; p++) printf("%8d", p + 1);
	printf(ESC "[0m\n");
	printf(ESC "[96m" VB ESC "[0m Volts ");
	for (int p = 0; p < 6; p++) printf("%8.2f", s->pin_v[p]);
	printf("\n");
	printf(ESC "[96m" VB ESC "[0m Amps  ");
	for (int p = 0; p < 6; p++) printf("%8.2f", s->pin_c[p]);
	printf("\n");
	printf(ESC "[96m" VB ESC "[0m Watts ");
	for (int p = 0; p < 6; p++) printf("%8.1f", s->pin_v[p] * s->pin_c[p]);
	printf("\n");

	printf(ESC "[96m" VB ESC "[0m Temp   ");
	static const char *tn[] = { "In", "Out", "E1", "E2" };
	for (int t = 0; t < 4; t++) {
		if (s->temp[t] <= TEMP_NA + 1)
			printf("%s -- ", tn[t]);
		else
			printf("%s %.1f" DEG " ", tn[t], s->temp[t]);
	}
	if (s->fan >= 0) printf("  Fan %d%%", s->fan);
	else printf("  Fan --");
	if (s->fault_status || s->fault_log)
		printf("  " ESC "[91mFaults 0x%X/0x%X" ESC "[0m", s->fault_status, s->fault_log);
	else
		printf("  " ESC "[92mFaults none" ESC "[0m");
	printf("\n" ESC "[96m" BL HR ESC "[0m\n\n");
}

static volatile sig_atomic_t g_quit = 0;
static struct termios g_orig_termios;
static int g_termios_saved = 0;

static void on_sig(int s) { (void)s; g_quit = 1; }
static void term_restore(void)
{
	if (g_termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
	printf(ESC "[?25h" ESC "[?1049l");
	fflush(stdout);
}

static int cmd_top(int argc, char **argv)
{
	char hosts[WV_MAXHOST][80];
	int nhost = 0;
	int interval_ms = 1000;

	FILE *f = fopen("/etc/wireview/hosts", "r");
	if (f) {
		char line[100];
		while (fgets(line, sizeof(line), f) && nhost < WV_MAXHOST) {
			size_t l = strlen(line);
			while (l && (line[l-1] == '\n' || line[l-1] == '\r' || line[l-1] == ' ')) line[--l] = '\0';
			char *t = line;
			while (*t == ' ' || *t == '\t') t++;
			if (*t && *t != '#') snprintf(hosts[nhost++], 80, "%s", t);
		}
		fclose(f);
	}
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
			/* one flag may carry several hosts: --host a,b c */
			char *tok = strtok(argv[++i], ", ");
			while (tok && nhost < WV_MAXHOST) {
				snprintf(hosts[nhost++], 80, "%s", tok);
				tok = strtok(NULL, ", ");
			}
		} else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc)
			interval_ms = atoi(argv[++i]);
	}
	if (interval_ms < 200) interval_ms = 200;

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	if (isatty(STDIN_FILENO)) {
		tcgetattr(STDIN_FILENO, &g_orig_termios);
		g_termios_saved = 1;
		struct termios raw = g_orig_termios;
		raw.c_lflag &= ~(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	}
	printf(ESC "[?1049h" ESC "[?25l");

	while (!g_quit) {
		struct wv_snap snaps[WV_MAXDEV];
		int n = 0;
		if (read_local(&snaps[n]) == 0) n++;
		for (int h = 0; h < nhost && n < WV_MAXDEV; h++) {
			static char buf[16384];
			if (http_get_sensors(hosts[h], buf, sizeof(buf)) == 0) {
				n += parse_remote(hosts[h], buf, snaps + n, WV_MAXDEV - n);
			} else {
				memset(&snaps[n], 0, sizeof(snaps[n]));
				snprintf(snaps[n].source, sizeof(snaps[n].source), "%s", hosts[h]);
				snaps[n].ok = 0;
				n++;
			}
		}

		printf(ESC "[H" ESC "[2J");
		time_t t = time(NULL);
		struct tm tmv;
		localtime_r(&t, &tmv);
		char ts[16];
		strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
		printf(ESC "[1;96m WireView top" ESC "[0m  %d device%s  %s  refresh %.1fs  "
		       ESC "[90mq to quit" ESC "[0m\n\n",
		       n, n == 1 ? "" : "s", ts, interval_ms / 1000.0);
		for (int i = 0; i < n; i++)
			draw_panel(&snaps[i]);
		fflush(stdout);

		struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
		if (poll(&pfd, 1, interval_ms) == 1) {
			char c;
			if (read(STDIN_FILENO, &c, 1) == 1 && (c == 'q' || c == 'Q'))
				break;
		}
	}
	term_restore();
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: wireviewctl <command> [args]\n"
		"\n"
		"Commands (require wireviewd running):\n"
		"  info              Show device firmware, UID, and build info\n"
		"  clear-faults      Clear all fault status and log\n"
		"  read-config       Read device config (hex to stdout)\n"
		"  write-config FILE Write device config (hex from file)\n"
		"  screen CMD        Change display (main|simple|current|temp|status|same|pause|resume)\n"
		"  nvm CMD           NVM operation (load|store|reset|load-cal|store-cal|load-cal-factory|store-cal-factory)\n"
		"  build             Show firmware build string\n"
		"  bootloader        Enter DFU bootloader mode\n"
		"  flash [FILE] [-y] Flash firmware (.hex or .bin) via DFU (needs dfu-util;\n"
		"                    works without the daemon if the bootloader is already up).\n"
		"                    Without FILE, flashes the bundled image at\n"
		"                    " DEFAULT_FIRMWARE_PATH "\n"
		"\n"
		"Commands (require wireview_hwmon module):\n"
		"  sensors           Show all sensor readings from hwmon sysfs\n"
		"\n"
		"Monitor:\n"
		"  top [--host H[:port][,H2...]]... [--interval MS]\n"
		"                    Live dashboard: the local device plus remote hosts.\n"
		"                    --host repeats and/or takes a comma/space list; hosts are\n"
		"                    also read from /etc/wireview/hosts. Press q to quit.\n"
	);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 1;
	}

	const char *cmd = argv[1];

	if (strcmp(cmd, "info") == 0)
		return cmd_info();
	if (strcmp(cmd, "clear-faults") == 0)
		return cmd_clear_faults();
	if (strcmp(cmd, "read-config") == 0)
		return cmd_read_config();
	if (strcmp(cmd, "write-config") == 0) {
		if (argc < 3) {
			fprintf(stderr, "wireviewctl: write-config requires a file path\n");
			return 1;
		}
		return cmd_write_config(argv[2]);
	}
	if (strcmp(cmd, "screen") == 0) {
		if (argc < 3) {
			fprintf(stderr, "wireviewctl: screen requires a command name\n"
				"Valid: main, simple, current, temp, status, same, pause, resume\n");
			return 1;
		}
		return cmd_screen(argv[2]);
	}
	if (strcmp(cmd, "nvm") == 0) {
		if (argc < 3) {
			fprintf(stderr, "wireviewctl: nvm requires a command name\n"
				"Valid: load, store, reset, load-cal, store-cal, "
				"load-cal-factory, store-cal-factory\n");
			return 1;
		}
		return cmd_nvm(argv[2]);
	}
	if (strcmp(cmd, "build") == 0)
		return cmd_build();
	if (strcmp(cmd, "flash") == 0) {
		const char *path = NULL;
		int yes = 0;
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "-y") == 0) {
				yes = 1;
			} else if (!path) {
				path = argv[i];
			} else {
				fprintf(stderr, "wireviewctl: flash takes one firmware file at most\n");
				return 1;
			}
		}
		if (!path) {
			if (access(DEFAULT_FIRMWARE_PATH, R_OK) != 0) {
				fprintf(stderr, "wireviewctl: no firmware file given and the bundled image\n"
					"is not installed at %s\n"
					"(install/upgrade the wireview-hwmon package, or pass a file path)\n",
					DEFAULT_FIRMWARE_PATH);
				return 1;
			}
			path = DEFAULT_FIRMWARE_PATH;
			printf("using bundled firmware: %s\n", path);
		}
		return cmd_flash(path, yes);
	}
	if (strcmp(cmd, "bootloader") == 0)
		return cmd_bootloader();
	if (strcmp(cmd, "sensors") == 0)
		return cmd_sensors();
	if (strcmp(cmd, "top") == 0)
		return cmd_top(argc, argv);
	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
		usage();
		return 0;
	}

	fprintf(stderr, "wireviewctl: unknown command '%s'\n", cmd);
	usage();
	return 1;
}
