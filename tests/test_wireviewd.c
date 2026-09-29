/*
 * Unit tests for wireviewd's helpers: JSON/base64/HTTP parsing, HMAC, the
 * hwmon record (v3 with energy, v2 fallback), energy integration, the
 * GET /sensors and /metrics bodies, the config file and the listener.
 *
 * wireviewd.c is one file of static functions, so it is compiled into this
 * test directly with its main() renamed out of the way.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#define _GNU_SOURCE
#include <errno.h>
#include <unistd.h>

/* write() of exactly this many bytes fails with EINVAL, as a pre-v3
 * wireview_hwmon module rejects a v3 record; -1 = never. Every other
 * write goes through. */
static long g_write_einval_len = -1;

static ssize_t test_write(int fd, const void *buf, size_t n)
{
	if ((long)n == g_write_einval_len) {
		errno = EINVAL;
		return -1;
	}
	return write(fd, buf, n);
}

/* load_config() reads CONFIG_PATH; make it a variable the test points at
 * its own temp files (the Makefile default is under /nonexistent). */
static char test_config_path[256] = "/nonexistent/wireview-config";
#undef CONFIG_PATH
#define CONFIG_PATH test_config_path

#define write test_write
#define main wireviewd_main
#include "../wireviewd.c"
#undef main
#undef write

#include <stddef.h>
#include "check.h"

/* Run stmt with stdout sent to /dev/null (the daemon's status lines). */
#define QUIET_OUT(stmt) do {						\
	fflush(stdout);							\
	int saved_o_ = dup(1);						\
	int null_o_ = open("/dev/null", O_WRONLY);			\
	if (null_o_ >= 0) { dup2(null_o_, 1); close(null_o_); }		\
	stmt;								\
	fflush(stdout);							\
	if (saved_o_ >= 0) { dup2(saved_o_, 1); close(saved_o_); }	\
} while (0)

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

/* Keys only match at the top level of the object: never inside a string
 * value, a nested object or an array, and never on a truncated input. */
static void test_json_hostile(void)
{
	char s[32];
	long v;

	/* A value equal to the key name used to be found first. */
	v = 0;
	CHECK(json_int("{\"op\":\"cmd\",\"cmd\":5}", "cmd", &v));
	CHECK_EQ_INT(v, 5);
	CHECK(json_str("{\"deviceId\":\"op\",\"op\":\"screen\"}", "op", s, sizeof(s)));
	CHECK_EQ_STR(s, "screen");

	/* Key text inside a string value, with escaped quotes. */
	CHECK(json_str("{\"a\":\"\\\"op\\\":\\\"evil\\\"\",\"op\":\"good\"}",
		       "op", s, sizeof(s)));
	CHECK_EQ_STR(s, "good");
	CHECK(json_str("{\"a\":\"x \\\"op\\\" \",\"op\":\"good\"}", "op", s, sizeof(s)));
	CHECK_EQ_STR(s, "good");
	/* An escaped backslash right before the closing quote. */
	CHECK(json_str("{\"a\":\"\\\\\",\"op\":\"good\"}", "op", s, sizeof(s)));
	CHECK_EQ_STR(s, "good");
	/* Braces and brackets inside strings do not count. */
	CHECK(json_str("{\"a\":\"}\",\"b\":\"{[\",\"op\":\"good\"}", "op", s, sizeof(s)));
	CHECK_EQ_STR(s, "good");

	/* Nested objects and arrays are skipped by depth. */
	CHECK(json_int("{\"x\":{\"cmd\":5},\"cmd\":224}", "cmd", &v));
	CHECK_EQ_INT(v, 224);
	CHECK(json_int("{\"x\":[1,{\"cmd\":5},\"]\"],\"cmd\":225}", "cmd", &v));
	CHECK_EQ_INT(v, 225);
	CHECK(json_int("{\"x\":{\"y\":{\"z\":[[{}]]}},\"cmd\":226}", "cmd", &v));
	CHECK_EQ_INT(v, 226);
	v = 42;
	CHECK(!json_int("{\"x\":{\"cmd\":5}}", "cmd", &v));
	CHECK(!json_int("{\"x\":[\"cmd\",5]}", "cmd", &v));
	CHECK(!json_str("{\"x\":{\"op\":\"evil\"}}", "op", s, sizeof(s)));
	/* Other scalars are skipped too. */
	CHECK(json_int("{\"a\":true,\"b\":null,\"c\":-1.5e3,\"cmd\":7}", "cmd", &v));
	CHECK_EQ_INT(v, 7);

	/* Only whole keys match; the first of duplicates wins. */
	CHECK(json_str("{\"opx\":\"a\",\"xop\":\"b\",\"op\":\"c\"}", "op", s, sizeof(s)));
	CHECK_EQ_STR(s, "c");
	CHECK(!json_str("{\"o\":\"a\"}", "op", s, sizeof(s)));
	CHECK(json_str("{\"op\":\"first\",\"op\":\"second\"}", "op", s, sizeof(s)));
	CHECK_EQ_STR(s, "first");

	/* Blanks anywhere between tokens. */
	CHECK(json_int(" \r\n{ \"a\" : \"b\" ,\n\t\"cmd\"\t:\r\n 9 \n}", "cmd", &v));
	CHECK_EQ_INT(v, 9);

	/* Not an object, or not a key where one is due. */
	v = 42;
	CHECK(!json_int("\"cmd\":5", "cmd", &v));
	CHECK(!json_int("[{\"cmd\":5}]", "cmd", &v));
	CHECK(!json_int("{cmd:5}", "cmd", &v));
	CHECK(!json_int("{\"a\" 1,\"cmd\":5}", "cmd", &v));	/* no ':' */
	CHECK(!json_int("{\"a\":1 \"cmd\":5}", "cmd", &v));	/* no ',' */
	CHECK(!json_int("{\"a\":1]\"cmd\":5}", "cmd", &v));
	CHECK(!json_int("", "cmd", &v));
	CHECK(!json_int("{}", "cmd", &v));

	/* Truncated input. */
	CHECK(!json_int("{", "cmd", &v));
	CHECK(!json_int("{\"cm", "cmd", &v));
	CHECK(!json_int("{\"cmd\"", "cmd", &v));
	CHECK(!json_int("{\"cmd\":", "cmd", &v));
	CHECK(!json_int("{\"cmd\":22", "cmd", &v));	/* could be 227 */
	CHECK(!json_int("{\"a\":\"xx", "cmd", &v));
	CHECK(!json_int("{\"a\":\"x\\", "cmd", &v));	/* ends mid-escape */
	CHECK(!json_int("{\"a\":{\"b\":1", "cmd", &v));
	CHECK(!json_int("{\"a\":[1,2", "cmd", &v));
	CHECK(!json_str("{\"op\":\"scr", "op", s, sizeof(s)));
	CHECK(!json_str("{\"op\":\"scr\\\"", "op", s, sizeof(s)));
	CHECK_EQ_INT(v, 42);
	/* A complete member before the cut is still readable. */
	CHECK(json_int("{\"cmd\":22,\"op\":\"scr", "cmd", &v));
	CHECK_EQ_INT(v, 22);

	/* Integers must be whole and end the member. */
	v = 42;
	CHECK(!json_int("{\"cmd\":5.5}", "cmd", &v));
	CHECK(!json_int("{\"cmd\":5x}", "cmd", &v));
	CHECK(!json_int("{\"cmd\":\"5\"}", "cmd", &v));
	CHECK(!json_int("{\"cmd\":99999999999999999999999}", "cmd", &v));
	CHECK(!json_int("{\"cmd\":{}}", "cmd", &v));
	CHECK_EQ_INT(v, 42);
	CHECK(json_int("{\"cmd\":5 }", "cmd", &v));
	CHECK_EQ_INT(v, 5);

	/* A string value must be a string; n == 0 writes nothing. */
	CHECK(!json_str("{\"op\":{\"a\":\"b\"}}", "op", s, sizeof(s)));
	CHECK(!json_str("{\"op\":[\"a\"]}", "op", s, sizeof(s)));
	memset(s, 'Z', 4);
	CHECK(!json_str("{\"op\":\"a\"}", "op", s, 0));
	CHECK(s[0] == 'Z');
}

/* ---- handle_post_command, end to end in one process ---- */

/* Sign body as a client would, call handle_post_command() with the serial
 * relay going into a pipe, and return the HTTP status. serial[] receives
 * what reached the "device" (*serial_len bytes). */
static int post_command(const char *body, uint8_t *serial, size_t serial_cap,
			size_t *serial_len)
{
	static int nonce_seq;
	int sp[2], pp[2];
	char ts[24], nonce[40], msg[HTTP_MAX_BODY + 128], sig[65];
	static char req[HTTP_MAX_HEADER + HTTP_MAX_BODY];
	uint8_t mac[32];
	int status = -1;

	*serial_len = 0;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0 || pipe(pp) < 0)
		return -1;
	fcntl(pp[0], F_SETFL, O_NONBLOCK);
	fcntl(sp[1], F_SETFL, O_NONBLOCK);

	snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));
	snprintf(nonce, sizeof(nonce), "test-nonce-%d", ++nonce_seq);
	int mlen = snprintf(msg, sizeof(msg), "%s\n%s\n%s", ts, nonce, body);
	hmac_sha256((const uint8_t *)g_secret, strlen(g_secret),
		    (const uint8_t *)msg, (size_t)mlen, mac);
	hex_encode(mac, 32, sig);
	snprintf(req, sizeof(req),
		 "POST /command HTTP/1.1\r\nX-Auth-Ts: %s\r\nX-Auth-Nonce: %s\r\n"
		 "X-Auth-Sig: %s\r\nContent-Length: %zu\r\n\r\n%s",
		 ts, nonce, sig, strlen(body), body);

	g_serial_fd = pp[1];
	handle_post_command(sp[0], req, strstr(req, "\r\n\r\n") + 4, "test");
	g_serial_fd = -1;

	char resp[512];
	ssize_t n = read(sp[1], resp, sizeof(resp) - 1);
	if (n > 0) {
		resp[n] = '\0';
		sscanf(resp, "HTTP/1.1 %d", &status);
	}
	n = read(pp[0], serial, serial_cap);
	if (n > 0)
		*serial_len = (size_t)n;
	close(sp[0]);
	close(sp[1]);
	close(pp[0]);
	close(pp[1]);
	return status;
}

static void test_post_command(void)
{
	uint8_t ser[512];
	size_t n;

	snprintf(g_secret, sizeof(g_secret), "unit-test-secret");
	g_suspend_until.tv_sec = 0;

	/* The requests the GUI (WireViewCommand.ToJson) and wireviewctl
	 * send, and the serial bytes each must produce. */
	CHECK_EQ_INT(post_command("{\"deviceId\":\"A1A2\",\"op\":\"screen\",\"cmd\":227}",
				  ser, sizeof(ser), &n), 200);
	CHECK_EQ_INT(n, 2);
	CHECK_EQ_MEM(ser, "\x0c\xe3", 2);
	CHECK_EQ_INT(post_command("{\"op\":\"nvm\",\"cmd\":2}", ser, sizeof(ser), &n), 200);
	CHECK_EQ_INT(n, 6);
	CHECK_EQ_MEM(ser, "\xf2\x55\xaa\x55\xaa\x02", 6);
	CHECK_EQ_INT(post_command("{\"deviceId\":\"A1A2\",\"op\":\"clearFaults\","
				  "\"statusMask\":65534,\"logMask\":0}",
				  ser, sizeof(ser), &n), 200);
	CHECK_EQ_INT(n, 5);
	CHECK_EQ_MEM(ser, "\x0e\xfe\xff\x00\x00", 5);
	/* Masks default to 0 (clear all) when omitted. */
	CHECK_EQ_INT(post_command("{\"op\":\"clearFaults\"}", ser, sizeof(ser), &n), 200);
	CHECK_EQ_MEM(ser, "\x0e\x00\x00\x00\x00", 5);

	/* writeConfig: a 96-byte config goes out as offset frames 0 and 62. */
	uint8_t cfg[96];
	char b64[200], body[400];
	for (int i = 0; i < 96; i++)
		cfg[i] = (uint8_t)(i * 7 + 3);
	b64_encode(cfg, 96, b64);
	snprintf(body, sizeof(body),
		 "{\"deviceId\":\"A1A2\",\"op\":\"writeConfig\",\"version\":2,\"data\":\"%s\"}", b64);
	CHECK_EQ_INT(post_command(body, ser, sizeof(ser), &n), 200);
	CHECK_EQ_INT(n, 2 + 62 + 2 + 34);
	CHECK(ser[0] == 0x06 && ser[1] == 0);
	CHECK_EQ_MEM(ser + 2, cfg, 62);
	CHECK(ser[64] == 0x06 && ser[65] == 62);
	CHECK_EQ_MEM(ser + 66, cfg + 62, 34);

	/* Key names inside values or nested members no longer derail the
	 * lookup: each of these used to fail or pick the wrong value. */
	CHECK_EQ_INT(post_command("{\"deviceId\":\"op\",\"op\":\"screen\",\"cmd\":224}",
				  ser, sizeof(ser), &n), 200);
	CHECK_EQ_MEM(ser, "\x0c\xe0", 2);
	CHECK_EQ_INT(post_command("{\"op\":\"screen\",\"x\":{\"cmd\":5},\"cmd\":225}",
				  ser, sizeof(ser), &n), 200);
	CHECK_EQ_MEM(ser, "\x0c\xe1", 2);
	CHECK_EQ_INT(post_command("{\"deviceId\":\"\\\"op\\\":\\\"nvm\\\"\",\"op\":\"screen\",\"cmd\":226}",
				  ser, sizeof(ser), &n), 200);
	CHECK_EQ_MEM(ser, "\x0c\xe2", 2);

	/* Bad requests reach nothing. */
	QUIET(CHECK_EQ_INT(post_command("{\"x\":{\"op\":\"screen\"},\"cmd\":1}",
					ser, sizeof(ser), &n), 400));
	CHECK_EQ_INT(n, 0);
	CHECK_EQ_INT(post_command("{\"op\":\"bogus\"}", ser, sizeof(ser), &n), 400);
	CHECK_EQ_INT(n, 0);
	CHECK_EQ_INT(post_command("{\"op\":\"screen\",\"cmd\":22", ser, sizeof(ser), &n), 500);
	CHECK_EQ_INT(n, 0);
	CHECK_EQ_INT(post_command("{\"op\":\"screen\",\"cmd\":\"227\"}", ser, sizeof(ser), &n), 500);
	CHECK_EQ_INT(n, 0);
	CHECK_EQ_INT(post_command("{\"op\":\"writeConfig\",\"data\":\"AAEC", ser, sizeof(ser), &n), 500);
	CHECK_EQ_INT(n, 0);

	g_secret[0] = '\0';
	memset(g_nonces, 0, sizeof(g_nonces));
	g_nonce_idx = 0;
}

/* ---- command-socket clients: idle timeout, eviction, handover ---- */

static void test_client_idle(void)
{
	struct client cl[MAX_CLIENTS];
	const struct timespec now = { .tv_sec = 1000, .tv_nsec = 500000000 };

	CHECK_EQ_INT(MAX_CLIENTS, 8);
	CHECK_EQ_INT(CLIENT_IDLE_S, 60);

	/* Client i last active 10 * i s ago. */
	memset(cl, 0, sizeof(cl));
	for (int i = 0; i < MAX_CLIENTS; i++) {
		cl[i].fd = -1;
		cl[i].id = 100 + (unsigned long)i;
		cl[i].last_active = now;
		cl[i].last_active.tv_sec -= 10 * i;
	}
	g_suspend_owner = 0;
	memset(&g_suspend_until, 0, sizeof(g_suspend_until));

	/* Closed CLIENT_IDLE_S after the last activity, to the ms. */
	CHECK_EQ_INT(client_idle_left(&cl[0], &now), 60000);
	CHECK_EQ_INT(client_idle_left(&cl[5], &now), 10000);
	CHECK_EQ_INT(client_idle_left(&cl[6], &now), 0);	/* due now */
	CHECK_EQ_INT(client_idle_left(&cl[7], &now), -10000);
	cl[0].last_active.tv_nsec = 0;
	CHECK_EQ_INT(client_idle_left(&cl[0], &now), 59500);
	cl[0].last_active.tv_nsec = now.tv_nsec;

	/* A request in flight is the request deadline's business. */
	cl[7].have = 1;
	CHECK_EQ_INT(client_idle_left(&cl[7], &now), LONG_MAX);
	cl[7].have = 0;

	/* The longest idle client makes room, never one mid-request. */
	CHECK_EQ_INT(client_evict_pick(cl, MAX_CLIENTS, &now), 7);
	cl[7].have = 3;
	CHECK_EQ_INT(client_evict_pick(cl, MAX_CLIENTS, &now), 6);
	cl[7].have = 0;
	/* Sub-second order counts. */
	cl[3].last_active = cl[7].last_active;
	cl[3].last_active.tv_nsec -= 1;
	CHECK_EQ_INT(client_evict_pick(cl, MAX_CLIENTS, &now), 3);
	cl[3].last_active = now;
	cl[3].last_active.tv_sec -= 30;

	/* The client holding a serial handover is kept until it ends, even
	 * when idle past the timeout, and is never evicted. */
	g_suspend_owner = cl[7].id;
	g_suspend_until = now;
	g_suspend_until.tv_sec += 30;
	CHECK(client_holds_handover(&cl[7], &now));
	CHECK(!client_holds_handover(&cl[6], &now));
	CHECK_EQ_INT(client_idle_left(&cl[7], &now), 30000);
	CHECK_EQ_INT(client_evict_pick(cl, MAX_CLIENTS, &now), 6);
	/* Other clients' timers are unaffected. */
	CHECK_EQ_INT(client_idle_left(&cl[6], &now), 0);
	/* An owner active recently keeps the later of the two deadlines. */
	g_suspend_owner = cl[1].id;
	CHECK_EQ_INT(client_idle_left(&cl[1], &now), 50000);
	/* Once the handover is over (expired, resumed, or never set), the
	 * owner is an ordinary client again. */
	g_suspend_owner = cl[7].id;
	g_suspend_until = now;
	g_suspend_until.tv_sec -= 1;
	CHECK(!client_holds_handover(&cl[7], &now));
	CHECK_EQ_INT(client_idle_left(&cl[7], &now), -10000);
	CHECK_EQ_INT(client_evict_pick(cl, MAX_CLIENTS, &now), 7);
	memset(&g_suspend_until, 0, sizeof(g_suspend_until));
	CHECK(!client_holds_handover(&cl[7], &now));

	/* Nobody to evict: everyone mid-request, or no clients at all. */
	for (int i = 0; i < MAX_CLIENTS; i++)
		cl[i].have = 1;
	CHECK_EQ_INT(client_evict_pick(cl, MAX_CLIENTS, &now), -1);
	CHECK_EQ_INT(client_evict_pick(cl, 0, &now), -1);

	/* client_remove() closes the fd and moves the last client in. */
	int sp[2];
	CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);
	int n = 3;
	cl[0].fd = sp[0];
	cl[2].fd = -7;
	client_remove(cl, &n, 0);
	CHECK_EQ_INT(n, 2);
	CHECK_EQ_INT(cl[0].fd, -7);
	CHECK_EQ_INT(cl[2].fd, -1);
	char c;
	CHECK_EQ_INT(read(sp[1], &c, 1), 0);	/* peer sees EOF */
	close(sp[1]);
	g_suspend_owner = 0;
}

/* Activity and handover ownership as a real connection sets them. */
static void test_client_activity(void)
{
	int sp[2], pp[2];
	struct client c;
	uint8_t resp[16];
	struct timespec t0;

	CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);
	CHECK_EQ_INT(pipe(pp), 0);
	fcntl(sp[0], F_SETFL, O_NONBLOCK);
	unsigned long seq = g_client_seq;
	client_init(&c, sp[0]);
	CHECK(c.id == seq + 1 && c.id != 0);
	CHECK_EQ_INT(c.uid, getuid());	/* SO_PEERCRED: ourselves */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(client_idle_left(&c, &t0) > CLIENT_IDLE_S * 1000L - 1000);

	/* A partial request is not activity; a complete one is. */
	c.last_active.tv_sec -= 50;
	CHECK_EQ_INT(write(sp[1], "\x01", 1), 1);
	CHECK_EQ_INT(client_read(&c, -1), 0);
	CHECK_EQ_INT(c.have, 1);
	CHECK_EQ_INT(client_idle_left(&c, &t0), LONG_MAX);
	CHECK_EQ_INT(write(sp[1], "\x00\x00", 2), 2);
	CHECK_EQ_INT(client_read(&c, -1), 0);
	CHECK_EQ_INT(c.have, 0);
	CHECK_EQ_INT(read(sp[1], resp, sizeof(resp)), 3);
	CHECK_EQ_INT(resp[0], RESP_NOT_CONNECTED);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(client_idle_left(&c, &t0) > CLIENT_IDLE_S * 1000L - 1000);

	/* SUSPEND_SERIAL records the requesting connection; RESUME and a
	 * denied request from another peer behave as expected. */
	memset(&g_suspend_until, 0, sizeof(g_suspend_until));
	g_suspend_owner = 0;
	c.privileged = 1;
	memcpy(c.req, "\x09\x02\x00\x05\x00", 5);	/* 5 s */
	handle_client_request(&c, pp[1]);
	CHECK_EQ_INT(read(sp[1], resp, sizeof(resp)), 3);
	CHECK_EQ_INT(resp[0], RESP_OK);
	CHECK(g_suspend_owner == c.id);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(client_holds_handover(&c, &t0));
	c.last_active.tv_sec -= 3600;		/* long idle, still kept */
	long left = client_idle_left(&c, &t0);
	CHECK(left > 4000 && left <= 5000);

	struct client other = c;
	other.id = c.id + 1000;
	other.privileged = 0;
	memcpy(other.req, "\x0a\x00\x00", 3);	/* RESUME, denied */
	handle_client_request(&other, pp[1]);
	CHECK_EQ_INT(read(sp[1], resp, sizeof(resp)), 3);
	CHECK_EQ_INT(resp[0], RESP_DENIED);
	CHECK(g_suspend_owner == c.id);

	memcpy(c.req, "\x0a\x00\x00", 3);	/* RESUME */
	handle_client_request(&c, pp[1]);
	CHECK_EQ_INT(read(sp[1], resp, sizeof(resp)), 3);
	CHECK_EQ_INT(resp[0], RESP_OK);
	CHECK(g_suspend_owner == 0);
	CHECK(!client_holds_handover(&c, &t0));
	CHECK(client_idle_left(&c, &t0) < 0);

	close(sp[0]);
	close(sp[1]);
	close(pp[0]);
	close(pp[1]);
	memset(&g_suspend_until, 0, sizeof(g_suspend_until));
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

/* frame_is_sane() is the check main() applies before publishing a frame;
 * the e2e test checks the daemon discards a corrupt one end to end. */
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
	CHECK(frame_is_sane(&ss));
	ss.fan_duty = 100;
	CHECK(frame_is_sane(&ss));
	ss.fan_duty = 101;
	CHECK(!frame_is_sane(&ss));
	ss.fan_duty = 0;
	ss._pad1 = 1;
	CHECK(!frame_is_sane(&ss));
	ss._pad1 = 0;
	ss._pad2 = 0x80;
	CHECK(!frame_is_sane(&ss));

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
	CHECK(!frame_is_sane(&ss));
}

/* ---- write_hwmon conversion ---- */

static void test_write_hwmon(void)
{
	/* v3 record: the v2 layout (148 bytes) plus int64 energy_uj. */
	CHECK_EQ_INT(sizeof(struct hwmon_data), 156);
	CHECK_EQ_INT(WIREVIEW_VERSION, 3);
	CHECK_EQ_INT(HWMON_V2_SIZE, 148);
	CHECK_EQ_INT(offsetof(struct hwmon_data, voltage_mv), 8);
	CHECK_EQ_INT(offsetof(struct hwmon_data, current_ma), 32);
	CHECK_EQ_INT(offsetof(struct hwmon_data, total_power_uw), 56);
	CHECK_EQ_INT(offsetof(struct hwmon_data, temp_mc), 64);
	CHECK_EQ_INT(offsetof(struct hwmon_data, pin_power_uw), 80);
	CHECK_EQ_INT(offsetof(struct hwmon_data, total_current_ma), 128);
	CHECK_EQ_INT(offsetof(struct hwmon_data, vdd_mv), 136);
	CHECK_EQ_INT(offsetof(struct hwmon_data, fan_duty), 140);
	CHECK_EQ_INT(offsetof(struct hwmon_data, fault_log), 144);
	CHECK_EQ_INT(offsetof(struct hwmon_data, energy_uj), 148);

	g_hwmon_v2 = 0;
	g_energy_uj = 0x0102030405060708LL;

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
	g_energy_uj = INT64_MAX;
	CHECK_EQ_INT(write_hwmon(fd, &ss), 0);

	/* One write() per frame, the whole 156-byte record; energy_uj is
	 * little-endian at offset 148. */
	uint8_t rec[156];
	CHECK_EQ_INT(pread(fd, rec, sizeof(rec), 0), 156);
	static const uint8_t energy_le[8] = { 8, 7, 6, 5, 4, 3, 2, 1 };
	CHECK_EQ_MEM(rec + 148, energy_le, 8);
	CHECK_EQ_INT(rec[4], 3);	/* version, little-endian u32 */
	CHECK(rec[5] == 0 && rec[6] == 0 && rec[7] == 0);

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
	CHECK_EQ_INT(hd[0]._pad, 0);
	CHECK_EQ_INT(hd[0].energy_uj, 0x0102030405060708LL);
	CHECK_EQ_INT(hd[1].version, 3);
	CHECK_EQ_INT(hd[1].energy_uj, INT64_MAX);

	/* A full-scale reading must not overflow the 64-bit sums. */
	for (int i = 0; i < 6; i++) {
		ss.pins[i].voltage = INT16_MAX;
		ss.pins[i].current = UINT32_MAX;
	}
	fd = open("/dev/null", O_WRONLY);
	CHECK_EQ_INT(write_hwmon(fd, &ss), 0);
	close(fd);

	/* A short write is an error, and not a reason to fall back to v2. */
	fd = open("/dev/full", O_WRONLY);
	if (fd >= 0) {
		CHECK_EQ_INT(write_hwmon(fd, &ss), -1);
		CHECK_EQ_INT(g_hwmon_v2, 0);
		close(fd);
	}

	/* A pre-v3 module rejects the 156-byte record with EINVAL: the same
	 * frame goes out as a 148-byte v2 record at once, and so do the
	 * following ones without trying v3 again. */
	char path2[] = "/tmp/wireview-test-hwmon-XXXXXX";
	fd = mkstemp(path2);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	unlink(path2);
	ss.pins[0].voltage = 12000;
	ss.pins[0].current = 8000;
	g_energy_uj = 42;
	g_write_einval_len = (long)sizeof(struct hwmon_data);
	QUIET_OUT(CHECK_EQ_INT(write_hwmon(fd, &ss), 0));
	CHECK_EQ_INT(g_hwmon_v2, 1);
	g_write_einval_len = -1;
	CHECK_EQ_INT(write_hwmon(fd, &ss), 0);	/* v3 would fit now; stays v2 */
	CHECK_EQ_INT(lseek(fd, 0, SEEK_END), 2 * 148);

	uint8_t v2[2][148];
	CHECK_EQ_INT(pread(fd, v2, sizeof(v2), 0), (ssize_t)sizeof(v2));
	for (int k = 0; k < 2; k++) {
		struct hwmon_data h;
		memset(&h, 0xEE, sizeof(h));
		memcpy(&h, v2[k], 148);
		CHECK_EQ_INT(h.magic, WIREVIEW_MAGIC);
		CHECK_EQ_INT(h.version, 2);
		CHECK_EQ_INT(h.voltage_mv[0], 12000);
		CHECK_EQ_INT(h.current_ma[0], 8000);
		CHECK_EQ_INT(h.vdd_mv, 3300);
		CHECK_EQ_INT(h.fan_duty, 42);
	}
	/* A v2-only module that rejects the v2 record too: an error. */
	g_write_einval_len = 148;
	CHECK_EQ_INT(write_hwmon(fd, &ss), -1);
	g_write_einval_len = -1;
	close(fd);
	g_hwmon_v2 = 0;
	g_energy_uj = 0;
}

/* ---- energy integration ---- */

static void energy_reset(void)
{
	g_energy_uj = 0;
	g_energy_frac = 0;
	g_energy_ts_valid = 0;
}

static void test_energy(void)
{
	energy_reset();
	/* 1 W for 1.5 s = 1.5 J. */
	energy_add(1000000, 1500000000LL);
	CHECK_EQ_INT(g_energy_uj, 1500000);

	/* Sub-microjoule steps carry over exactly: 3 uW * 0.4 s = 1.2 uJ. */
	energy_reset();
	energy_add(3, 400000000LL);
	CHECK_EQ_INT(g_energy_uj, 1);
	energy_add(3, 400000000LL);
	CHECK_EQ_INT(g_energy_uj, 2);
	energy_add(3, 400000000LL);
	CHECK_EQ_INT(g_energy_uj, 3);	/* 3.6 */
	energy_add(3, 400000000LL);
	CHECK_EQ_INT(g_energy_uj, 4);	/* 4.8 */
	energy_add(1, 200000000LL);
	CHECK_EQ_INT(g_energy_uj, 5);	/* 5.0: the remainders add up */
	CHECK_EQ_INT(g_energy_frac, 0);

	/* Both factors past 1e9: 2.5 kW for 2.5 s = 6250 J. */
	energy_reset();
	energy_add(2500000000LL, 2500000000LL);
	CHECK_EQ_INT(g_energy_uj, 6250000000LL);

	/* Zero, negative power and non-positive time add nothing. */
	energy_reset();
	energy_add(0, 1000000000LL);
	energy_add(-5000000, 1000000000LL);
	energy_add(1000000, 0);
	energy_add(1000000, -1);
	CHECK_EQ_INT(g_energy_uj, 0);

	/* Saturates at INT64_MAX instead of wrapping. */
	g_energy_uj = INT64_MAX - 10;
	energy_add(1000000, 1000000000LL);
	CHECK_EQ_INT(g_energy_uj, INT64_MAX);
	energy_add(1000000, 1000000000LL);
	CHECK_EQ_INT(g_energy_uj, INT64_MAX);
	energy_reset();
	energy_add(INT64_MAX, 3000000000LL);
	CHECK_EQ_INT(g_energy_uj, INT64_MAX);

	/* energy_accumulate: the first frame only starts the clock; a frame
	 * within max_gap integrates its power over the elapsed time; one
	 * after a longer gap only restarts the clock. */
	struct sensor_struct ss;
	memset(&ss, 0, sizeof(ss));
	for (int i = 0; i < 6; i++) {
		ss.pins[i].voltage = 12000;
		ss.pins[i].current = 8000;	/* 6 x 96 W = 576 W */
	}
	CHECK_EQ_INT(frame_power_uw(&ss), 576000000LL);
	energy_reset();
	energy_accumulate(&ss, 5000);
	CHECK_EQ_INT(g_energy_uj, 0);
	CHECK(g_energy_ts_valid);
	g_energy_ts.tv_sec -= 1;		/* pretend 1 s passed */
	energy_accumulate(&ss, 5000);
	/* 576 J, plus the few microseconds the test itself took. */
	CHECK(g_energy_uj >= 576000000LL && g_energy_uj < 577000000LL);
	int64_t before = g_energy_uj;
	g_energy_ts.tv_sec -= 6;		/* a 6 s gap */
	energy_accumulate(&ss, 5000);
	CHECK_EQ_INT(g_energy_uj, before);
	energy_reset();
}

/* ---- GET /sensors body ---- */

static void test_sensors_json(void)
{
	static char out[4096];
	int n;

	/* No frame yet: an empty device list. */
	g_have_last = 0;
	dev_info.valid = 0;
	n = build_sensors_json(out, sizeof(out));
	CHECK(n > 0 && (size_t)n < sizeof(out));
	CHECK(strstr(out, "\"appVersion\":\"wireviewd\",\"devices\":[]}") != NULL);

	memset(&g_last, 0, sizeof(g_last));
	for (int i = 0; i < 6; i++) {
		g_last.pins[i].voltage = 12000;
		g_last.pins[i].current = 8000;
	}
	g_last.ts[0] = 355;
	g_last.ts[1] = -400;
	g_last.ts[2] = 2001;
	g_last.ts[3] = -401;
	g_last.fan_duty = 42;
	g_last.hpwr_cap = 1;
	g_last.fault_status = 3;
	g_last.fault_log = 256;
	memset(&dev_info, 0, sizeof(dev_info));
	dev_info.fw_version = 7;
	for (int i = 0; i < 12; i++)
		dev_info.uid[i] = (uint8_t)(0xA1 + i);
	snprintf(dev_info.build_string, sizeof(dev_info.build_string), "say \"hi\"");
	dev_info.valid = 1;
	g_have_last = 1;
	g_energy_uj = 12345678901LL;

	n = build_sensors_json(out, sizeof(out));
	CHECK(n > 0 && (size_t)n < sizeof(out));
	CHECK(strstr(out, "\"id\":\"A1A2A3A4A5A6A7A8A9AAABAC\"") != NULL);
	CHECK(strstr(out, "\"fwVer\":\"7\",\"buildString\":\"say \\\"hi\\\"\"") != NULL);
	CHECK(strstr(out, "\"tempInC\":35.5,\"tempOutC\":-40.0,\"ext1C\":0.0,\"ext2C\":0.0") != NULL);
	CHECK(strstr(out, "\"psuCapW\":450,\"fan\":42,\"faultStatus\":3,\"faultLog\":256") != NULL);
	CHECK(strstr(out, "\"sumCurrentA\":48.000,\"sumPowerW\":576.000") != NULL);
	/* Energy in joules, the last key. */
	CHECK(strstr(out, "\"energyJ\":12345.679}]}") != NULL);

	g_have_last = 0;
	dev_info.valid = 0;
	g_energy_uj = 0;
}

/* ---- GET /metrics ---- */

static void test_prom_escape(void)
{
	char out[64];

	prom_escape("plain-1.0 build", out, sizeof(out));
	CHECK_EQ_STR(out, "plain-1.0 build");
	/* Backslash, quote and newline are escaped; other control bytes and
	 * non-ASCII become '?'. */
	prom_escape("a\\b\"c\nd\te\r\x01\x7f\xc3\xa9", out, sizeof(out));
	CHECK_EQ_STR(out, "a\\\\b\\\"c\\nd?e??\x7f??");
	prom_escape("", out, sizeof(out));
	CHECK_EQ_STR(out, "");

	memset(out, 'Z', 4);
	prom_escape("abc", out, 0);
	CHECK(out[0] == 'Z');

	/* Truncation never splits an escape: "ab\"" needs 5 bytes. */
	prom_escape("ab\"", out, 4);
	CHECK_EQ_STR(out, "ab");
	prom_escape("ab\"", out, 5);
	CHECK_EQ_STR(out, "ab\\\"");
	prom_escape("\n\n", out, 4);
	CHECK_EQ_STR(out, "\\n");
	prom_escape("xyz", out, 1);
	CHECK_EQ_STR(out, "");
}

static void test_metrics(void)
{
	static char body[16384];
	struct outbuf ob;

	/* Never seen a device: an unlabelled wireview_up 0 and nothing else. */
	g_dev_uid[0] = '\0';
	g_serial_fd = -1;
	g_have_last = 0;
	dev_info.valid = 0;
	ob = (struct outbuf){ .p = body, .cap = sizeof(body) };
	CHECK_EQ_INT(build_metrics(&ob), 0);
	CHECK_EQ_STR(body, "# HELP wireview_up 1 if the device is connected and reporting, else 0.\n"
		     "# TYPE wireview_up gauge\nwireview_up 0\n");

	/* Device gone after one was seen: labelled with its UID. */
	snprintf(g_dev_uid, sizeof(g_dev_uid), "A1A2A3A4A5A6A7A8A9AAABAC");
	ob = (struct outbuf){ .p = body, .cap = sizeof(body) };
	CHECK_EQ_INT(build_metrics(&ob), 0);
	CHECK(strstr(body, "\nwireview_up{device=\"A1A2A3A4A5A6A7A8A9AAABAC\"} 0\n") != NULL);
	CHECK(strstr(body, "wireview_pin") == NULL);

	/* Connected. */
	memset(&g_last, 0, sizeof(g_last));
	for (int i = 0; i < 6; i++) {
		g_last.pins[i].voltage = 12000;
		g_last.pins[i].current = 8000;
	}
	g_last.pins[5].voltage = -5;
	g_last.ts[0] = 355;
	g_last.ts[1] = 2001;	/* disconnected: omitted */
	g_last.ts[2] = -400;
	g_last.ts[3] = -401;	/* disconnected: omitted */
	g_last.vdd = 3300;
	g_last.fan_duty = 42;
	g_last.hpwr_cap = 2;
	g_last.total_current = 48000;
	g_last.avg_voltage = 12001;
	g_last.fault_status = 0x21;	/* chip_over_temp, current_imbalance */
	g_last.fault_log = 0x8001;
	memset(&dev_info, 0, sizeof(dev_info));
	dev_info.fw_version = 7;
	dev_info.vendor_id = 0xEF;
	dev_info.product_id = 0x06;
	snprintf(dev_info.build_string, sizeof(dev_info.build_string),
		 "b\"1\\2\n3\xff");
	dev_info.valid = 1;
	g_have_last = 1;
	g_serial_fd = 1000;	/* only compared with -1 here */
	g_energy_uj = 1234567891LL;

	ob = (struct outbuf){ .p = body, .cap = sizeof(body) };
	CHECK_EQ_INT(build_metrics(&ob), 0);
	CHECK_EQ_INT(strlen(body), ob.len);
	const char *d = "{device=\"A1A2A3A4A5A6A7A8A9AAABAC\"";
	char want[256];
#define HAS(...) do {							\
		snprintf(want, sizeof(want), __VA_ARGS__);		\
		CHECK(strstr(body, want) != NULL);			\
		if (!strstr(body, want))				\
			fprintf(stderr, "  missing: %s", want);		\
	} while (0)
	HAS("\nwireview_up%s} 1\n", d);
	HAS("\nwireview_pin_voltage_volts%s,pin=\"1\"} 12.000\n", d);
	HAS("\nwireview_pin_voltage_volts%s,pin=\"6\"} -0.005\n", d);
	HAS("\nwireview_pin_current_amps%s,pin=\"6\"} 8.000\n", d);
	HAS("\nwireview_pin_power_watts%s,pin=\"1\"} 96.000\n", d);
	HAS("\nwireview_pin_power_watts%s,pin=\"6\"} -0.040\n", d);
	HAS("\nwireview_power_watts%s} 479.960\n", d);
	HAS("\nwireview_current_amps%s} 48.000\n", d);
	HAS("\nwireview_voltage_average_volts%s} 12.001\n", d);
	HAS("\nwireview_vdd_volts%s} 3.300\n", d);
	HAS("\nwireview_temperature_celsius%s,sensor=\"onboard_in\"} 35.5\n", d);
	HAS("\nwireview_temperature_celsius%s,sensor=\"external_1\"} -40.0\n", d);
	CHECK(strstr(body, "sensor=\"onboard_out\"") == NULL);
	CHECK(strstr(body, "sensor=\"external_2\"") == NULL);
	HAS("\nwireview_fan_duty_ratio%s} 0.42\n", d);
	HAS("\nwireview_psu_cap_watts%s} 300\n", d);
	HAS("\nwireview_fault_status%s} 33\n", d);
	HAS("\nwireview_fault_log%s} 32769\n", d);
	HAS("\nwireview_fault_active%s,fault=\"chip_over_temp\"} 1\n", d);
	HAS("\nwireview_fault_active%s,fault=\"over_current\"} 0\n", d);
	HAS("\nwireview_fault_active%s,fault=\"current_imbalance\"} 1\n", d);
	HAS("# TYPE wireview_energy_joules_total counter\n"
	    "wireview_energy_joules_total%s} 1234.567891\n", d);
	/* The build string is escaped as a label value. */
	HAS("\nwireview_firmware_info%s,version=\"7\",build=\"b\\\"1\\\\2\\n3?\","
	    "product=\"EF06\",edition=\"WireView Pro II Noctua Edition\"} 1\n", d);
#undef HAS
	/* Every sample line belongs to a family declared just before it. */
	CHECK(strncmp(body, "# HELP wireview_up ", 19) == 0);
	CHECK(body[strlen(body) - 1] == '\n');

	/* A body that does not fit is refused, not torn. */
	static char small[512];
	ob = (struct outbuf){ .p = small, .cap = sizeof(small) };
	CHECK_EQ_INT(build_metrics(&ob), -1);
	CHECK(ob.overflow);
	CHECK(ob.len < sizeof(small));

	g_serial_fd = -1;
	g_have_last = 0;
	dev_info.valid = 0;
	g_dev_uid[0] = '\0';
	g_energy_uj = 0;
}

/* ---- config file and the listener address ---- */

static void write_config(const char *text)
{
	FILE *f = fopen(test_config_path, "w");
	if (!f) {
		perror(test_config_path);
		return;
	}
	fputs(text, f);
	fclose(f);
}

/* load_config() only resets the listener flag and the secret; the rest
 * keep their compiled-in defaults unless set, so reset them per case. */
static void config_defaults(void)
{
	g_http_enabled = 0;
	g_http_port = HTTP_PORT;
	g_bind_addr[0] = '\0';
	g_log_retain_days = 14;
	g_secret[0] = '\0';
}

static void test_load_config(void)
{
	char dir[] = "/tmp/wireview-test-config-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		CHECK(0);
		return;
	}
	snprintf(test_config_path, sizeof(test_config_path), "%s/config", dir);
	unsetenv("WIREVIEW_SECRET");
	unsetenv("WIREVIEW_LISTEN");

	/* No file: listener off, no secret, defaults. */
	config_defaults();
	load_config();
	CHECK_EQ_INT(g_http_enabled, 0);
	CHECK_EQ_STR(g_secret, "");
	CHECK_EQ_INT(g_http_port, 9876);
	CHECK_EQ_STR(g_bind_addr, "");

	/* The sample's defaults, then every key set. */
	write_config("# comment\n\nremote_enabled=0\nport=9876\n#bind=\n#secret=\nlog_days=14\n");
	config_defaults();
	load_config();
	CHECK_EQ_INT(g_http_enabled, 0);
	CHECK_EQ_STR(g_secret, "");
	CHECK_EQ_STR(g_bind_addr, "");

	write_config("remote_enabled=1\r\n"
		     "  port=19876  \n"
		     "bind=127.0.0.1\n"
		     "secret= s3cret pass \t\n"
		     "log_days=3\n");
	config_defaults();
	load_config();
	CHECK_EQ_INT(g_http_enabled, 1);
	CHECK_EQ_INT(g_http_port, 19876);
	CHECK_EQ_STR(g_bind_addr, "127.0.0.1");
	CHECK_EQ_STR(g_secret, "s3cret pass");	/* inner space kept */
	CHECK_EQ_INT(g_log_retain_days, 3);

	/* Truthy spellings, an IPv6 bind, out-of-range ports ignored, the
	 * first secret wins, a bare line is a secret only if none is set. */
	static const struct { const char *v; int on; } truthy_cases[] = {
		{ "1", 1 }, { "true", 1 }, { "Yes", 1 }, { "y", 1 },
		{ "0", 0 }, { "false", 0 }, { "no", 0 }, { "", 0 }, { "on", 0 },
	};
	for (size_t i = 0; i < sizeof(truthy_cases) / sizeof(truthy_cases[0]); i++) {
		char text[64];
		snprintf(text, sizeof(text), "remote_enabled=%s\n", truthy_cases[i].v);
		write_config(text);
		config_defaults();
		load_config();
		CHECK_EQ_INT(g_http_enabled, truthy_cases[i].on);
	}

	write_config("bind=::1\nport=0\nport=70000\nport=abc\nsecret=first\nsecret=second\nbare\n");
	config_defaults();
	load_config();
	CHECK_EQ_STR(g_bind_addr, "::1");
	CHECK_EQ_INT(g_http_port, 9876);
	CHECK_EQ_STR(g_secret, "first");

	write_config("legacy-passphrase\nremote_enabled=1\n");
	config_defaults();
	load_config();
	CHECK_EQ_STR(g_secret, "legacy-passphrase");
	CHECK_EQ_INT(g_http_enabled, 1);

	/* A later bind= replaces an earlier one; an empty one means all. */
	write_config("bind=10.0.0.1\nbind=\n");
	config_defaults();
	load_config();
	CHECK_EQ_STR(g_bind_addr, "");

	/* Blanks around '=' are ignored on both sides. */
	write_config("port = 9000\nsecret\t= spaced\nremote_enabled =1\n");
	config_defaults();
	load_config();
	CHECK_EQ_INT(g_http_port, 9000);
	CHECK_EQ_STR(g_secret, "spaced");
	CHECK_EQ_INT(g_http_enabled, 1);

	/* The environment wins over the file. */
	write_config("remote_enabled=1\nsecret=file\n");
	setenv("WIREVIEW_SECRET", "env", 1);
	setenv("WIREVIEW_LISTEN", "0", 1);
	config_defaults();
	load_config();
	CHECK_EQ_STR(g_secret, "env");
	CHECK_EQ_INT(g_http_enabled, 0);
	setenv("WIREVIEW_SECRET", "", 1);	/* empty: the file's */
	setenv("WIREVIEW_LISTEN", "1", 1);
	write_config("secret=file\n");
	config_defaults();
	load_config();
	CHECK_EQ_STR(g_secret, "file");
	CHECK_EQ_INT(g_http_enabled, 1);
	unsetenv("WIREVIEW_SECRET");
	unsetenv("WIREVIEW_LISTEN");

	unlink(test_config_path);
	rmdir(dir);
	snprintf(test_config_path, sizeof(test_config_path), "/nonexistent/wireview-config");
	config_defaults();
}

static void test_setup_http(void)
{
	char desc[256];
	struct sockaddr_storage sa;
	socklen_t len;
	int fd;

	config_defaults();
	g_http_port = 0;	/* ephemeral: never collides with a live daemon */

	/* bind=127.0.0.1: IPv4 loopback only. */
	snprintf(g_bind_addr, sizeof(g_bind_addr), "127.0.0.1");
	fd = setup_http(desc, sizeof(desc));
	CHECK(fd >= 0);
	CHECK_EQ_STR(desc, "127.0.0.1:0");
	if (fd >= 0) {
		len = sizeof(sa);
		CHECK_EQ_INT(getsockname(fd, (struct sockaddr *)&sa, &len), 0);
		CHECK_EQ_INT(sa.ss_family, AF_INET);
		CHECK_EQ_INT(ntohl(((struct sockaddr_in *)&sa)->sin_addr.s_addr),
			     INADDR_LOOPBACK);
		CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
		CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
		close(fd);
	}

	/* bind=::1: IPv6 loopback (skipped on hosts without IPv6). */
	snprintf(g_bind_addr, sizeof(g_bind_addr), "::1");
	fd = setup_http(desc, sizeof(desc));
	CHECK_EQ_STR(desc, "[::1]:0");
	if (fd >= 0) {
		len = sizeof(sa);
		CHECK_EQ_INT(getsockname(fd, (struct sockaddr *)&sa, &len), 0);
		CHECK_EQ_INT(sa.ss_family, AF_INET6);
		CHECK(IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)&sa)->sin6_addr));
		close(fd);
	} else {
		CHECK(errno == EADDRNOTAVAIL || errno == EAFNOSUPPORT);
	}

	/* Not a numeric address: refused, never widened to all addresses. */
	static const char *const bad[] = { "localhost", "0.0.0.0.0", "1.2.3.4:80", "[::1]" };
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		snprintf(g_bind_addr, sizeof(g_bind_addr), "%s", bad[i]);
		errno = 0;
		QUIET(fd = setup_http(desc, sizeof(desc)));
		CHECK_EQ_INT(fd, -1);
		CHECK_EQ_INT(errno, EINVAL);
		if (fd >= 0)
			close(fd);
	}

	/* Unset: every address, dual-stack [::] (or 0.0.0.0 without IPv6). */
	g_bind_addr[0] = '\0';
	fd = setup_http(desc, sizeof(desc));
	CHECK(fd >= 0);
	CHECK(strcmp(desc, "[::]:0") == 0 || strcmp(desc, "0.0.0.0:0") == 0);
	if (fd >= 0) {
		len = sizeof(sa);
		CHECK_EQ_INT(getsockname(fd, (struct sockaddr *)&sa, &len), 0);
		if (sa.ss_family == AF_INET6) {
			int v6only = -1;
			socklen_t ol = sizeof(v6only);
			CHECK_EQ_INT(getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
						&v6only, &ol), 0);
			CHECK_EQ_INT(v6only, 0);
			CHECK(IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6 *)&sa)->sin6_addr));
		}
		close(fd);
	}
	config_defaults();
}



int main(void)
{
	test_json_escape();
	test_b64();
	test_json_readers();
	test_json_hostile();
	test_http_header();
	test_post_command();
	test_client_idle();
	test_client_activity();
	test_hmac();
	test_frame();
	test_write_hwmon();
	test_energy();
	test_sensors_json();
	test_prom_escape();
	test_metrics();
	test_load_config();
	test_setup_http();
	return check_summary("test_wireviewd");
}
