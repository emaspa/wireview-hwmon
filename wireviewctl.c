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
#include <sys/socket.h>
#include <sys/un.h>
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
		"\n"
		"Commands (require wireview_hwmon module):\n"
		"  sensors           Show all sensor readings from hwmon sysfs\n"
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
	if (strcmp(cmd, "bootloader") == 0)
		return cmd_bootloader();
	if (strcmp(cmd, "sensors") == 0)
		return cmd_sensors();
	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
		usage();
		return 0;
	}

	fprintf(stderr, "wireviewctl: unknown command '%s'\n", cmd);
	usage();
	return 1;
}
