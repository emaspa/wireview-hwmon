/*
 * Unit tests for wireviewd's pure helpers.
 *
 * wireviewd.c is one file of static functions, so it is compiled into this
 * test directly with its main() renamed out of the way.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#define main wireviewd_main
#include "../wireviewd.c"
#undef main

#include <stddef.h>
#include "check.h"

/* ---- json_escape ---- */

/* Escape one byte the way json_escape documents it; returns its length. */
static size_t ref_escape(unsigned char c, char *out)
{
	if (c == '"' || c == '\\') {
		out[0] = '\\';
		out[1] = (char)c;
		return 2;
	}
	if (c < 0x20)
		return (size_t)sprintf(out, "\\u%04x", c);
	out[0] = c >= 0x80 ? '?' : (char)c;
	return 1;
}

static void test_json_escape(void)
{
	char out[256];

	json_escape("plain text 123", out, sizeof(out));
	CHECK_EQ_STR(out, "plain text 123");

	json_escape("say \"hi\"", out, sizeof(out));
	CHECK_EQ_STR(out, "say \\\"hi\\\"");

	json_escape("C:\\path\\", out, sizeof(out));
	CHECK_EQ_STR(out, "C:\\\\path\\\\");

	json_escape("a\nb\tc\x01\x1f", out, sizeof(out));
	CHECK_EQ_STR(out, "a\\u000ab\\u0009c\\u0001\\u001f");

	/* Non-ASCII bytes become '?', DEL and ordinary ASCII pass through. */
	json_escape("caf\xc3\xa9 \x80\xff\x7f", out, sizeof(out));
	CHECK_EQ_STR(out, "caf?? ??\x7f");

	json_escape("", out, sizeof(out));
	CHECK_EQ_STR(out, "");

	/* cap == 0 must not write anything. */
	memset(out, 'Z', 4);
	json_escape("abc", out, 0);
	CHECK(out[0] == 'Z');

	/* Truncation: for every cap, the output is the longest whole-escape
	 * prefix of the full escaping that fits in cap - 1 bytes. */
	const char *in = "x\"y\\z\x01w\xfe";
	char full[128];
	size_t bound[16], nb = 0, flen = 0;

	bound[nb++] = 0;
	for (const char *p = in; *p; p++) {
		flen += ref_escape((unsigned char)*p, full + flen);
		bound[nb++] = flen;
	}
	full[flen] = '\0';
	json_escape(in, out, sizeof(out));
	CHECK_EQ_STR(out, full);

	for (size_t cap = 1; cap <= flen + 2; cap++) {
		char buf[128];
		size_t want = 0;

		memset(buf, 'Z', sizeof(buf));
		json_escape(in, buf, cap);
		for (size_t i = 0; i < nb; i++)
			if (bound[i] < cap)
				want = bound[i];
		CHECK(strlen(buf) < cap);
		CHECK_EQ_INT(strlen(buf), want);
		CHECK(strncmp(buf, full, want) == 0);
		CHECK(buf[cap] == 'Z');	/* nothing written past cap */
	}
}

/* ---- base64 ---- */

static void test_b64(void)
{
	static const struct { const char *raw, *enc; } rfc4648[] = {
		{ "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
		{ "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" },
		{ "foobar", "Zm9vYmFy" },
	};
	char enc[64];
	uint8_t dec[64];

	for (size_t i = 0; i < sizeof(rfc4648) / sizeof(rfc4648[0]); i++) {
		int len = (int)strlen(rfc4648[i].raw);

		b64_encode((const uint8_t *)rfc4648[i].raw, len, enc);
		CHECK_EQ_STR(enc, rfc4648[i].enc);
		CHECK_EQ_INT(b64_decode(rfc4648[i].enc, dec, sizeof(dec)), len);
		CHECK_EQ_MEM(dec, rfc4648[i].raw, (size_t)len);
	}

	/* Round trip every length 0..96 (a v2 config is 96 bytes) with all
	 * byte values, so every padding case and the 62/63 digits show up. */
	uint8_t raw[96];
	char big[160];
	uint8_t back[128];

	for (int i = 0; i < 96; i++)
		raw[i] = (uint8_t)(i * 37 + 251);
	for (int len = 0; len <= 96; len++) {
		b64_encode(raw, len, big);
		CHECK_EQ_INT(strlen(big), (len + 2) / 3 * 4);
		CHECK_EQ_INT(b64_decode(big, back, sizeof(back)), len);
		CHECK_EQ_MEM(back, raw, (size_t)len);
	}

	/* '+' and '/' are the standard alphabet. */
	uint8_t hi[3] = { 0xfb, 0xff, 0xbf };
	b64_encode(hi, 3, enc);
	CHECK_EQ_STR(enc, "+/+/");

	/* Whitespace is skipped. */
	CHECK_EQ_INT(b64_decode("Zm9v\r\nYmFy", dec, sizeof(dec)), 6);
	CHECK_EQ_MEM(dec, "foobar", 6);

	/* Characters outside the alphabet are rejected. */
	CHECK_EQ_INT(b64_decode("Zm9v!", dec, sizeof(dec)), -1);
	CHECK_EQ_INT(b64_decode("Zm9-", dec, sizeof(dec)), -1);	/* base64url */
	CHECK_EQ_INT(b64_decode("Zm9_", dec, sizeof(dec)), -1);
	CHECK_EQ_INT(b64_decode("Zm\"9v", dec, sizeof(dec)), -1);

	/* Output capacity is enforced. */
	CHECK_EQ_INT(b64_decode("Zm9vYmFy", dec, 5), -1);
	CHECK_EQ_INT(b64_decode("Zm9vYmFy", dec, 6), 6);
}

/* ---- json_str / json_int on the POST /command schema ---- */

static void test_json_readers(void)
{
	char s[32];
	long v;

	const char *screen = "{\"op\":\"screen\",\"cmd\":227}";
	CHECK(json_str(screen, "op", s, sizeof(s)));
	CHECK_EQ_STR(s, "screen");
	CHECK(json_int(screen, "cmd", &v));
	CHECK_EQ_INT(v, 227);

	const char *clear = "{ \"op\" : \"clearFaults\", \"statusMask\": 0,\n"
			    "\t\"logMask\":\t65534 }";
	CHECK(json_str(clear, "op", s, sizeof(s)));
	CHECK_EQ_STR(s, "clearFaults");
	CHECK(json_int(clear, "statusMask", &v));
	CHECK_EQ_INT(v, 0);
	CHECK(json_int(clear, "logMask", &v));
	CHECK_EQ_INT(v, 65534);

	const char *wc = "{\"op\":\"writeConfig\",\"data\":\"AAEC/w==\"}";
	char data[64];
	uint8_t cfg[16];
	CHECK(json_str(wc, "data", data, sizeof(data)));
	CHECK_EQ_STR(data, "AAEC/w==");
	CHECK_EQ_INT(b64_decode(data, cfg, sizeof(cfg)), 4);
	CHECK(cfg[0] == 0 && cfg[1] == 1 && cfg[2] == 2 && cfg[3] == 0xff);

	/* Negative numbers parse. */
	CHECK(json_int("{\"cmd\":-5}", "cmd", &v));
	CHECK_EQ_INT(v, -5);

	/* Backslash escapes are unwrapped. */
	CHECK(json_str("{\"op\":\"a\\\"b\\\\c\"}", "op", s, sizeof(s)));
	CHECK_EQ_STR(s, "a\"b\\c");

	/* Missing keys and wrong types fail and leave *out alone. */
	v = 42;
	CHECK(!json_int(screen, "nope", &v));
	CHECK(!json_int(screen, "op", &v));	/* string, not a number */
	CHECK_EQ_INT(v, 42);
	CHECK(!json_str(screen, "cmd", s, sizeof(s)));	/* number, not a string */
	CHECK(!json_str(screen, "missing", s, sizeof(s)));

	/* Output is bounded by n. */
	CHECK(json_str("{\"op\":\"clearFaults\"}", "op", s, 6));
	CHECK_EQ_STR(s, "clear");
}

/* ---- http_header ---- */

static void test_http_header(void)
{
	const char *req =
		"POST /command HTTP/1.1\r\n"
		"Host: box:9876\r\n"
		"x-auth-ts:   1700000000\r\n"
		"X-AUTH-NONCE:\tabc123\r\n"
		"X-Auth-Sig: deadbeef\r\n"
		"Content-Length: 27\r\n"
		"\r\n"
		"{\"op\":\"screen\",\"cmd\":224}";
	char out[64];

	CHECK(http_header(req, "X-Auth-Ts", out, sizeof(out)));
	CHECK_EQ_STR(out, "1700000000");
	CHECK(http_header(req, "x-auth-nonce", out, sizeof(out)));
	CHECK_EQ_STR(out, "abc123");
	CHECK(http_header(req, "X-Auth-Sig", out, sizeof(out)));
	CHECK_EQ_STR(out, "deadbeef");
	CHECK(http_header(req, "content-length", out, sizeof(out)));
	CHECK_EQ_STR(out, "27");

	strcpy(out, "untouched");
	CHECK(!http_header(req, "X-Missing", out, sizeof(out)));
	CHECK_EQ_STR(out, "untouched");

	/* The request line is not a header; a name must start a line. */
	CHECK(!http_header(req, "POST /command HTTP/1.1", out, sizeof(out)));
	CHECK(!http_header(req, "Auth-Ts", out, sizeof(out)));

	/* The value is truncated to the buffer. */
	CHECK(http_header(req, "X-Auth-Ts", out, 5));
	CHECK_EQ_STR(out, "1700");
}

/* ---- HMAC-SHA256 (RFC 4231) and the signature helpers ---- */

static void test_hmac(void)
{
	uint8_t k1[20], k3[20], d3[50], k4[25], d4[50], k6[131];
	uint8_t mac[32];
	char hex[65];

	memset(k1, 0x0b, sizeof(k1));
	memset(k3, 0xaa, sizeof(k3));
	memset(d3, 0xdd, sizeof(d3));
	for (int i = 0; i < 25; i++)
		k4[i] = (uint8_t)(i + 1);
	memset(d4, 0xcd, sizeof(d4));
	memset(k6, 0xaa, sizeof(k6));

	static const char *const d6 =
		"Test Using Larger Than Block-Size Key - Hash Key First";
	const struct {
		const uint8_t *key; size_t klen;
		const uint8_t *msg; size_t mlen;
		const char *want;
	} tc[] = {
		{ k1, sizeof(k1), (const uint8_t *)"Hi There", 8,
		  "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7" },
		{ (const uint8_t *)"Jefe", 4,
		  (const uint8_t *)"what do ya want for nothing?", 28,
		  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843" },
		{ k3, sizeof(k3), d3, sizeof(d3),
		  "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe" },
		{ k4, sizeof(k4), d4, sizeof(d4),
		  "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b" },
		/* Case 6: key longer than the block, hashed first. */
		{ k6, sizeof(k6), (const uint8_t *)d6, strlen(d6),
		  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54" },
	};

	for (size_t i = 0; i < sizeof(tc) / sizeof(tc[0]); i++) {
		hmac_sha256(tc[i].key, tc[i].klen, tc[i].msg, tc[i].mlen, mac);
		hex_encode(mac, 32, hex);
		CHECK_EQ_STR(hex, tc[i].want);
		CHECK(ct_str_equal(hex, tc[i].want));
	}

	static const uint8_t bytes[] = { 0x00, 0x01, 0x7f, 0x80, 0xab, 0xff };
	hex_encode(bytes, sizeof(bytes), hex);
	CHECK_EQ_STR(hex, "00017f80abff");
	hex_encode(bytes, 0, hex);
	CHECK_EQ_STR(hex, "");

	CHECK(ct_str_equal("", ""));
	CHECK(ct_str_equal("abc", "abc"));
	CHECK(!ct_str_equal("abc", "abd"));
	CHECK(!ct_str_equal("abc", "ab"));
	CHECK(!ct_str_equal("ab", "abc"));
	CHECK(!ct_str_equal("", "a"));
	/* Case matters: signatures are lowercase hex. */
	CHECK(!ct_str_equal("DEADBEEF", "deadbeef"));
}

/* ---- sensor frame layout and the corrupt-frame rule ---- */

/* The rule main() applies before publishing a frame (see the comment
 * there). It is inline in the poll loop, so this mirrors it; the e2e test
 * checks the daemon itself discards such frames. */
static int frame_is_corrupt(const struct sensor_struct *ss)
{
	return ss->fan_duty > 100 || ss->_pad1 || ss->_pad2;
}

static void test_frame(void)
{
	/* Firmware SensorStruct layout (Pack=4). */
	CHECK_EQ_INT(sizeof(struct power_sensor), 12);
	CHECK_EQ_INT(sizeof(struct sensor_struct), 100);
	CHECK_EQ_INT(offsetof(struct sensor_struct, vdd), 8);
	CHECK_EQ_INT(offsetof(struct sensor_struct, fan_duty), 10);
	CHECK_EQ_INT(offsetof(struct sensor_struct, _pad1), 11);
	CHECK_EQ_INT(offsetof(struct sensor_struct, pins), 12);
	CHECK_EQ_INT(offsetof(struct sensor_struct, total_power), 84);
	CHECK_EQ_INT(offsetof(struct sensor_struct, avg_voltage), 92);
	CHECK_EQ_INT(offsetof(struct sensor_struct, hpwr_cap), 94);
	CHECK_EQ_INT(offsetof(struct sensor_struct, _pad2), 95);
	CHECK_EQ_INT(offsetof(struct sensor_struct, fault_status), 96);
	CHECK_EQ_INT(offsetof(struct sensor_struct, fault_log), 98);

	struct sensor_struct ss;
	uint8_t raw[100];

	memset(&ss, 0, sizeof(ss));
	CHECK(!frame_is_corrupt(&ss));
	ss.fan_duty = 100;
	CHECK(!frame_is_corrupt(&ss));
	ss.fan_duty = 101;
	CHECK(frame_is_corrupt(&ss));
	ss.fan_duty = 0;
	ss._pad1 = 1;
	CHECK(frame_is_corrupt(&ss));
	ss._pad1 = 0;
	ss._pad2 = 0x80;
	CHECK(frame_is_corrupt(&ss));

	/* The corrupt frame seen in the field: fan=105 pad1=122
	 * status=0x0607, as raw little-endian bytes off the wire. */
	memset(raw, 0, sizeof(raw));
	raw[10] = 105;
	raw[11] = 122;
	raw[96] = 0x07;
	raw[97] = 0x06;
	memcpy(&ss, raw, sizeof(ss));
	CHECK_EQ_INT(ss.fan_duty, 105);
	CHECK_EQ_INT(ss._pad1, 122);
	CHECK_EQ_INT(ss.fault_status, 0x0607);
	CHECK(frame_is_corrupt(&ss));
}

/* ---- write_hwmon conversion ---- */

static void test_write_hwmon(void)
{
	/* The kernel module accepts v2 (148 bytes) and may grow to v3 (156). */
	CHECK(sizeof(struct hwmon_data) == 148 || sizeof(struct hwmon_data) == 156);

	struct sensor_struct ss;
	memset(&ss, 0, sizeof(ss));
	static const int16_t mv[6] = { 12000, 12050, 11990, -5, 0, 12345 };
	static const uint32_t ma[6] = { 8000, 8100, 7900, 1000, 0, 9500 };
	for (int i = 0; i < 6; i++) {
		ss.pins[i].voltage = mv[i];
		ss.pins[i].current = ma[i];
		ss.pins[i].power = 1;	/* ignored: power is recomputed */
	}
	ss.ts[0] = 355;		/* 35.5 C */
	ss.ts[1] = -400;	/* lowest valid */
	ss.ts[2] = 2001;	/* above range: disconnected */
	ss.ts[3] = -401;	/* below range: disconnected */
	ss.vdd = 3300;
	ss.fan_duty = 42;
	ss.hpwr_cap = 2;
	ss.total_current = 34500;
	ss.avg_voltage = 12011;
	ss.fault_status = 0x0102;
	ss.fault_log = 0x8001;

	char path[] = "/tmp/wireview-test-hwmon-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	unlink(path);

	CHECK_EQ_INT(write_hwmon(fd, &ss), 0);
	/* Second frame: the extremes of the temperature range. */
	ss.ts[0] = 2000;
	ss.ts[1] = INT16_MIN;
	CHECK_EQ_INT(write_hwmon(fd, &ss), 0);

	struct hwmon_data hd[2];
	CHECK_EQ_INT(lseek(fd, 0, SEEK_END), 2 * (off_t)sizeof(struct hwmon_data));
	CHECK_EQ_INT(pread(fd, hd, sizeof(hd), 0), (ssize_t)sizeof(hd));
	close(fd);

	CHECK_EQ_INT(hd[0].magic, 0x57565032);
	CHECK_EQ_INT(hd[0].version, WIREVIEW_VERSION);

	int64_t sum = 0;
	for (int i = 0; i < 6; i++) {
		int64_t uw = (int64_t)mv[i] * ma[i];
		CHECK_EQ_INT(hd[0].voltage_mv[i], mv[i]);
		CHECK_EQ_INT(hd[0].current_ma[i], ma[i]);
		CHECK_EQ_INT(hd[0].pin_power_uw[i], uw);
		sum += uw;
	}
	CHECK_EQ_INT(hd[0].pin_power_uw[0], 96000000);	/* 12 V * 8 A = 96 W */
	CHECK_EQ_INT(hd[0].pin_power_uw[3], -5000);	/* sign preserved */
	CHECK_EQ_INT(hd[0].total_power_uw, sum);

	CHECK_EQ_INT(hd[0].temp_mc[0], 35500);
	CHECK_EQ_INT(hd[0].temp_mc[1], -40000);
	CHECK_EQ_INT(hd[0].temp_mc[2], INT32_MIN);
	CHECK_EQ_INT(hd[0].temp_mc[3], INT32_MIN);
	CHECK_EQ_INT(hd[1].temp_mc[0], 200000);
	CHECK_EQ_INT(hd[1].temp_mc[1], INT32_MIN);

	CHECK_EQ_INT(hd[0].total_current_ma, 34500);
	CHECK_EQ_INT(hd[0].avg_voltage_mv, 12011);
	CHECK_EQ_INT(hd[0].vdd_mv, 3300);
	CHECK_EQ_INT(hd[0].fan_duty, 42);
	CHECK_EQ_INT(hd[0].psu_cap, 2);
	CHECK_EQ_INT(hd[0].fault_status, 0x0102);
	CHECK_EQ_INT(hd[0].fault_log, 0x8001);

	/* A full-scale reading must not overflow the 64-bit sums. */
	for (int i = 0; i < 6; i++) {
		ss.pins[i].voltage = INT16_MAX;
		ss.pins[i].current = UINT32_MAX;
	}
	fd = open("/dev/null", O_WRONLY);
	CHECK_EQ_INT(write_hwmon(fd, &ss), 0);
	close(fd);

	/* A short write is an error. */
	fd = open("/dev/full", O_WRONLY);
	if (fd >= 0) {
		CHECK_EQ_INT(write_hwmon(fd, &ss), -1);
		close(fd);
	}
}

int main(void)
{
	test_json_escape();
	test_b64();
	test_json_readers();
	test_http_header();
	test_hmac();
	test_frame();
	test_write_hwmon();
	return check_summary("test_wireviewd");
}
