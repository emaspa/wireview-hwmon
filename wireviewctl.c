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
#include <sys/random.h>
#include <linux/limits.h>

#include "sha256.h"

#ifndef SOCK_PATH
#define SOCK_PATH "/run/wireviewd.sock"
#endif

/* Package version, injected by the Makefile (-DWIREVIEW_PKG_VERSION="x.y.z"). */
#ifndef WIREVIEW_PKG_VERSION
#define WIREVIEW_PKG_VERSION "unknown"
#endif

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
#define RESP_DENIED        3	/* privileged command, peer not root or in "wireview" */

/* sock_command() return values other than 0 */
#define SOCK_FAIL   (-1)
#define SOCK_DENIED (-2)

/* Global options. With --host, commands go to that host's wireviewd over
 * HTTP (writes signed with the shared secret) instead of the local socket. */
static const char *g_host;		/* "host[:port]", NULL = local */
static const char *g_secret_file;	/* --secret-file, else $WIREVIEW_SECRET */

/* Remote (--host) implementations, defined with the HTTP client below. */
static int http_command(const char *json);
static int remote_info(int build_only);
static int remote_read_config(void);
static void b64_encode(const uint8_t *in, size_t len, char *out);

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

/* Send request and receive response. Returns 0 on success, SOCK_DENIED if
 * the daemon refused a privileged command, SOCK_FAIL on any other failure
 * (both negative, so callers can test "< 0").
 * On success, *resp_buf is malloc'd and must be freed, *resp_len is set.
 * On failure, *resp_buf is NULL. */
static int sock_command(uint8_t cmd, const void *payload, uint16_t payload_len,
			uint8_t **resp_buf, uint16_t *resp_len)
{
	int fd = sock_connect();
	if (fd < 0)
		return SOCK_FAIL;

	/* Send: [cmd:1][len:2 LE][payload] */
	uint8_t hdr[3] = { cmd, payload_len & 0xFF, payload_len >> 8 };
	if (write(fd, hdr, 3) != 3 ||
	    (payload_len > 0 && write(fd, payload, payload_len) != payload_len)) {
		perror("write");
		close(fd);
		return SOCK_FAIL;
	}

	/* Receive: [status:1][len:2 LE][data] */
	uint8_t rhdr[3];
	ssize_t n = 0, off = 0;
	while (off < 3) {
		n = read(fd, rhdr + off, 3 - off);
		if (n <= 0) {
			perror("read header");
			close(fd);
			return SOCK_FAIL;
		}
		off += n;
	}

	uint8_t status = rhdr[0];
	uint16_t rlen = rhdr[1] | (rhdr[2] << 8);

	uint8_t *data = NULL;
	if (rlen > 0) {
		data = calloc(1, rlen);
		if (!data) {
			close(fd);
			return SOCK_FAIL;
		}
		off = 0;
		while (off < rlen) {
			n = read(fd, data + off, rlen - off);
			if (n <= 0) {
				perror("read data");
				free(data);
				close(fd);
				return SOCK_FAIL;
			}
			off += n;
		}
	}

	close(fd);

	if (status == RESP_NOT_CONNECTED) {
		fprintf(stderr, "wireviewctl: device not connected\n");
		free(data);
		return SOCK_FAIL;
	}
	if (status == RESP_ERROR) {
		fprintf(stderr, "wireviewctl: command failed\n");
		free(data);
		return SOCK_FAIL;
	}
	if (status == RESP_DENIED) {
		fprintf(stderr, "wireviewctl: permission denied: this command needs root "
				"or membership of the 'wireview' group\n"
				"(sudo usermod -aG wireview $USER, no re-login needed, membership is checked per connection)\n");
		free(data);
		return SOCK_DENIED;
	}
	if (status != RESP_OK) {
		fprintf(stderr, "wireviewctl: unexpected status %u from wireviewd\n", status);
		free(data);
		return SOCK_FAIL;
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

	if (g_host)
		return remote_info(0);
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

static int parse_fault_mask(const char *s, uint16_t *out)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 16);
	if (errno || end == s || *end != '\0' || v > 0xFFFF) {
		fprintf(stderr, "wireviewctl: invalid fault mask '%s' (hex, 0-FFFF)\n", s);
		return -1;
	}
	*out = (uint16_t)v;
	return 0;
}

/*
 * The arguments name the bits to CLEAR (default FFFF = everything). On the
 * wire the firmware takes keep-masks: fault &= mask, so a set bit keeps a
 * fault and 0 clears all (the GUI sends ~(1 << fault) to clear one fault).
 * Payload is status_keep (u16 LE) then log_keep (u16 LE).
 */
static int cmd_clear_faults(const char *status_arg, const char *log_arg)
{
	uint16_t status_mask = 0xFFFF, log_mask = 0xFFFF;
	uint8_t *data = NULL;
	uint16_t len = 0;

	if (status_arg && parse_fault_mask(status_arg, &status_mask) < 0)
		return 1;
	if (log_arg && parse_fault_mask(log_arg, &log_mask) < 0)
		return 1;

	uint16_t status_keep = (uint16_t)~status_mask;
	uint16_t log_keep = (uint16_t)~log_mask;

	if (g_host) {
		/* The HTTP clearFaults op takes the same keep-masks. */
		char json[96];
		snprintf(json, sizeof(json),
			 "{\"op\":\"clearFaults\",\"statusMask\":%u,\"logMask\":%u}",
			 status_keep, log_keep);
		if (http_command(json))
			return 1;
	} else {
		uint8_t payload[4] = {
			status_keep & 0xFF, status_keep >> 8,
			log_keep & 0xFF, log_keep >> 8,
		};
		if (sock_command(WCMD_CLEAR_FAULTS, payload, 4, &data, &len) < 0)
			return 1;
	}

	printf("faults cleared (status bits 0x%04X, log bits 0x%04X)\n",
	       status_mask, log_mask);
	free(data);
	return 0;
}

static int cmd_read_config(void)
{
	uint8_t *data = NULL;
	uint16_t len = 0;

	if (g_host)
		return remote_read_config();
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

	if (g_host) {
		/* writeConfig takes the raw config bytes; the daemon knows the
		 * device's config version. */
		char json[64 + 4 * (96 / 3 + 1)];
		char b64[4 * (96 / 3 + 1) + 1];
		b64_encode(payload + 1, nbytes, b64);
		free(payload);
		snprintf(json, sizeof(json), "{\"op\":\"writeConfig\",\"data\":\"%s\"}", b64);
		if (http_command(json))
			return 1;
		printf("config written (%zu bytes, version %u)\n", nbytes, cfg_ver);
		return 0;
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

/* screen and nvm: one command byte, as WCMD_* over the socket or as
 * {"op":OP,"cmd":N} over HTTP. Returns 0 on success, 1 on failure. */
static int send_byte_command(uint8_t wcmd, const char *op, uint8_t val)
{
	if (g_host) {
		char json[64];
		snprintf(json, sizeof(json), "{\"op\":\"%s\",\"cmd\":%u}", op, val);
		return http_command(json);
	}

	uint8_t *resp = NULL;
	uint16_t rlen = 0;
	if (sock_command(wcmd, &val, 1, &resp, &rlen) < 0)
		return 1;
	free(resp);
	return 0;
}

static int cmd_screen(const char *name)
{
	for (const struct name_val *s = screen_cmds; s->name; s++) {
		if (strcmp(name, s->name) == 0) {
			if (send_byte_command(WCMD_SCREEN_CMD, "screen", s->val))
				return 1;
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
			if (send_byte_command(WCMD_NVM_CMD, "nvm", s->val))
				return 1;
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

	if (g_host)
		return remote_info(1);
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

/* Where an image may land: the STM32's main flash, and no more than the
 * 4 MiB a .bin may hold either. */
#define FW_FLASH_BASE 0x08000000u
#define FW_FLASH_SIZE (4u * 1024 * 1024)

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/*
 * Decode one Intel HEX record, t pointing at its ':' with trailing blanks
 * already stripped, into rec[]: count, address (2 bytes), type, data,
 * checksum. Returns the data byte count, or -1 with *why set when the
 * record has a non-hex character, an odd number of digits, fewer or more
 * bytes than its count says, or a checksum that does not bring the sum of
 * all its bytes to 0 mod 256.
 */
static int ihex_record(const char *t, uint8_t rec[260], const char **why)
{
	if (*t++ != ':') {
		*why = "not an Intel HEX record (no leading ':')";
		return -1;
	}
	size_t n = strlen(t);
	for (size_t i = 0; i < n; i++) {
		if (hex_nibble(t[i]) < 0) {
			*why = "invalid hex digit";
			return -1;
		}
	}
	if (n % 2) {
		*why = "odd number of hex digits";
		return -1;
	}
	if (n < 10) {
		*why = "record too short";
		return -1;
	}
	n /= 2;
	if (n > 260) {
		*why = "record longer than its byte count";
		return -1;
	}
	uint8_t sum = 0;
	for (size_t i = 0; i < n; i++) {
		rec[i] = (uint8_t)(hex_nibble(t[2 * i]) << 4 | hex_nibble(t[2 * i + 1]));
		sum += rec[i];
	}
	if (n < (size_t)rec[0] + 5) {
		*why = "record shorter than its byte count (truncated?)";
		return -1;
	}
	if (n > (size_t)rec[0] + 5) {
		*why = "record longer than its byte count";
		return -1;
	}
	if (sum != 0) {
		*why = "bad checksum";
		return -1;
	}
	return rec[0];
}

/*
 * Intel HEX into a flat image padded with 0xFF, in two passes: the first
 * checks every record up to the EOF record (which must be there) and finds
 * the address range, the second fills the image. Data must lie within the
 * flash window and no byte may be given twice; a data record may not run
 * past its 64 KiB segment. Start-address records (03, 05) are checked and
 * ignored: DFU does not use them. Errors name the file and line.
 */
static long load_ihex(FILE *f, const char *path, uint8_t **img_out, uint32_t *base_out)
{
	char line[600];		/* the longest record is 521 characters */
	uint8_t rec[260];
	uint32_t minaddr = 0xFFFFFFFFu, maxaddr = 0;
	uint8_t *img = NULL, *seen = NULL;
	size_t len = 0;
	int lineno = 0;
	const char *why = NULL;

	for (int pass = 0; pass < 2; pass++) {
		uint32_t upper = 0;
		int eof = 0;

		rewind(f);
		lineno = 0;
		while (!eof && fgets(line, sizeof(line), f)) {
			lineno++;
			size_t l = strlen(line);
			if (l == sizeof(line) - 1 && line[l - 1] != '\n' && !feof(f)) {
				why = "line too long";
				goto bad;
			}
			while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' ||
				     line[l - 1] == ' ' || line[l - 1] == '\t'))
				line[--l] = '\0';
			const char *t = line;
			while (*t == ' ' || *t == '\t') t++;
			if (!*t)
				continue;

			int cnt = ihex_record(t, rec, &why);
			if (cnt < 0)
				goto bad;
			uint32_t addr = (uint32_t)rec[1] << 8 | rec[2];
			switch (rec[3]) {
			case 0: {
				if (cnt == 0)
					break;
				if (addr + (uint32_t)cnt > 0x10000) {
					why = "data record runs past its 64 KiB segment";
					goto bad;
				}
				uint32_t a = upper + addr;
				if (a < FW_FLASH_BASE || a - FW_FLASH_BASE > FW_FLASH_SIZE - (uint32_t)cnt) {
					why = "data outside the flash window 0x08000000-0x083FFFFF";
					goto bad;
				}
				if (pass == 0) {
					if (a < minaddr) minaddr = a;
					if (a + (uint32_t)cnt - 1 > maxaddr) maxaddr = a + (uint32_t)cnt - 1;
					break;
				}
				size_t off = a - minaddr;
				for (int i = 0; i < cnt; i++, off++) {
					uint8_t bit = (uint8_t)(1u << (off & 7));
					if (seen[off >> 3] & bit) {
						why = "data overlaps an earlier record";
						goto bad;
					}
					seen[off >> 3] |= bit;
					img[off] = rec[4 + i];
				}
				break;
			}
			case 1:
				if (cnt != 0) {
					why = "end-of-file record carries data";
					goto bad;
				}
				eof = 1;	/* anything after it is ignored */
				break;
			case 2:
			case 4:
				if (cnt != 2) {
					why = "address record must carry 2 bytes";
					goto bad;
				}
				upper = (uint32_t)rec[4] << 8 | rec[5];
				upper <<= rec[3] == 4 ? 16 : 4;
				break;
			case 3:
			case 5:
				if (cnt != 4) {
					why = "start-address record must carry 4 bytes";
					goto bad;
				}
				break;
			default:
				why = "unknown record type";
				goto bad;
			}
		}
		if (ferror(f)) {
			fprintf(stderr, "wireviewctl: %s: read error\n", path);
			goto fail;
		}
		if (!eof) {
			fprintf(stderr, "wireviewctl: %s: no end-of-file record after line %d "
				"(truncated file?)\n", path, lineno);
			goto fail;
		}
		if (pass == 0) {
			if (minaddr > maxaddr) {
				fprintf(stderr, "wireviewctl: %s: no data records\n", path);
				goto fail;
			}
			len = (size_t)(maxaddr - minaddr) + 1;
			img = malloc(len);
			seen = calloc(len / 8 + 1, 1);
			if (!img || !seen) {
				fprintf(stderr, "wireviewctl: out of memory\n");
				goto fail;
			}
			memset(img, 0xFF, len);
		}
	}
	free(seen);
	*img_out = img;
	*base_out = minaddr;
	return (long)len;

bad:
	fprintf(stderr, "wireviewctl: %s: line %d: %s\n", path, lineno, why);
fail:
	free(img);
	free(seen);
	return -1;
}

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
	uint32_t base = FW_FLASH_BASE;

	if (c == ':') {
		len = load_ihex(f, path, &img, &base);
		if (len < 0) {
			fclose(f);
			return -1;
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
	/* Validate the image first: a corrupted file fails here, before
	 * anything runs or reaches the device. */
	uint8_t *img = NULL;
	uint32_t base = 0;
	int version = -1;
	char build[40];
	long len = load_firmware(path, &img, &base, &version, build, sizeof(build));
	if (len < 0)
		return 1;

	if (system("dfu-util --version >/dev/null 2>&1") != 0) {
		fprintf(stderr, "wireviewctl: dfu-util not found; install the dfu-util package\n");
		free(img);
		return 1;
	}

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
		int rc = sock_command(WCMD_ENTER_BOOTLOADER, NULL, 0, &resp, &rlen);
		free(resp);
		if (rc == SOCK_DENIED) {
			free(img);
			return 1;
		}
		if (rc < 0)
			fprintf(stderr, "warning: could not reach wireviewd; waiting for a "
					"manually started DFU bootloader\n");

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

/* $WIREVIEW_HWMON_PATH names a hwmon directory to use instead of scanning
 * /sys/class/hwmon, so the sysfs readers can be tested against a fake tree. */
static int find_hwmon_path(char *buf, size_t bufsize)
{
	const char *env = getenv("WIREVIEW_HWMON_PATH");
	if (env && env[0]) {
		char namepath[PATH_MAX];
		snprintf(namepath, sizeof(namepath), "%s/name", env);
		if (access(namepath, R_OK) != 0)
			return -1;
		snprintf(buf, bufsize, "%s", env);
		return 0;
	}

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

/* long long: energy1_input in microjoules overflows a 32-bit long in minutes. */
static int read_sysfs_int(const char *hwmon, const char *attr, long long *val)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", hwmon, attr);

	FILE *f = fopen(path, "r");
	if (!f)
		return -1;

	int rc = (fscanf(f, "%lld", val) == 1) ? 0 : -1;
	fclose(f);
	return rc;
}

#define WV_MAXDEV   32
#define WV_MAXHOST  32
#define WV_NALARM   12

/*
 * One device's readings, in hwmon sysfs units (mV, mA, uW, m°C, uJ). Filled
 * from local sysfs by read_local() or from a remote GET /sensors by
 * parse_remote(); "sensors", "sensors --json" and "top" all print from it.
 * Bit i of a have_* mask marks index i as present.
 */
struct wv_snap {
	char      source[72];   /* "local" or "host[:port]" */
	char      name[40];
	char      fw[12];       /* firmware version, "" if unknown */
	char      uid[28];      /* device UID, uppercase hex, "" if unknown */
	char      build[72];    /* firmware build string, "" if unknown */
	int       ok;           /* readings present (local: sysfs data is fresh) */
	long long in_mv[8];     unsigned have_in;     /* pins 1-6, average, vdd */
	long long curr_ma[7];   unsigned have_curr;   /* pins 1-6, total */
	long long power_uw[7];  unsigned have_power;  /* total, pins 1-6 */
	long long temp_mc[4];   unsigned have_temp;   /* in, out, ext 1, ext 2 */
	long long energy_uj;    int have_energy;
	int       fan;          /* duty %, -1 if unavailable */
	int       psu_cap_w;    /* PSU cap in W, 0 if unknown, -1 if unavailable */
	unsigned  fault_status, fault_log;
	int       have_fault_status, have_fault_log;
	signed char alarm[WV_NALARM];  /* 0/1, -1 if the attribute is absent */
};

static const char *const in_labels[8] = {
	"pin1_voltage_mv", "pin2_voltage_mv", "pin3_voltage_mv",
	"pin4_voltage_mv", "pin5_voltage_mv", "pin6_voltage_mv",
	"avg_voltage_mv", "vdd_mv"
};
static const char *const curr_labels[7] = {
	"pin1_current_ma", "pin2_current_ma", "pin3_current_ma",
	"pin4_current_ma", "pin5_current_ma", "pin6_current_ma",
	"total_current_ma"
};
static const char *const power_labels[7] = {
	"total_power_uw",
	"pin1_power_uw", "pin2_power_uw", "pin3_power_uw",
	"pin4_power_uw", "pin5_power_uw", "pin6_power_uw"
};
static const char *const temp_labels[4] = {
	"temp_onboard_in_mc", "temp_onboard_out_mc",
	"temp_external1_mc", "temp_external2_mc"
};
/* hwmon alarm attributes and the alarm_<name> lines they print as. */
static const struct { const char *attr, *name; } alarm_attrs[WV_NALARM] = {
	{ "temp1_alarm", "temp_onboard_in" },   { "temp2_alarm", "temp_onboard_out" },
	{ "temp3_alarm", "temp_external1" },    { "temp4_alarm", "temp_external2" },
	{ "curr1_alarm", "pin1_current" },      { "curr2_alarm", "pin2_current" },
	{ "curr3_alarm", "pin3_current" },      { "curr4_alarm", "pin4_current" },
	{ "curr5_alarm", "pin5_current" },      { "curr6_alarm", "pin6_current" },
	{ "curr7_alarm", "total_current" },     { "power1_alarm", "total_power" },
};

static int read_full(int fd, void *buf, size_t n)
{
	size_t off = 0;
	while (off < n) {
		ssize_t r = read(fd, (uint8_t *)buf + off, n - off);
		if (r <= 0)
			return -1;
		off += (size_t)r;
	}
	return 0;
}

/* Best-effort: ask the daemon (if running) for the local device's firmware
 * version, UID and build string; sysfs has none of these. Silent on any
 * failure so it never disturbs the TUI or the JSON output. */
static void get_local_info(struct wv_snap *s)
{
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
		/* [fw][cfg_ver][uid:12][build string, NUL-terminated] */
		uint8_t d[2 + 12 + 64 + 1];
		if (write(fd, hdr, 3) == 3 && read_full(fd, rh, 3) == 0 &&
		    rh[0] == RESP_OK) {
			size_t len = (size_t)(rh[1] | (rh[2] << 8));
			if (len >= 14 && len < sizeof(d) && read_full(fd, d, len) == 0) {
				d[len] = '\0';
				snprintf(s->fw, sizeof(s->fw), "%u", d[0]);
				for (int i = 0; i < 12; i++)
					snprintf(s->uid + i * 2, 3, "%02X", d[2 + i]);
				snprintf(s->build, sizeof(s->build), "%s", (const char *)d + 14);
			}
		}
	}
	close(fd);
}

/* Read every attribute the module exposes. Newer modules publish pwm1,
 * power1_cap, energy1_input and *_alarm; older ones only fan1_input and
 * psu_cap, so each new attribute falls back to its old counterpart. */
static int read_local(struct wv_snap *s)
{
	char hwmon[PATH_MAX];
	if (find_hwmon_path(hwmon, sizeof(hwmon)) < 0)
		return -1;

	memset(s, 0, sizeof(*s));
	snprintf(s->source, sizeof(s->source), "local");
	snprintf(s->name, sizeof(s->name), "WireView Pro II");

	long long v;
	char a[24];
	for (int i = 0; i < 8; i++) {
		snprintf(a, sizeof(a), "in%d_input", i);
		if (read_sysfs_int(hwmon, a, &v) == 0) {
			s->in_mv[i] = v;
			s->have_in |= 1u << i;
		}
	}
	for (int i = 0; i < 7; i++) {
		snprintf(a, sizeof(a), "curr%d_input", i + 1);
		if (read_sysfs_int(hwmon, a, &v) == 0) {
			s->curr_ma[i] = v;
			s->have_curr |= 1u << i;
		}
		snprintf(a, sizeof(a), "power%d_input", i + 1);
		if (read_sysfs_int(hwmon, a, &v) == 0) {
			s->power_uw[i] = v;
			s->have_power |= 1u << i;
		}
	}
	for (int i = 0; i < 4; i++) {
		snprintf(a, sizeof(a), "temp%d_input", i + 1);
		if (read_sysfs_int(hwmon, a, &v) == 0) {
			s->temp_mc[i] = v;
			s->have_temp |= 1u << i;
		}
	}

	/* Fan duty: pwm1 is 0-255; fan1_input (deprecated) is the duty in % */
	s->fan = -1;
	if (read_sysfs_int(hwmon, "pwm1", &v) == 0)
		s->fan = (int)((v * 100 + 127) / 255);
	else if (read_sysfs_int(hwmon, "fan1_input", &v) == 0)
		s->fan = (int)v;

	/* PSU cap: power1_cap in uW; psu_cap (deprecated) is the firmware enum */
	s->psu_cap_w = -1;
	if (read_sysfs_int(hwmon, "power1_cap", &v) == 0) {
		s->psu_cap_w = (int)(v / 1000000);
	} else if (read_sysfs_int(hwmon, "psu_cap", &v) == 0) {
		static const int capw[] = { 600, 450, 300, 150 };
		s->psu_cap_w = (v >= 0 && v <= 3) ? capw[v] : 0;
	}

	if (read_sysfs_int(hwmon, "energy1_input", &v) == 0) {
		s->energy_uj = v;
		s->have_energy = 1;
	}

	/* Raw fault bitmasks; the intrusion alarms only say "some bit is set" */
	if (read_sysfs_int(hwmon, "fault_status_raw", &v) == 0 ||
	    read_sysfs_int(hwmon, "intrusion0_alarm", &v) == 0) {
		s->fault_status = (unsigned)v;
		s->have_fault_status = 1;
	}
	if (read_sysfs_int(hwmon, "fault_log_raw", &v) == 0 ||
	    read_sysfs_int(hwmon, "intrusion1_alarm", &v) == 0) {
		s->fault_log = (unsigned)v;
		s->have_fault_log = 1;
	}

	for (int i = 0; i < WV_NALARM; i++)
		s->alarm[i] = read_sysfs_int(hwmon, alarm_attrs[i].attr, &v) == 0 ?
			      (v != 0) : -1;

	/* The module answers ENODATA once the daemon's data is stale. */
	s->ok = s->have_in || s->have_curr || s->have_power;
	get_local_info(s);
	return 0;
}

/* ---------- top: live monitor (local sysfs + remote /sensors) ---------- */

/* UTF-8 glyphs + ANSI (explicit bytes so any compiler is happy) */
#define ESC   "\033"
#define BLK   "\xe2\x96\x88"   /* full block  */
#define SHADE "\xe2\x96\x91"   /* light shade */
#define TL    "\xe2\x95\xad"   /* round corner top-left */
#define BL    "\xe2\x95\xb0"   /* round corner bottom-left */
#define HR    "\xe2\x94\x80"   /* horizontal */
#define VB    "\xe2\x94\x82"   /* vertical */
#define DEG   "\xc2\xb0"       /* degree */

/* ---------- HTTP client (remote daemons: top, --host) ---------- */

#define HTTP_PORT_DEFAULT 9876

/* Split "host", "host:port", "[v6addr]:port" or a bare IPv6 address. */
static int split_hostport(const char *hostport, char *host, size_t hcap,
			  char *port, size_t pcap)
{
	const char *p = hostport, *colon;
	size_t hl;

	snprintf(port, pcap, "%d", HTTP_PORT_DEFAULT);
	if (*p == '[') {
		const char *rb = strchr(p, ']');
		if (!rb)
			return -1;
		hl = (size_t)(rb - p - 1);
		if (rb[1] == ':' && rb[2])
			snprintf(port, pcap, "%s", rb + 2);
		p++;
	} else if ((colon = strchr(p, ':')) != NULL && !strchr(colon + 1, ':')) {
		hl = (size_t)(colon - p);
		if (colon[1])
			snprintf(port, pcap, "%s", colon + 1);
	} else {
		hl = strlen(p);	/* no port, or a bare IPv6 address */
	}
	if (hl == 0 || hl >= hcap)
		return -1;
	memcpy(host, p, hl);
	host[hl] = '\0';
	return 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t w = write(fd, buf, len);
		if (w <= 0)
			return -1;
		buf += w;
		len -= (size_t)w;
	}
	return 0;
}

/* Connect with a 1.5 s limit, trying every address the name resolves to. */
static int http_connect(const char *host, const char *port)
{
	struct addrinfo hints = {0}, *res = NULL;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res) != 0)
		return -1;

	int fd = -1;
	for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0)
			continue;
		fcntl(fd, F_SETFL, O_NONBLOCK);
		int cr = connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (cr < 0 && errno == EINPROGRESS) {
			struct pollfd pfd = { .fd = fd, .events = POLLOUT };
			if (poll(&pfd, 1, 1500) == 1) {
				int err = 0; socklen_t el = sizeof(err);
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
				cr = err ? -1 : 0;
			}
		}
		if (cr != 0) {
			close(fd);
			fd = -1;
			continue;
		}
		fcntl(fd, F_SETFL, 0);
	}
	freeaddrinfo(res);
	return fd;
}

/*
 * One HTTP/1.0 request to a wireviewd listener. body may be NULL (no
 * Content-* headers are sent then); extra_headers is NULL or a block of
 * complete "Name: value\r\n" lines. The response body, headers stripped,
 * lands in out (NUL-terminated, truncated to cap - 1) and the status code in
 * *status. Returns 0 when a response arrived, -1 if the host could not be
 * reached or the reply was not HTTP.
 */
static int http_request(const char *hostport, const char *method, const char *path,
			const char *body, const char *extra_headers, int timeout_ms,
			char *out, size_t cap, int *status)
{
	char host[128], port[16];
	if (cap < 2 || split_hostport(hostport, host, sizeof(host), port, sizeof(port)) < 0)
		return -1;

	char req[4096];
	int n;
	if (body)
		n = snprintf(req, sizeof(req),
			"%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n%s"
			"Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
			method, path, host, extra_headers ? extra_headers : "",
			strlen(body), body);
	else
		n = snprintf(req, sizeof(req),
			"%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n%s\r\n",
			method, path, host, extra_headers ? extra_headers : "");
	if (n < 0 || (size_t)n >= sizeof(req))
		return -1;

	int fd = http_connect(host, port);
	if (fd < 0)
		return -1;

	struct timeval tv = { .tv_sec = timeout_ms / 1000,
			      .tv_usec = (timeout_ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	int rc = -1;
	if (write_all(fd, req, (size_t)n) == 0) {
		size_t total = 0;
		ssize_t r;
		while (total < cap - 1 && (r = read(fd, out + total, cap - 1 - total)) > 0)
			total += (size_t)r;
		out[total] = '\0';
		if (sscanf(out, "HTTP/%*s %d", status) == 1)
			rc = 0;
	}
	close(fd);
	if (rc < 0)
		return -1;

	char *b = strstr(out, "\r\n\r\n");
	if (b)
		memmove(out, b + 4, strlen(b + 4) + 1);
	else
		out[0] = '\0';
	return 0;
}

/* GET /sensors for top; any non-200 reply counts as unreachable. */
static int http_get_sensors(const char *hostport, char *out, size_t cap)
{
	int status = 0;
	if (http_request(hostport, "GET", "/sensors", NULL, NULL, 2000,
			 out, cap, &status) < 0 || status != 200)
		return -1;
	return 0;
}

/*
 * ---- a small JSON reader for GET /sensors and GET /config ----
 *
 * Not a validator: it walks the structure far enough to find an object's
 * own members, skipping strings (with their escapes) and nested values, so
 * text inside a string or a key of a nested object never matches. Every
 * step stops at the terminating NUL, so a truncated body reads as absent
 * members, never past the buffer.
 */
static const char *js_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

/* p is on a '"'; returns the byte after the closing quote, NULL if the
 * string is unterminated. */
static const char *js_skip_string(const char *p)
{
	for (p++; *p; p++) {
		if (*p == '\\') {
			if (!p[1])
				return NULL;
			p++;
		} else if (*p == '"') {
			return p + 1;
		}
	}
	return NULL;
}

/* Skip one value (string, object, array or scalar) at p; returns the byte
 * after it, NULL if it is missing or truncated. */
static const char *js_skip_value(const char *p)
{
	p = js_ws(p);
	if (*p == '"')
		return js_skip_string(p);
	if (*p == '{' || *p == '[') {
		size_t depth = 0;
		while (*p) {
			if (*p == '"') {
				p = js_skip_string(p);
				if (!p)
					return NULL;
				continue;
			}
			if (*p == '{' || *p == '[') {
				depth++;
			} else if (*p == '}' || *p == ']') {
				if (--depth == 0)
					return p + 1;
			}
			p++;
		}
		return NULL;
	}
	const char *s = p;	/* number, true, false, null */
	while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
	       *p != '\t' && *p != '\n' && *p != '\r' && *p != '"' &&
	       *p != '{' && *p != '[' && *p != ':')
		p++;
	return p == s ? NULL : p;
}

/* The value of member key of the object at obj (only its own members, the
 * first one wins), whitespace skipped; NULL if obj is not an object or has
 * no such member before it ends or breaks off. */
static const char *js_member(const char *obj, const char *key)
{
	size_t klen = strlen(key);
	const char *p = js_ws(obj);

	if (*p != '{')
		return NULL;
	p = js_ws(p + 1);
	while (*p == '"') {
		const char *kend = js_skip_string(p);
		if (!kend)
			return NULL;
		const char *v = js_ws(kend);
		if (*v != ':')
			return NULL;
		v = js_ws(v + 1);
		if ((size_t)(kend - p - 2) == klen && memcmp(p + 1, key, klen) == 0)
			return v;
		p = js_skip_value(v);
		if (!p)
			return NULL;
		p = js_ws(p);
		if (*p != ',')
			return NULL;
		p = js_ws(p + 1);
	}
	return NULL;
}

/* A number at v (NULL-safe); 1 and *out set if there is one. */
static int js_number_at(const char *v, double *out)
{
	char *end;

	if (!v || !(*v == '-' || (*v >= '0' && *v <= '9')))
		return 0;
	*out = strtod(v, &end);
	return end != v;
}

/* Member key of obj as a number; 1 and *out set if present and numeric. */
static int js_num(const char *obj, const char *key, double *out)
{
	return js_number_at(js_member(obj, key), out);
}

/* Member key of obj as a number, 0 when absent or not a number. */
static double js_num0(const char *obj, const char *key)
{
	double d = 0;
	return js_num(obj, key, &d) ? d : 0;
}

/* Member key of obj as a string, unescaped into out (always terminated,
 * truncated to n - 1). \uXXXX other than ASCII becomes '?', as wireviewd
 * writes non-ASCII bytes. Returns 1 if the member is a string. */
static int js_str(const char *obj, const char *key, char *out, size_t n)
{
	const char *p = js_member(obj, key);
	size_t i = 0;

	out[0] = '\0';
	if (!p || *p != '"')
		return 0;
	for (p++; *p && *p != '"'; p++) {
		char c = *p;
		if (c == '\\') {
			p++;
			switch (*p) {
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'u': {
				unsigned u = 0;
				int k;
				for (k = 1; k <= 4 && hex_nibble(p[k]) >= 0; k++)
					u = u << 4 | (unsigned)hex_nibble(p[k]);
				if (k <= 4)
					return 1;	/* broken escape: keep what we have */
				p += 4;
				c = u >= 0x01 && u < 0x80 ? (char)u : '?';
				break;
			}
			case '\0':
				return 1;
			default:	/* \" \\ \/ */
				c = *p;
			}
		}
		if (i + 1 < n) {
			out[i++] = c;
			out[i] = '\0';
		}
	}
	return 1;
}

/* Member key of obj as an array of up to 6 numbers into out; returns the
 * count read (it stops at the first element that is not a number). */
static int js_arr6(const char *obj, const char *key, double out[6])
{
	const char *p = js_member(obj, key);
	int i = 0;

	if (!p || *p != '[')
		return 0;
	p = js_ws(p + 1);
	while (i < 6 && js_number_at(p, &out[i])) {
		i++;
		p = js_skip_value(p);
		if (!p)
			break;
		p = js_ws(p);
		if (*p != ',')
			break;
		p = js_ws(p + 1);
	}
	return i;
}

/* Round a reading to the integer sysfs unit the snapshot stores. */
static long long to_ll(double x)
{
	return (long long)(x >= 0 ? x + 0.5 : x - 0.5);
}

/* One device object of GET /sensors into a snapshot. */
static void parse_device(const char *hostport, const char *obj, struct wv_snap *s)
{
	memset(s, 0, sizeof(*s));
	snprintf(s->source, sizeof(s->source), "%s", hostport);
	js_str(obj, "name", s->name, sizeof(s->name));
	js_str(obj, "fwVer", s->fw, sizeof(s->fw));
	js_str(obj, "id", s->uid, sizeof(s->uid));
	js_str(obj, "buildString", s->build, sizeof(s->build));

	double pv[6] = {0}, pc[6] = {0};
	js_arr6(obj, "pinVoltage", pv);
	js_arr6(obj, "pinCurrent", pc);
	for (int i = 0; i < 6; i++) {
		s->in_mv[i] = to_ll(pv[i] * 1000.0);
		s->curr_ma[i] = to_ll(pc[i] * 1000.0);
		s->power_uw[i + 1] = to_ll(pv[i] * pc[i] * 1e6);
	}
	s->curr_ma[6] = to_ll(js_num0(obj, "sumCurrentA") * 1000.0);
	s->power_uw[0] = to_ll(js_num0(obj, "sumPowerW") * 1e6);
	s->have_in = 0x3F;	/* no average / vdd over the network */
	s->have_curr = 0x7F;
	s->have_power = 0x7F;

	s->temp_mc[0] = to_ll(js_num0(obj, "tempInC") * 1000.0);
	s->temp_mc[1] = to_ll(js_num0(obj, "tempOutC") * 1000.0);
	s->have_temp = 0x3;
	/* Disconnected externals read 0.0 (daemon clamp) or a deeply negative
	 * sentinel (~-100, app publisher) — treat both as "not present".
	 * -40.0 is the lowest reading the daemon publishes, so keep it. */
	double e[2] = { js_num0(obj, "ext1C"), js_num0(obj, "ext2C") };
	for (int i = 0; i < 2; i++) {
		if (e[i] != 0.0 && e[i] >= -40.0) {
			s->temp_mc[2 + i] = to_ll(e[i] * 1000.0);
			s->have_temp |= 1u << (2 + i);
		}
	}

	s->psu_cap_w = (int)js_num0(obj, "psuCapW");
	s->fault_status = (unsigned)js_num0(obj, "faultStatus");
	s->fault_log = (unsigned)js_num0(obj, "faultLog");
	s->have_fault_status = s->have_fault_log = 1;
	double d;
	s->fan = js_num(obj, "fan", &d) ? (int)d : -1;
	if (js_num(obj, "energyJ", &d)) {
		s->energy_uj = to_ll(d * 1e6);
		s->have_energy = 1;
	}
	for (int i = 0; i < WV_NALARM; i++)
		s->alarm[i] = -1;	/* /sensors carries no per-channel alarms */
	if (s->name[0] == '\0') snprintf(s->name, sizeof(s->name), "WireView");
	const char *cp = js_member(obj, "connected");
	s->ok = !(cp && strncmp(cp, "false", 5) == 0);
}

/* The complete device objects in the root's "devices" array, up to max; a
 * truncated body stops at the last complete one, other elements are
 * skipped. Returns the number of snapshots written. */
static int parse_remote(const char *hostport, const char *body,
			struct wv_snap *snaps, int max)
{
	const char *p = js_member(body, "devices");
	int n = 0;

	if (!p || *p != '[')
		return 0;
	p = js_ws(p + 1);
	while (n < max && *p && *p != ']') {
		const char *end = js_skip_value(p);
		if (!end)
			break;
		if (*p == '{')
			parse_device(hostport, p, &snaps[n++]);
		p = js_ws(end);
		if (*p != ',')
			break;
		p = js_ws(p + 1);
	}
	return n;
}

/* ---------- remote commands (--host) ---------- */

static void b64_encode(const uint8_t *in, size_t len, char *out)
{
	static const char T[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, o = 0;
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

static int b64val(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

/* Returns the decoded length, or -1 on a bad character or overflow. */
static int b64_decode(const char *in, uint8_t *out, size_t outcap)
{
	size_t outlen = 0;
	uint32_t acc = 0;
	int bits = 0;
	for (const char *p = in; *p; p++) {
		if (*p == '=')
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

static int random_bytes(uint8_t *buf, size_t n)
{
	if (getrandom(buf, n, 0) == (ssize_t)n)
		return 0;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int rc = read_full(fd, buf, n);
	close(fd);
	return rc;
}

/*
 * The shared secret for signed writes: --secret-file FILE, else
 * $WIREVIEW_SECRET (never a command-line value, which ps would show). The
 * file is read like the daemon reads /etc/wireview/config: the first
 * "secret=" value or bare line wins, '#' lines and other key=value lines are
 * skipped. So a file holding just the passphrase works, and so does the
 * daemon's own config on the same machine. Truncated to the daemon's 127.
 */
static int load_secret(char *out, size_t cap)
{
	out[0] = '\0';
	if (g_secret_file) {
		FILE *f = fopen(g_secret_file, "r");
		if (!f) {
			fprintf(stderr, "wireviewctl: %s: %s\n", g_secret_file, strerror(errno));
			return -1;
		}
		char line[192];
		while (!out[0] && fgets(line, sizeof(line), f)) {
			size_t n = strlen(line);
			while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
				     line[n - 1] == ' ' || line[n - 1] == '\t'))
				line[--n] = '\0';
			char *p = line;
			while (*p == ' ' || *p == '\t') p++;
			if (*p == '#' || *p == '\0')
				continue;
			char *eq = strchr(p, '=');
			if (eq) {
				*eq = '\0';
				char *val = eq + 1;
				while (*val == ' ' || *val == '\t') val++;
				if (strcmp(p, "secret") == 0)
					snprintf(out, cap, "%s", val);
			} else {
				snprintf(out, cap, "%s", p);
			}
		}
		explicit_bzero(line, sizeof(line));
		fclose(f);
		if (!out[0]) {
			fprintf(stderr, "wireviewctl: no secret found in %s\n", g_secret_file);
			return -1;
		}
		return 0;
	}

	const char *env = getenv("WIREVIEW_SECRET");
	if (env && env[0]) {
		snprintf(out, cap, "%s", env);
		return 0;
	}
	fprintf(stderr, "wireviewctl: remote writes are signed: pass --secret-file FILE "
		"or set WIREVIEW_SECRET\n");
	return -1;
}

static void remote_unreachable(void)
{
	fprintf(stderr, "wireviewctl: cannot reach wireviewd at %s\n"
		"(is remote_enabled=1 set in its /etc/wireview/config?)\n", g_host);
}

/* Non-2xx reply: show the status and the daemon's {"error":...} body. */
static void remote_fail(int status, char *body)
{
	size_t n = strlen(body);
	while (n && (body[n - 1] == '\n' || body[n - 1] == '\r'))
		body[--n] = '\0';
	fprintf(stderr, "wireviewctl: %s: HTTP %d%s%s\n", g_host, status,
		body[0] ? ": " : "", body);
}

/*
 * POST /command with the daemon's auth headers: X-Auth-Ts (unix time),
 * X-Auth-Nonce (16 random bytes, hex) and X-Auth-Sig, the lowercase hex
 * HMAC-SHA256 of "ts\nnonce\nbody" under the shared secret. Returns 0 on a
 * 2xx reply, 1 otherwise (after printing why).
 */
static int http_command(const char *json)
{
	char secret[128];
	if (load_secret(secret, sizeof(secret)) < 0)
		return 1;

	char ts[24], nonce[33], sig[65];
	uint8_t rnd[16], mac[32];
	snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
	if (random_bytes(rnd, sizeof(rnd)) < 0) {
		fprintf(stderr, "wireviewctl: no random source for the nonce\n");
		explicit_bzero(secret, sizeof(secret));
		return 1;
	}
	hex_encode(rnd, sizeof(rnd), nonce);

	size_t mcap = strlen(ts) + strlen(nonce) + strlen(json) + 3;
	char *msg = malloc(mcap);
	if (!msg) {
		explicit_bzero(secret, sizeof(secret));
		return 1;
	}
	int mlen = snprintf(msg, mcap, "%s\n%s\n%s", ts, nonce, json);
	hmac_sha256((const uint8_t *)secret, strlen(secret),
		    (const uint8_t *)msg, (size_t)mlen, mac);
	hex_encode(mac, sizeof(mac), sig);
	explicit_bzero(secret, sizeof(secret));
	free(msg);

	char hdrs[256];
	snprintf(hdrs, sizeof(hdrs),
		 "X-Auth-Ts: %s\r\nX-Auth-Nonce: %s\r\nX-Auth-Sig: %s\r\n", ts, nonce, sig);

	char resp[4096];
	int status = 0;
	if (http_request(g_host, "POST", "/command", json, hdrs, 5000,
			 resp, sizeof(resp), &status) < 0) {
		remote_unreachable();
		return 1;
	}
	if (status < 200 || status > 299) {
		remote_fail(status, resp);
		return 1;
	}
	return 0;
}

/* GET /sensors from --host into body; 0 on a 200 reply. */
static int remote_get_sensors(char *body, size_t cap)
{
	int status = 0;
	if (http_request(g_host, "GET", "/sensors", NULL, NULL, 3000,
			 body, cap, &status) < 0) {
		remote_unreachable();
		return -1;
	}
	if (status != 200) {
		remote_fail(status, body);
		return -1;
	}
	return 0;
}

/* info / build over HTTP: the /sensors fields fwVer, id and buildString.
 * The config version is not published there, so info omits it. */
static int remote_info(int build_only)
{
	static char body[16384];
	struct wv_snap s;
	if (remote_get_sensors(body, sizeof(body)) < 0)
		return 1;
	if (parse_remote(g_host, body, &s, 1) < 1) {
		fprintf(stderr, "wireviewctl: %s: device not connected\n", g_host);
		return 1;
	}
	if (build_only) {
		printf("build: %s\n", s.build[0] ? s.build : "(empty)");
		return 0;
	}
	printf("firmware: %s\n", s.fw);
	printf("uid: ");
	for (const char *p = s.uid; *p; p++)	/* lowercase, as the local path prints */
		putchar(*p >= 'A' && *p <= 'F' ? *p - 'A' + 'a' : *p);
	printf("\n");
	if (s.build[0])
		printf("build: %s\n", s.build);
	return 0;
}

/* read-config over HTTP: GET /config -> {"deviceId","version","data":b64}. */
static int remote_read_config(void)
{
	char body[2048];
	int status = 0;
	if (http_request(g_host, "GET", "/config", NULL, NULL, 5000,
			 body, sizeof(body), &status) < 0) {
		remote_unreachable();
		return 1;
	}
	if (status != 200) {
		remote_fail(status, body);
		return 1;
	}

	char b64[1024];
	uint8_t cfg[512];
	double ver;
	js_str(body, "data", b64, sizeof(b64));
	int n = b64_decode(b64, cfg, sizeof(cfg));
	if (n <= 0 || !js_num(body, "version", &ver)) {
		fprintf(stderr, "wireviewctl: %s: malformed /config reply\n", g_host);
		return 1;
	}
	fprintf(stderr, "config_version: %d, size: %d bytes\n", (int)ver, n);
	for (int i = 0; i < n; i++)
		printf("%02x", cfg[i]);
	printf("\n");
	return 0;
}

/* ---------- sensors output (plain and JSON, from one snapshot) ---------- */

static void print_sensors_plain(const struct wv_snap *s)
{
	for (int i = 0; i < 8; i++)
		if (s->have_in & (1u << i))
			printf("%s: %lld\n", in_labels[i], s->in_mv[i]);
	for (int i = 0; i < 7; i++)
		if (s->have_curr & (1u << i))
			printf("%s: %lld\n", curr_labels[i], s->curr_ma[i]);
	for (int i = 0; i < 7; i++)
		if (s->have_power & (1u << i))
			printf("%s: %lld\n", power_labels[i], s->power_uw[i]);
	for (int i = 0; i < 4; i++)
		if (s->have_temp & (1u << i))
			printf("%s: %lld\n", temp_labels[i], s->temp_mc[i]);
	if (s->fan >= 0)
		printf("fan_duty: %d\n", s->fan);
	if (s->have_fault_status)
		printf("fault_status: %u\n", s->fault_status);
	if (s->have_fault_log)
		printf("fault_log: %u\n", s->fault_log);
	if (s->psu_cap_w > 0)
		printf("psu_cap: %dW\n", s->psu_cap_w);
	else if (s->psu_cap_w == 0)
		printf("psu_cap: unknown\n");
	if (s->have_energy)
		printf("energy_uj: %lld\n", s->energy_uj);
	for (int i = 0; i < WV_NALARM; i++)
		if (s->alarm[i] >= 0)
			printf("alarm_%s: %d\n", alarm_attrs[i].name, s->alarm[i]);
}

/*
 * Escape a string for a JSON string literal, exactly as wireviewd does:
 * '"' and '\\' are backslash-escaped, control characters become \u00XX and
 * bytes >= 0x80 become '?'. Always NUL-terminates, never splits an escape.
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
 * Same schema as wireviewd's GET /sensors, with the values computed the same
 * way (sums from the pins, absent temperatures as 0.0), plus "energyJ" when
 * the module has energy1_input. s == NULL (no hwmon device) gives an empty
 * device list, like the daemon with no device. id, fwVer and buildString come
 * from the daemon socket and are "" when it cannot be reached; "connected"
 * is false when the module has no fresh readings.
 */
static void print_sensors_json(const struct wv_snap *s)
{
	char host[64] = "wireview";
	gethostname(host, sizeof(host) - 1);
	host[sizeof(host) - 1] = '\0';
	char host_js[sizeof(host) * 6];
	json_escape(host, host_js, sizeof(host_js));

	printf("{\"host\":\"%s\",\"appVersion\":\"wireviewctl\",\"devices\":[", host_js);
	if (s) {
		char ts[32];
		time_t now = time(NULL);
		struct tm tmv;
		gmtime_r(&now, &tmv);
		strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);

		char name_js[sizeof(s->name) * 6], uid_js[sizeof(s->uid) * 6];
		char fw_js[sizeof(s->fw) * 6], build_js[sizeof(s->build) * 6];
		json_escape(s->name, name_js, sizeof(name_js));
		json_escape(s->uid, uid_js, sizeof(uid_js));
		json_escape(s->fw, fw_js, sizeof(fw_js));
		json_escape(s->build, build_js, sizeof(build_js));

		double pv[6], pc[6], sum_p = 0, sum_c = 0, t[4];
		for (int i = 0; i < 6; i++) {
			pv[i] = s->in_mv[i] / 1000.0;
			pc[i] = s->curr_ma[i] / 1000.0;
			sum_p += pv[i] * pc[i];
			sum_c += pc[i];
		}
		for (int i = 0; i < 4; i++)
			t[i] = (s->have_temp & (1u << i)) ? s->temp_mc[i] / 1000.0 : 0.0;

		printf("{\"id\":\"%s\",\"name\":\"%s\",\"connected\":%s,"
		       "\"hwRev\":\"\",\"fwVer\":\"%s\",\"buildString\":\"%s\",\"timestamp\":\"%s\","
		       "\"pinVoltage\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f],"
		       "\"pinCurrent\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f],"
		       "\"tempInC\":%.1f,\"tempOutC\":%.1f,\"ext1C\":%.1f,\"ext2C\":%.1f,"
		       "\"psuCapW\":%d,\"fan\":%d,\"faultStatus\":%u,\"faultLog\":%u,"
		       "\"sumCurrentA\":%.3f,\"sumPowerW\":%.3f",
		       uid_js, name_js, s->ok ? "true" : "false",
		       fw_js, build_js, ts,
		       pv[0], pv[1], pv[2], pv[3], pv[4], pv[5],
		       pc[0], pc[1], pc[2], pc[3], pc[4], pc[5],
		       t[0], t[1], t[2], t[3],
		       s->psu_cap_w > 0 ? s->psu_cap_w : 0, s->fan > 0 ? s->fan : 0,
		       s->fault_status, s->fault_log, sum_c, sum_p);
		if (s->have_energy)
			printf(",\"energyJ\":%.3f", s->energy_uj / 1e6);
		printf("}");
	}
	printf("]}\n");
}

static int cmd_sensors(int argc, char **argv)
{
	int json = 0;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0) {
			json = 1;
		} else {
			fprintf(stderr, "wireviewctl: sensors: unknown option '%s'\n", argv[i]);
			return 1;
		}
	}

	struct wv_snap s;
	if (g_host) {
		/* --json passes the daemon's own document through unchanged. */
		static char body[16384];
		if (remote_get_sensors(body, sizeof(body)) < 0)
			return 1;
		int n = parse_remote(g_host, body, &s, 1);
		if (json) {
			size_t l = strlen(body);
			printf("%s%s", body, l && body[l - 1] == '\n' ? "" : "\n");
		}
		if (n < 1) {
			fprintf(stderr, "wireviewctl: %s: device not connected\n", g_host);
			return 1;
		}
		if (!json)
			print_sensors_plain(&s);
		return 0;
	}

	if (read_local(&s) < 0) {
		fprintf(stderr, "wireviewctl: wireview hwmon device not found\n"
			"Is the wireview_hwmon module loaded?\n");
		if (json)
			print_sensors_json(NULL);
		return 1;
	}
	if (json)
		print_sensors_json(&s);
	else
		print_sensors_plain(&s);
	return 0;
}

/* ---------- top drawing ---------- */

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
	if (s->psu_cap_w > 0) printf(" " HR " cap %dW", s->psu_cap_w);
	printf(ESC "[0m\n");

	double pin_v[6], pin_c[6];
	for (int p = 0; p < 6; p++) {
		pin_v[p] = s->in_mv[p] / 1000.0;
		pin_c[p] = s->curr_ma[p] / 1000.0;
	}
	double sum_w = s->power_uw[0] / 1e6, sum_a = s->curr_ma[6] / 1000.0;

	double cap = s->psu_cap_w > 0 ? s->psu_cap_w : 300.0;
	printf(ESC "[96m" VB ESC "[0m Power   ");
	print_bar(sum_w / cap, 28);
	printf("  %7.1f W", sum_w);
	if (s->have_energy)
		printf("  %10.3f Wh", s->energy_uj / 3.6e9);
	printf("\n");
	printf(ESC "[96m" VB ESC "[0m Current ");
	print_bar(sum_a / (cap / 12.0), 28);
	printf("  %7.2f A\n", sum_a);

	/* per-pin breakdown, one metric per row so each is easy to scan/compare */
	printf(ESC "[96m" VB ESC "[0;90m Pin   ");
	for (int p = 0; p < 6; p++) printf("%8d", p + 1);
	printf(ESC "[0m\n");
	printf(ESC "[96m" VB ESC "[0m Volts ");
	for (int p = 0; p < 6; p++) printf("%8.2f", pin_v[p]);
	printf("\n");
	printf(ESC "[96m" VB ESC "[0m Amps  ");
	for (int p = 0; p < 6; p++) printf("%8.2f", pin_c[p]);
	printf("\n");
	printf(ESC "[96m" VB ESC "[0m Watts ");
	for (int p = 0; p < 6; p++) printf("%8.1f", pin_v[p] * pin_c[p]);
	printf("\n");

	printf(ESC "[96m" VB ESC "[0m Temp   ");
	static const char *tn[] = { "In", "Out", "E1", "E2" };
	for (int t = 0; t < 4; t++) {
		if (!(s->have_temp & (1u << t)))
			printf("%s -- ", tn[t]);
		else
			printf("%s %.1f" DEG " ", tn[t], s->temp_mc[t] / 1000.0);
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

/* Append a host unless it is already listed (hosts file, global --host and
 * top's --host may name the same one). */
static void add_top_host(char hosts[][80], int *nhost, const char *h)
{
	if (*nhost >= WV_MAXHOST || !h[0])
		return;
	for (int i = 0; i < *nhost; i++)
		if (strcmp(hosts[i], h) == 0)
			return;
	snprintf(hosts[(*nhost)++], 80, "%s", h);
}

static int cmd_top(int argc, char **argv)
{
	char hosts[WV_MAXHOST][80];
	int nhost = 0;
	int interval_ms = 1000;
	int watch_stdin = 1;	/* q to quit; cleared once stdin hits EOF */

	FILE *f = fopen("/etc/wireview/hosts", "r");
	if (f) {
		char line[100];
		while (fgets(line, sizeof(line), f) && nhost < WV_MAXHOST) {
			size_t l = strlen(line);
			while (l && (line[l-1] == '\n' || line[l-1] == '\r' || line[l-1] == ' ')) line[--l] = '\0';
			char *t = line;
			while (*t == ' ' || *t == '\t') t++;
			if (*t && *t != '#') add_top_host(hosts, &nhost, t);
		}
		fclose(f);
	}
	/* The global --host (before "top") adds one more host; top's own
	 * --host after the command still works and may repeat. */
	if (g_host)
		add_top_host(hosts, &nhost, g_host);
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
			/* one flag may carry several hosts: --host a,b c */
			char *tok = strtok(argv[++i], ", ");
			while (tok) {
				add_top_host(hosts, &nhost, tok);
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

		/* poll() ignores a negative fd, so once stdin is gone this
		 * is a plain interval sleep. */
		struct pollfd pfd = { .fd = watch_stdin ? STDIN_FILENO : -1,
				      .events = POLLIN };
		if (poll(&pfd, 1, interval_ms) == 1) {
			char c;
			ssize_t r = read(STDIN_FILENO, &c, 1);
			if (r == 1 && (c == 'q' || c == 'Q'))
				break;
			/* EOF (</dev/null, closed pipe, hung-up tty) stays
			 * readable forever: stop watching or we spin. */
			if (r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN))
				watch_stdin = 0;
		}
	}
	term_restore();
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: wireviewctl [--host H[:port] [--secret-file FILE]] <command> [args]\n"
		"\n"
		"Global options:\n"
		"  --host H[:port]   Talk to wireviewd on H over HTTP (default port 9876)\n"
		"                    instead of the local socket: info, build, sensors and\n"
		"                    read-config read; screen, nvm, clear-faults and\n"
		"                    write-config are signed with the shared secret.\n"
		"                    bootloader and flash are local only.\n"
		"  --secret-file FILE  Shared secret for signed writes: the passphrase, or a\n"
		"                    file with a secret= line (like /etc/wireview/config).\n"
		"                    Without it, $WIREVIEW_SECRET is used.\n"
		"\n"
		"Commands (require wireviewd running):\n"
		"  info              Show device firmware, UID, and build info\n"
		"  clear-faults [STATUS_MASK [LOG_MASK]]\n"
		"                    Clear faults. Masks are hex bits to clear (default FFFF,\n"
		"                    i.e. all active faults and the whole fault log)\n"
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
		"  sensors [--json]  Show all sensor readings from hwmon sysfs; --json prints\n"
		"                    the same schema as the daemon's GET /sensors\n"
		"\n"
		"Monitor:\n"
		"  top [--host H[:port][,H2...]]... [--interval MS]\n"
		"                    Live dashboard: the local device plus remote hosts.\n"
		"                    --host repeats and/or takes a comma/space list; hosts are\n"
		"                    also read from /etc/wireview/hosts, and the global\n"
		"                    --host (before \"top\") adds one more. Press q to quit.\n"
		"\n"
		"Other:\n"
		"  -V, --version     Print the wireviewctl version\n"
	);
}

int main(int argc, char **argv)
{
	/* Global options come before the command; shift them off so the
	 * commands keep seeing their own arguments from argv[2]. */
	int argi = 1;
	while (argi < argc) {
		const char *o = argv[argi];
		if (strcmp(o, "--host") == 0 || strcmp(o, "--secret-file") == 0) {
			if (argi + 1 >= argc) {
				fprintf(stderr, "wireviewctl: %s needs an argument\n", o);
				return 1;
			}
			if (o[2] == 'h')
				g_host = argv[argi + 1];
			else
				g_secret_file = argv[argi + 1];
			argi += 2;
		} else if (strncmp(o, "--host=", 7) == 0) {
			g_host = o + 7;
			argi++;
		} else if (strncmp(o, "--secret-file=", 14) == 0) {
			g_secret_file = o + 14;
			argi++;
		} else if (strcmp(o, "--secret") == 0 || strncmp(o, "--secret=", 9) == 0) {
			fprintf(stderr, "wireviewctl: no --secret option: a secret on the command "
				"line is visible in ps.\nUse --secret-file FILE or WIREVIEW_SECRET.\n");
			return 1;
		} else {
			break;
		}
	}
	if (g_host && !g_host[0]) {
		fprintf(stderr, "wireviewctl: --host needs a host name\n");
		return 1;
	}
	argv += argi - 1;
	argc -= argi - 1;

	if (argc < 2) {
		usage();
		return 1;
	}

	const char *cmd = argv[1];

	if (g_host && (strcmp(cmd, "bootloader") == 0 || strcmp(cmd, "flash") == 0)) {
		fprintf(stderr, "wireviewctl: %s is local only: wireviewd never exposes the "
			"bootloader over the network.\nRun it on the machine the device is "
			"attached to.\n", cmd);
		return 1;
	}

	if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
		printf("wireviewctl %s\n", WIREVIEW_PKG_VERSION);
		return 0;
	}
	if (strcmp(cmd, "info") == 0)
		return cmd_info();
	if (strcmp(cmd, "clear-faults") == 0) {
		if (argc > 4) {
			fprintf(stderr, "wireviewctl: clear-faults takes at most STATUS_MASK LOG_MASK\n");
			return 1;
		}
		return cmd_clear_faults(argc > 2 ? argv[2] : NULL,
					argc > 3 ? argv[3] : NULL);
	}
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
		return cmd_sensors(argc, argv);
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
