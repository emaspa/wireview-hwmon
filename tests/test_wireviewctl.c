/*
 * Unit tests for wireviewctl's parsers: the Intel HEX firmware loader, the
 * clear-faults mask parser and the remote /sensors JSON reader.
 *
 * wireviewctl.c is one file of static functions, so it is compiled into
 * this test directly with its main() renamed out of the way.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#define main wireviewctl_main
#include "../wireviewctl.c"
#undef main

#include <sys/stat.h>
#include "check.h"

static char g_tmpdir[] = "/tmp/wireview-test-ctl-XXXXXX";

/* Write text to a file in the temp dir; returns its path (static). */
static const char *write_file(const char *name, const void *data, size_t len)
{
	static char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_tmpdir, name);
	f = fopen(path, "wb");
	if (!f)
		return NULL;
	fwrite(data, 1, len, f);
	fclose(f);
	return path;
}

/* Append one Intel HEX record (with checksum) to buf. */
static void hex_record(char *buf, unsigned addr, unsigned type,
		       const uint8_t *data, unsigned n)
{
	unsigned sum = n + (addr >> 8) + (addr & 0xFF) + type;
	char *p = buf + strlen(buf);

	p += sprintf(p, ":%02X%04X%02X", n, addr & 0xFFFF, type);
	for (unsigned i = 0; i < n; i++) {
		p += sprintf(p, "%02X", data[i]);
		sum += data[i];
	}
	sprintf(p, "%02X\n", (0x100 - (sum & 0xFF)) & 0xFF);
}

/* Encode img (len bytes at base) as Intel HEX with 16-byte data records,
 * emitting a type 04 (extended linear address) record at the start and
 * whenever the upper 16 address bits change. */
static void hex_image(char *buf, uint32_t base, const uint8_t *img, size_t len)
{
	uint32_t upper = 0xFFFFFFFFu;

	buf[0] = '\0';
	for (size_t off = 0; off < len; ) {
		uint32_t a = base + (uint32_t)off;
		unsigned n = 16;

		if ((a >> 16) != upper) {
			uint8_t ela[2] = { (uint8_t)(a >> 24), (uint8_t)(a >> 16) };
			upper = a >> 16;
			hex_record(buf, 0, 4, ela, 2);
		}
		/* never let a record run across a 64 KiB boundary */
		if ((a & 0xFFFF) + n > 0x10000)
			n = 0x10000 - (a & 0xFFFF);
		if (off + n > len)
			n = (unsigned)(len - off);
		hex_record(buf, a & 0xFFFF, 0, img + off, n);
		off += n;
	}
	hex_record(buf, 0, 1, NULL, 0);
}

static void test_load_firmware_handwritten(void)
{
	/* ELA 0x0800, 16 bytes at 0x08000000, ELA 0x0801, one byte at
	 * 0x08010000; the gap is filled with 0xFF. */
	static const char hex[] =
		":020000040800F2\n"
		":10000000000102030405060708090A0B0C0D0E0F78\n"
		":020000040801F1\n"
		":0100000055AA\n"
		":00000001FF\n";
	const char *path = write_file("hand.hex", hex, sizeof(hex) - 1);
	uint8_t *img = NULL;
	uint32_t base = 0;
	int ver = 0;
	char build[40] = "x";

	long len = load_firmware(path, &img, &base, &ver, build, sizeof(build));
	CHECK_EQ_INT(len, 0x10001);
	CHECK_EQ_INT(base, 0x08000000u);
	CHECK(img != NULL);
	if (img && len == 0x10001) {
		for (int i = 0; i < 16; i++)
			CHECK_EQ_INT(img[i], i);
		CHECK_EQ_INT(img[16], 0xFF);
		CHECK_EQ_INT(img[0xFFFF], 0xFF);
		CHECK_EQ_INT(img[0x10000], 0x55);
		/* offsets 194 and 227.. fall in the 0xFF gap */
		CHECK_EQ_INT(ver, 0xFF);
		CHECK_EQ_INT(strlen(build), 32);
	}
	free(img);

	/* Type 02 (extended segment address): upper = 0x1000 << 4. The
	 * image starts at the first data byte, not at 0x08000000. */
	static const char seg[] =
		":020000021000EC\n"
		":02001000ABCD76\n"
		":00000001FF\n";
	path = write_file("seg.hex", seg, sizeof(seg) - 1);
	img = NULL;
	len = load_firmware(path, &img, &base, &ver, build, sizeof(build));
	CHECK_EQ_INT(len, 2);
	CHECK_EQ_INT(base, 0x10010);
	if (img && len == 2)
		CHECK(img[0] == 0xAB && img[1] == 0xCD);
	/* Too short to hold the version byte or build string. */
	CHECK_EQ_INT(ver, -1);
	CHECK_EQ_STR(build, "");
	free(img);

	/* Records after the EOF record are ignored. */
	static const char eof[] =
		":0100000011EE\n"
		":00000001FF\n"
		":0100100022CD\n";
	path = write_file("eof.hex", eof, sizeof(eof) - 1);
	img = NULL;
	len = load_firmware(path, &img, &base, &ver, NULL, 0);
	CHECK_EQ_INT(len, 1);
	CHECK_EQ_INT(base, 0);
	free(img);
}

static void test_load_firmware_buildstruct(void)
{
	/* A 300-byte image straddling a 64 KiB boundary, so the file needs
	 * two ELA records; BuildStruct version at +194, build at +227. */
	enum { LEN = 300 };
	const uint32_t base = 0x0800FF80u;
	uint8_t img_in[LEN];
	static char hex[8192];

	for (int i = 0; i < LEN; i++)
		img_in[i] = (uint8_t)(i ^ 0x5A);
	img_in[194] = 7;
	memset(img_in + 227, 0, 33);
	memcpy(img_in + 227, "WV2 2025-06-01 build 42", 23);
	hex_image(hex, base, img_in, LEN);

	CHECK(strstr(hex, ":020000040800F2") != NULL);
	CHECK(strstr(hex, ":020000040801F1") != NULL);

	const char *path = write_file("build.hex", hex, strlen(hex));
	uint8_t *img = NULL;
	uint32_t got_base = 0;
	int ver = -1;
	char build[40];

	long len = load_firmware(path, &img, &got_base, &ver, build, sizeof(build));
	CHECK_EQ_INT(len, LEN);
	CHECK_EQ_INT(got_base, base);
	if (img && len == LEN)
		CHECK_EQ_MEM(img, img_in, LEN);
	CHECK_EQ_INT(ver, 7);
	CHECK_EQ_STR(build, "WV2 2025-06-01 build 42");
	free(img);

	/* A build string that fills all 32 bytes is cut at 32. */
	memset(img_in + 227, 'B', 32);
	hex_image(hex, base, img_in, LEN);
	path = write_file("build32.hex", hex, strlen(hex));
	img = NULL;
	len = load_firmware(path, &img, &got_base, &ver, build, sizeof(build));
	CHECK_EQ_INT(len, LEN);
	CHECK_EQ_INT(strlen(build), 32);
	free(img);

	/* ... and to the caller's buffer. */
	char small[8];
	img = NULL;
	len = load_firmware(path, &img, &got_base, &ver, small, sizeof(small));
	CHECK_EQ_STR(small, "BBBBBBB");
	free(img);

	/* The same image as a raw .bin loads at the default base. */
	img_in[0] = 0x20;	/* anything but ':' */
	path = write_file("fw.bin", img_in, LEN);
	img = NULL;
	len = load_firmware(path, &img, &got_base, &ver, build, sizeof(build));
	CHECK_EQ_INT(len, LEN);
	CHECK_EQ_INT(got_base, 0x08000000u);
	CHECK_EQ_INT(ver, 7);
	if (img && len == LEN)
		CHECK_EQ_MEM(img, img_in, LEN);
	free(img);
}

static void test_load_firmware_errors(void)
{
	uint8_t *img = NULL;
	uint32_t base;
	int ver;
	long len;
	const char *path;

	/* No data records at all. */
	static const char empty[] = ":00000001FF\n";
	path = write_file("empty.hex", empty, sizeof(empty) - 1);
	QUIET(len = load_firmware(path, &img, &base, &ver, NULL, 0));
	CHECK_EQ_INT(len, -1);
	if (len >= 0) { free(img); img = NULL; }

	/* Data spread over more than 4 MiB. */
	static const char huge[] =
		":020000040800F2\n"
		":0100000011EE\n"
		":020000040900F1\n"
		":0100000022DD\n"
		":00000001FF\n";
	path = write_file("huge.hex", huge, sizeof(huge) - 1);
	QUIET(len = load_firmware(path, &img, &base, &ver, NULL, 0));
	CHECK_EQ_INT(len, -1);
	if (len >= 0) { free(img); img = NULL; }

	/* Empty .bin and a missing file. */
	path = write_file("empty.bin", "", 0);
	QUIET(len = load_firmware(path, &img, &base, &ver, NULL, 0));
	CHECK_EQ_INT(len, -1);
	if (len >= 0) { free(img); img = NULL; }
	QUIET(len = load_firmware("/nonexistent/fw.hex", &img, &base, &ver, NULL, 0));
	CHECK_EQ_INT(len, -1);
	if (len >= 0) { free(img); img = NULL; }
}

static void test_parse_fault_mask(void)
{
	uint16_t m;
	int rc;

	m = 1;
	CHECK_EQ_INT(parse_fault_mask("FFFF", &m), 0);
	CHECK_EQ_INT(m, 0xFFFF);
	CHECK_EQ_INT(parse_fault_mask("ffff", &m), 0);
	CHECK_EQ_INT(m, 0xFFFF);
	CHECK_EQ_INT(parse_fault_mask("0x1F", &m), 0);
	CHECK_EQ_INT(m, 0x1F);
	CHECK_EQ_INT(parse_fault_mask("0X8000", &m), 0);
	CHECK_EQ_INT(m, 0x8000);
	CHECK_EQ_INT(parse_fault_mask("0", &m), 0);
	CHECK_EQ_INT(m, 0);
	CHECK_EQ_INT(parse_fault_mask("10", &m), 0);
	CHECK_EQ_INT(m, 0x10);	/* hex, not decimal */

	/* Invalid input fails and leaves *out unchanged. */
	static const char *const bad[] = {
		"", "xyz", "12g", "0x", "FFFF ", "10000", "0x10000", "-1",
		"FFFFFFFFFFFFFFFFFFFF",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		m = 0x1234;
		QUIET(rc = parse_fault_mask(bad[i], &m));
		CHECK_EQ_INT(rc, -1);
		CHECK_EQ_INT(m, 0x1234);
	}
}

static void test_parse_remote(void)
{
	/* Two devices: the first as wireviewd publishes it (disconnected
	 * externals read 0.0), the second as the desktop app does (-100.0
	 * sentinel, no name, no fan key). */
	static const char body[] =
		"{\"host\":\"rig\",\"appVersion\":\"wireviewd\",\"devices\":["
		"{\"id\":\"0102030405060708090A0B0C\",\"name\":\"WireView Pro II\","
		"\"connected\":true,\"hwRev\":\"\",\"fwVer\":\"7\","
		"\"buildString\":\"FAKE build\",\"timestamp\":\"2026-01-01T00:00:00Z\","
		"\"pinVoltage\":[12.000,12.050,11.990,12.010,12.020,12.030],"
		"\"pinCurrent\":[8.000,8.100,7.900,8.050,0.000,8.200],"
		"\"tempInC\":35.5,\"tempOutC\":40.0,\"ext1C\":0.0,\"ext2C\":0.0,"
		"\"psuCapW\":600,\"fan\":42,\"faultStatus\":3,\"faultLog\":256,"
		"\"sumCurrentA\":40.250,\"sumPowerW\":483.100},"
		"{\"id\":\"AA\",\"fwVer\":\"9\","
		"\"pinVoltage\":[1, 2, 3, 4, 5, 6],"
		"\"pinCurrent\":[0.5,0.5,0.5,0.5,0.5,0.5],"
		"\"tempInC\":20.0,\"tempOutC\":21.0,\"ext1C\":-100.0,\"ext2C\":25.5,"
		"\"psuCapW\":300,\"faultStatus\":0,\"faultLog\":0,"
		"\"sumCurrentA\":3.0,\"sumPowerW\":10.5}"
		"]}";
	struct wv_snap s[4];

	memset(s, 0xAB, sizeof(s));
	int n = parse_remote("rig:9876", body, s, 4);
	CHECK_EQ_INT(n, 2);

	CHECK_EQ_STR(s[0].source, "rig:9876");
	CHECK_EQ_STR(s[0].name, "WireView Pro II");
	CHECK_EQ_STR(s[0].fw, "7");
	CHECK(s[0].ok == 1);
	CHECK(s[0].pin_v[0] == 12.0 && s[0].pin_v[5] == 12.03);
	CHECK(s[0].pin_c[1] == 8.1 && s[0].pin_c[4] == 0.0);
	CHECK(s[0].temp[0] == 35.5 && s[0].temp[1] == 40.0);
	CHECK(s[0].temp[2] == TEMP_NA && s[0].temp[3] == TEMP_NA);
	CHECK_EQ_INT(s[0].psu_cap_w, 600);
	CHECK_EQ_INT(s[0].fan, 42);
	CHECK_EQ_INT(s[0].fault_status, 3);
	CHECK_EQ_INT(s[0].fault_log, 256);
	CHECK(s[0].sum_a == 40.25 && s[0].sum_w == 483.1);

	CHECK_EQ_STR(s[1].source, "rig:9876");
	CHECK_EQ_STR(s[1].name, "WireView");	/* default when absent */
	CHECK_EQ_STR(s[1].fw, "9");
	CHECK(s[1].pin_v[0] == 1.0 && s[1].pin_v[5] == 6.0);
	CHECK(s[1].temp[2] == TEMP_NA);		/* app sentinel */
	CHECK(s[1].temp[3] == 25.5);
	CHECK_EQ_INT(s[1].fan, -1);		/* no fan key */
	CHECK_EQ_INT(s[1].psu_cap_w, 300);
	/* Values from the first object do not leak into the second. */
	CHECK_EQ_INT(s[1].fault_status, 0);
	CHECK(s[1].sum_w == 10.5);

	/* max caps the number of devices written. */
	memset(s, 0, sizeof(s));
	CHECK_EQ_INT(parse_remote("h", body, s, 1), 1);
	CHECK_EQ_STR(s[1].source, "");

	/* No device (daemon without a frame yet), or not /sensors at all. */
	CHECK_EQ_INT(parse_remote("h", "{\"host\":\"x\",\"appVersion\":\"wireviewd\","
				  "\"devices\":[]}", s, 4), 0);
	CHECK_EQ_INT(parse_remote("h", "<html>404</html>", s, 4), 0);
	CHECK_EQ_INT(parse_remote("h", "", s, 4), 0);

	/* A truncated body stops at the last complete object. */
	char cut[sizeof(body)];
	const char *second = strstr(body, "{\"id\":\"AA\"");
	size_t keep = (size_t)(second - body) + 20;
	memcpy(cut, body, keep);
	cut[keep] = '\0';
	CHECK_EQ_INT(parse_remote("h", cut, s, 4), 1);
}

int main(void)
{
	if (!mkdtemp(g_tmpdir)) {
		perror("mkdtemp");
		return 1;
	}

	test_load_firmware_handwritten();
	test_load_firmware_buildstruct();
	test_load_firmware_errors();
	test_parse_fault_mask();
	test_parse_remote();

	char cmd[PATH_MAX + 16];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: could not remove %s\n", g_tmpdir);

	return check_summary("test_wireviewctl");
}
