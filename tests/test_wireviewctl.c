/*
 * Unit tests for wireviewctl: the Intel HEX firmware loader, the
 * clear-faults mask parser, the remote /sensors JSON reader, the sysfs
 * reader (against a fake hwmon directory via $WIREVIEW_HWMON_PATH) and
 * the "sensors" / "sensors --json" output.
 *
 * wireviewctl.c is one file of static functions, so it is compiled into
 * this test directly with its main() renamed out of the way.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#define main wireviewctl_main
#include "../wireviewctl.c"
#undef main

#include <ctype.h>
#include <sys/stat.h>
#include "check.h"

#ifndef WV_SRCDIR
#define WV_SRCDIR ".."
#endif

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

	/* Strings, and readings converted to sysfs units (mV, mA, uW, m°C). */
	CHECK_EQ_STR(s[0].source, "rig:9876");
	CHECK_EQ_STR(s[0].name, "WireView Pro II");
	CHECK_EQ_STR(s[0].fw, "7");
	CHECK_EQ_STR(s[0].uid, "0102030405060708090A0B0C");
	CHECK_EQ_STR(s[0].build, "FAKE build");
	CHECK(s[0].ok == 1);
	static const long long mv0[6] = { 12000, 12050, 11990, 12010, 12020, 12030 };
	static const long long ma0[6] = { 8000, 8100, 7900, 8050, 0, 8200 };
	/* Per-pin power is V * A, rounded to the nearest uW. */
	static const long long uw0[6] = {
		96000000, 97605000, 94721000, 96680500, 0, 98646000
	};
	for (int i = 0; i < 6; i++) {
		CHECK_EQ_INT(s[0].in_mv[i], mv0[i]);
		CHECK_EQ_INT(s[0].curr_ma[i], ma0[i]);
		CHECK_EQ_INT(s[0].power_uw[i + 1], uw0[i]);
	}
	CHECK_EQ_INT(s[0].curr_ma[6], 40250);		/* sumCurrentA */
	CHECK_EQ_INT(s[0].power_uw[0], 483100000);	/* sumPowerW */
	/* No average / vdd over the network; every current and power. */
	CHECK_EQ_INT(s[0].have_in, 0x3F);
	CHECK_EQ_INT(s[0].have_curr, 0x7F);
	CHECK_EQ_INT(s[0].have_power, 0x7F);
	/* Disconnected externals (0.0 from wireviewd) are absent. */
	CHECK_EQ_INT(s[0].temp_mc[0], 35500);
	CHECK_EQ_INT(s[0].temp_mc[1], 40000);
	CHECK_EQ_INT(s[0].have_temp, 0x3);
	CHECK_EQ_INT(s[0].psu_cap_w, 600);
	CHECK_EQ_INT(s[0].fan, 42);
	CHECK_EQ_INT(s[0].fault_status, 3);
	CHECK_EQ_INT(s[0].fault_log, 256);
	CHECK(s[0].have_fault_status && s[0].have_fault_log);
	CHECK_EQ_INT(s[0].have_energy, 0);		/* no energyJ key */
	CHECK_EQ_INT(s[0].energy_uj, 0);
	for (int i = 0; i < WV_NALARM; i++)
		CHECK_EQ_INT(s[0].alarm[i], -1);	/* /sensors has none */

	CHECK_EQ_STR(s[1].source, "rig:9876");
	CHECK_EQ_STR(s[1].name, "WireView");	/* default when absent */
	CHECK_EQ_STR(s[1].fw, "9");
	CHECK_EQ_STR(s[1].uid, "AA");
	CHECK_EQ_STR(s[1].build, "");
	CHECK(s[1].ok == 1);			/* no "connected" key */
	CHECK_EQ_INT(s[1].in_mv[0], 1000);
	CHECK_EQ_INT(s[1].in_mv[5], 6000);
	CHECK_EQ_INT(s[1].curr_ma[3], 500);
	CHECK_EQ_INT(s[1].power_uw[6], 3000000);	/* 6 V * 0.5 A */
	/* ext1 is the app's -100 sentinel (absent), ext2 is present. */
	CHECK_EQ_INT(s[1].have_temp, 0xB);
	CHECK_EQ_INT(s[1].temp_mc[3], 25500);
	CHECK_EQ_INT(s[1].fan, -1);		/* no fan key */
	CHECK_EQ_INT(s[1].psu_cap_w, 300);
	/* Values from the first object do not leak into the second. */
	CHECK_EQ_INT(s[1].fault_status, 0);
	CHECK_EQ_INT(s[1].power_uw[0], 10500000);
	CHECK_EQ_INT(s[1].have_energy, 0);

	/* A newer wireviewd adds energyJ; a disconnected device, negative
	 * external temperatures and escaped strings. */
	static const char body3[] =
		"{\"host\":\"h\",\"appVersion\":\"wireviewd\",\"devices\":["
		"{\"id\":\"BB\",\"name\":\"say \\\"hi\\\" C:\\\\x\",\"connected\":false,"
		"\"fwVer\":\"8\",\"buildString\":\"\","
		"\"pinVoltage\":[12.000,0,0,0,0,0],\"pinCurrent\":[-0.001,0,0,0,0,0],"
		"\"tempInC\":-5.5,\"tempOutC\":0.0,\"ext1C\":-39.9,\"ext2C\":-100.0,"
		"\"psuCapW\":0,\"fan\":0,\"faultStatus\":65535,\"faultLog\":1,"
		"\"sumCurrentA\":0.0,\"sumPowerW\":0.0,\"energyJ\":12345.678}]}";
	memset(s, 0xAB, sizeof(s));
	CHECK_EQ_INT(parse_remote("h:1", body3, s, 4), 1);
	CHECK_EQ_STR(s[0].name, "say \"hi\" C:\\x");
	CHECK(s[0].ok == 0);
	CHECK_EQ_INT(s[0].have_energy, 1);
	CHECK_EQ_INT(s[0].energy_uj, 12345678000LL);	/* past 32 bits */
	CHECK_EQ_INT(s[0].curr_ma[0], -1);		/* rounds away from 0 */
	CHECK_EQ_INT(s[0].power_uw[1], -12000);
	/* Onboard sensors are always present, even at 0.0. */
	CHECK_EQ_INT(s[0].temp_mc[0], -5500);
	CHECK_EQ_INT(s[0].temp_mc[1], 0);
	CHECK_EQ_INT(s[0].temp_mc[2], -39900);
	CHECK_EQ_INT(s[0].have_temp, 0x7);
	CHECK_EQ_INT(s[0].psu_cap_w, 0);
	CHECK_EQ_INT(s[0].fan, 0);
	CHECK_EQ_INT(s[0].fault_status, 0xFFFF);

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

/* ---- a fake hwmon sysfs directory for read_local() ---- */

static char g_hwmon[PATH_MAX];

static void sysfs_set(const char *attr, const char *val)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_hwmon, attr);
	f = fopen(path, "w");
	if (!f) {
		perror(path);
		return;
	}
	fprintf(f, "%s\n", val);
	fclose(f);
}

static void sysfs_del(const char *attr)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", g_hwmon, attr);
	unlink(path);
}

/* A module with every attribute: pins 12.0-12.5 V, 8.0-8.5 A. */
static void sysfs_populate(void)
{
	char a[32], v[32];

	sysfs_set("name", "wireview");
	for (int i = 0; i < 6; i++) {
		snprintf(a, sizeof(a), "in%d_input", i);
		snprintf(v, sizeof(v), "%d", 12000 + 100 * i);
		sysfs_set(a, v);
		snprintf(a, sizeof(a), "curr%d_input", i + 1);
		snprintf(v, sizeof(v), "%d", 8000 + 100 * i);
		sysfs_set(a, v);
		snprintf(a, sizeof(a), "power%d_input", i + 2);
		snprintf(v, sizeof(v), "%lld",
			 (long long)(12000 + 100 * i) * (8000 + 100 * i));
		sysfs_set(a, v);
	}
	sysfs_set("in6_input", "12250");	/* average */
	sysfs_set("in7_input", "3300");		/* vdd */
	sysfs_set("curr7_input", "49500");
	sysfs_set("power1_input", "595000000");
	sysfs_set("temp1_input", "35500");
	sysfs_set("temp2_input", "-40000");
	sysfs_set("temp4_input", "25000");	/* temp3 disconnected */
	sysfs_set("pwm1", "107");
	sysfs_set("fan1_input", "99");		/* deprecated, ignored */
	sysfs_set("power1_cap", "450000000");
	sysfs_set("psu_cap", "3");		/* deprecated, ignored */
	sysfs_set("energy1_input", "123456789012");
	sysfs_set("fault_status_raw", "513");
	sysfs_set("fault_log_raw", "65535");
	sysfs_set("intrusion0_alarm", "1");
	sysfs_set("intrusion1_alarm", "1");
	for (int i = 0; i < WV_NALARM; i++)
		sysfs_set(alarm_attrs[i].attr, "0");
	sysfs_set("temp1_alarm", "1");
	sysfs_set("power1_alarm", "1");
}

static void test_read_local(void)
{
	struct wv_snap s;

	snprintf(g_hwmon, sizeof(g_hwmon), "%s/hwmon7", g_tmpdir);
	if (mkdir(g_hwmon, 0700) != 0) {
		perror(g_hwmon);
		CHECK(0);
		return;
	}
	setenv("WIREVIEW_HWMON_PATH", g_hwmon, 1);

	/* No "name" file: not a hwmon device, and no fallback to the real
	 * /sys/class/hwmon scan. */
	CHECK_EQ_INT(read_local(&s), -1);

	sysfs_populate();
	memset(&s, 0xAB, sizeof(s));
	CHECK_EQ_INT(read_local(&s), 0);
	CHECK_EQ_STR(s.source, "local");
	CHECK_EQ_STR(s.name, "WireView Pro II");
	/* The daemon socket does not exist here: no firmware info. */
	CHECK_EQ_STR(s.fw, "");
	CHECK_EQ_STR(s.uid, "");
	CHECK(s.ok == 1);
	CHECK_EQ_INT(s.have_in, 0xFF);
	CHECK_EQ_INT(s.have_curr, 0x7F);
	CHECK_EQ_INT(s.have_power, 0x7F);
	CHECK_EQ_INT(s.in_mv[0], 12000);
	CHECK_EQ_INT(s.in_mv[5], 12500);
	CHECK_EQ_INT(s.in_mv[6], 12250);
	CHECK_EQ_INT(s.in_mv[7], 3300);
	CHECK_EQ_INT(s.curr_ma[5], 8500);
	CHECK_EQ_INT(s.curr_ma[6], 49500);
	CHECK_EQ_INT(s.power_uw[0], 595000000);	/* power1 = total */
	CHECK_EQ_INT(s.power_uw[1], 96000000);	/* power2 = pin 1 */
	CHECK_EQ_INT(s.have_temp, 0xB);
	CHECK_EQ_INT(s.temp_mc[0], 35500);
	CHECK_EQ_INT(s.temp_mc[1], -40000);
	CHECK_EQ_INT(s.temp_mc[3], 25000);
	/* pwm1 wins over fan1_input: 107/255 is 42 %. */
	CHECK_EQ_INT(s.fan, 42);
	/* power1_cap (uW) wins over psu_cap (enum 3 = 150 W). */
	CHECK_EQ_INT(s.psu_cap_w, 450);
	/* energy1_input is microjoules and does not fit in 32 bits. */
	CHECK_EQ_INT(s.have_energy, 1);
	CHECK_EQ_INT(s.energy_uj, 123456789012LL);
	/* The raw fault masks win over the intrusion alarms. */
	CHECK(s.have_fault_status && s.have_fault_log);
	CHECK_EQ_INT(s.fault_status, 513);
	CHECK_EQ_INT(s.fault_log, 65535);
	for (int i = 0; i < WV_NALARM; i++)
		CHECK_EQ_INT(s.alarm[i], (i == 0 || i == WV_NALARM - 1) ? 1 : 0);

	/* pwm1 -> percent, rounded to nearest. */
	static const struct { const char *pwm; int pct; } pwm[] = {
		{ "0", 0 }, { "1", 0 }, { "2", 1 }, { "128", 50 },
		{ "191", 75 }, { "254", 100 }, { "255", 100 },
	};
	for (size_t i = 0; i < sizeof(pwm) / sizeof(pwm[0]); i++) {
		sysfs_set("pwm1", pwm[i].pwm);
		CHECK_EQ_INT(read_local(&s), 0);
		CHECK_EQ_INT(s.fan, pwm[i].pct);
	}

	/* power1_cap is truncated to whole watts; 0 is "unknown". */
	sysfs_set("power1_cap", "599999999");
	read_local(&s);
	CHECK_EQ_INT(s.psu_cap_w, 599);
	sysfs_set("power1_cap", "0");
	read_local(&s);
	CHECK_EQ_INT(s.psu_cap_w, 0);

	/* An older module: fan1_input and the psu_cap enum, no energy, no
	 * raw fault masks, no per-channel alarms. */
	sysfs_del("pwm1");
	sysfs_del("power1_cap");
	sysfs_del("energy1_input");
	sysfs_del("fault_status_raw");
	sysfs_del("fault_log_raw");
	for (int i = 0; i < WV_NALARM; i++)
		sysfs_del(alarm_attrs[i].attr);
	memset(&s, 0xAB, sizeof(s));
	CHECK_EQ_INT(read_local(&s), 0);
	CHECK_EQ_INT(s.fan, 99);
	CHECK_EQ_INT(s.psu_cap_w, 150);
	CHECK_EQ_INT(s.have_energy, 0);
	CHECK_EQ_INT(s.energy_uj, 0);
	CHECK(s.have_fault_status && s.have_fault_log);
	CHECK_EQ_INT(s.fault_status, 1);	/* intrusion0_alarm */
	CHECK_EQ_INT(s.fault_log, 1);		/* intrusion1_alarm */
	for (int i = 0; i < WV_NALARM; i++)
		CHECK_EQ_INT(s.alarm[i], -1);

	static const struct { const char *cap; int w; } caps[] = {
		{ "0", 600 }, { "1", 450 }, { "2", 300 }, { "3", 150 },
		{ "4", 0 }, { "-1", 0 },
	};
	for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
		sysfs_set("psu_cap", caps[i].cap);
		read_local(&s);
		CHECK_EQ_INT(s.psu_cap_w, caps[i].w);
	}

	/* Neither generation of an attribute: unavailable. */
	sysfs_del("fan1_input");
	sysfs_del("psu_cap");
	sysfs_del("intrusion0_alarm");
	sysfs_del("intrusion1_alarm");
	read_local(&s);
	CHECK_EQ_INT(s.fan, -1);
	CHECK_EQ_INT(s.psu_cap_w, -1);
	CHECK(!s.have_fault_status && !s.have_fault_log);

	/* Stale data (the module returns ENODATA; here: no reading files)
	 * still finds the device but marks the snapshot not ok. */
	char a[32];
	for (int i = 0; i < 8; i++) {
		snprintf(a, sizeof(a), "in%d_input", i);
		sysfs_del(a);
	}
	for (int i = 1; i <= 7; i++) {
		snprintf(a, sizeof(a), "curr%d_input", i);
		sysfs_del(a);
		snprintf(a, sizeof(a), "power%d_input", i);
		sysfs_del(a);
	}
	CHECK_EQ_INT(read_local(&s), 0);
	CHECK(s.ok == 0);
	CHECK(!s.have_in && !s.have_curr && !s.have_power);
}

/* ---- sensors / sensors --json output ---- */

/* Run stmt with stdout captured into buf (NUL-terminated). */
#define CAPTURE(buf, stmt) do {						\
	char cpath_[PATH_MAX];						\
	snprintf(cpath_, sizeof(cpath_), "%s/stdout", g_tmpdir);	\
	fflush(stdout);							\
	int saved_out_ = dup(1);					\
	int cfd_ = open(cpath_, O_WRONLY | O_CREAT | O_TRUNC, 0600);	\
	if (cfd_ >= 0) { dup2(cfd_, 1); close(cfd_); }			\
	stmt;								\
	fflush(stdout);							\
	if (saved_out_ >= 0) { dup2(saved_out_, 1); close(saved_out_); } \
	FILE *cf_ = fopen(cpath_, "r");					\
	size_t cn_ = cf_ ? fread((buf), 1, sizeof(buf) - 1, cf_) : 0;	\
	(buf)[cn_] = '\0';						\
	if (cf_) fclose(cf_);						\
} while (0)

/* Collect the object keys of a flat JSON document, in order, as
 * "key1,key2,...": every string directly followed by ':' is a key. */
static void json_keys(const char *j, char *out, size_t cap)
{
	size_t o = 0;

	out[0] = '\0';
	while ((j = strchr(j, '"')) != NULL) {
		const char *end = strchr(j + 1, '"');
		if (!end)
			break;
		if (end[1] == ':') {
			o += (size_t)snprintf(out + o, cap - o, "%s%.*s",
					      o ? "," : "", (int)(end - j - 1), j + 1);
			if (o >= cap)
				break;
		}
		j = end + 1;
	}
}

/* The keys of wireviewd's GET /sensors, read from build_sensors_json() in
 * wireviewd.c: every \"key\": in the function, first occurrence only (the
 * no-device reply repeats host/appVersion/devices). As of this writing:
 * host,appVersion,devices,id,name,connected,hwRev,fwVer,buildString,
 * timestamp,pinVoltage,pinCurrent,tempInC,tempOutC,ext1C,ext2C,psuCapW,fan,
 * faultStatus,faultLog,sumCurrentA,sumPowerW,energyJ. */
static int daemon_sensors_keys(char *out, size_t cap)
{
	static char src[256 * 1024];
	FILE *f = fopen(WV_SRCDIR "/wireviewd.c", "r");
	if (!f)
		return -1;
	size_t n = fread(src, 1, sizeof(src) - 1, f);
	fclose(f);
	src[n] = '\0';

	const char *p = strstr(src, "static int build_sensors_json(");
	const char *end = p ? strstr(p, "\n}\n") : NULL;
	if (!end)
		return -1;
	size_t o = 0;
	out[0] = '\0';
	while ((p = strstr(p, "\\\"")) != NULL && p < end) {
		const char *k = p + 2;
		const char *q = strstr(k, "\\\"");
		if (!q || q > end)
			break;
		size_t len = (size_t)(q - k);
		int is_key = q[2] == ':' && len > 0 && len < 32;
		for (size_t i = 0; is_key && i < len; i++)
			if (!isalnum((unsigned char)k[i]))
				is_key = 0;
		if (is_key) {
			char key[40];
			snprintf(key, sizeof(key), "%.*s", (int)len, k);
			/* skip repeats: match whole entries only */
			char pad[4096], needle[48];
			snprintf(pad, sizeof(pad), ",%s,", out);
			snprintf(needle, sizeof(needle), ",%s,", key);
			if (!strstr(pad, needle))
				o += (size_t)snprintf(out + o, cap - o, "%s%s",
						      o ? "," : "", key);
			p = q + 2;
		} else {
			p = k;
		}
	}
	return o ? 0 : -1;
}

static void test_sensors_output(void)
{
	static char out[16384];
	char keys[2048], want[2048];
	char *argv_json[] = { "wireviewctl", "sensors", "--json", NULL };
	char *argv_plain[] = { "wireviewctl", "sensors", NULL };
	char *argv_bad[] = { "wireviewctl", "sensors", "--xml", NULL };
	int rc = -1;

	/* The fake tree from test_read_local(), restored to a full module. */
	sysfs_populate();
	setenv("WIREVIEW_HWMON_PATH", g_hwmon, 1);

	CAPTURE(out, rc = cmd_sensors(2, argv_plain));
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(out, "pin1_voltage_mv: 12000\n") != NULL);
	CHECK(strstr(out, "vdd_mv: 3300\n") != NULL);
	CHECK(strstr(out, "total_current_ma: 49500\n") != NULL);
	CHECK(strstr(out, "total_power_uw: 595000000\n") != NULL);
	CHECK(strstr(out, "temp_onboard_out_mc: -40000\n") != NULL);
	CHECK(strstr(out, "temp_external1_mc") == NULL);
	CHECK(strstr(out, "fan_duty: 42\n") != NULL);
	CHECK(strstr(out, "psu_cap: 450W\n") != NULL);
	CHECK(strstr(out, "energy_uj: 123456789012\n") != NULL);
	CHECK(strstr(out, "fault_status: 513\n") != NULL);
	CHECK(strstr(out, "alarm_temp_onboard_in: 1\n") != NULL);
	CHECK(strstr(out, "alarm_total_power: 1\n") != NULL);
	CHECK(strstr(out, "alarm_pin1_current: 0\n") != NULL);

	CAPTURE(out, rc = cmd_sensors(3, argv_json));
	CHECK_EQ_INT(rc, 0);
	CHECK(strncmp(out, "{\"host\":\"", 9) == 0);
	CHECK(strstr(out, "\"appVersion\":\"wireviewctl\"") != NULL);
	CHECK(strstr(out, "\"connected\":true") != NULL);
	CHECK(strstr(out, "\"pinVoltage\":[12.000,12.100,12.200,12.300,12.400,12.500]") != NULL);
	CHECK(strstr(out, "\"pinCurrent\":[8.000,8.100,8.200,8.300,8.400,8.500]") != NULL);
	/* An absent temperature is 0.0, as the daemon sends it. */
	CHECK(strstr(out, "\"tempInC\":35.5,\"tempOutC\":-40.0,\"ext1C\":0.0,\"ext2C\":25.0") != NULL);
	CHECK(strstr(out, "\"psuCapW\":450,\"fan\":42,\"faultStatus\":513,\"faultLog\":65535") != NULL);
	/* Sums from the pins: 49.5 A and the sum of (12 + 0.1i) * (8 + 0.1i). */
	CHECK(strstr(out, "\"sumCurrentA\":49.500,\"sumPowerW\":606.550") != NULL);
	CHECK(strstr(out, "\"energyJ\":123456.789}") != NULL);
	CHECK(strcmp(out + strlen(out) - 3, "]}\n") == 0);

	/* Same keys, same order as wireviewd's GET /sensors. The extracted
	 * list is sanity-checked at both ends so a broken extractor cannot
	 * pass by returning next to nothing. */
	json_keys(out, keys, sizeof(keys));
	want[0] = '\0';
	CHECK_EQ_INT(daemon_sensors_keys(want, sizeof(want)), 0);
	CHECK(strncmp(want, "host,appVersion,devices,id,name,connected,", 42) == 0);
	CHECK(strlen(want) > 8 && strcmp(want + strlen(want) - 8, ",energyJ") == 0);
	CHECK_EQ_STR(keys, want);

	/* A module without energy1_input: energyJ is left out. */
	sysfs_del("energy1_input");
	CAPTURE(out, rc = cmd_sensors(3, argv_json));
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(out, "energyJ") == NULL);
	CHECK(strstr(out, "\"sumPowerW\":606.550}]}") != NULL);
	CAPTURE(out, rc = cmd_sensors(2, argv_plain));
	CHECK(strstr(out, "energy_uj") == NULL);

	/* No wireview hwmon device: an empty device list, exit 1. */
	sysfs_del("name");
	QUIET(CAPTURE(out, rc = cmd_sensors(3, argv_json)));
	CHECK_EQ_INT(rc, 1);
	CHECK(strstr(out, "\"appVersion\":\"wireviewctl\",\"devices\":[]}\n") != NULL);
	QUIET(CAPTURE(out, rc = cmd_sensors(2, argv_plain)));
	CHECK_EQ_INT(rc, 1);
	CHECK_EQ_STR(out, "");

	/* Unknown option. */
	QUIET(CAPTURE(out, rc = cmd_sensors(3, argv_bad)));
	CHECK_EQ_INT(rc, 1);

	unsetenv("WIREVIEW_HWMON_PATH");
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
	test_read_local();
	test_sensors_output();

	char cmd[PATH_MAX + 16];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: could not remove %s\n", g_tmpdir);

	return check_summary("test_wireviewctl");
}
