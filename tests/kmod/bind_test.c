/*
 * Exercises the wireview_hwmon module's instances from userspace: the
 * legacy unbound write, WIREVIEW_IOC_BIND for instance 0 and N >= 1, the
 * identity attributes, EBUSY/EINVAL, removal on close, and a stress loop
 * of bind/write/close while other processes read every hwmon attribute.
 * Runs as root inside the VM that tests/kmod/run-vm-test.sh boots.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

struct __attribute__((packed)) hwmon_data {
	uint32_t magic, version;
	int32_t voltage_mv[6], current_ma[6];
	int64_t total_power_uw;
	int32_t temp_mc[4];
	int64_t pin_power_uw[6];
	int32_t total_current_ma, avg_voltage_mv, vdd_mv;
	uint8_t fan_duty, psu_cap;
	uint16_t fault_status, fault_log, _pad;
	int64_t energy_uj;
};

struct wireview_bind {
	uint32_t index;
	char serial[16];
	char uid[28];
	char label[48];
};
#define WIREVIEW_IOC_BIND _IOW('W', 0xA0, struct wireview_bind)

static int failures;

#define CHECK(cond, ...) do { \
	if (cond) { printf("ok   "); } else { printf("FAIL "); failures++; } \
	printf(__VA_ARGS__); printf("\n"); fflush(stdout); \
} while (0)

static int dev_open(void)
{
	return open("/dev/wireview-hwmon", O_WRONLY | O_CLOEXEC);
}

static int bind_to(int fd, unsigned idx, const char *serial)
{
	struct wireview_bind b;

	memset(&b, 0, sizeof(b));
	b.index = idx;
	snprintf(b.serial, sizeof(b.serial), "%s", serial);
	snprintf(b.uid, sizeof(b.uid), "UID%s", serial);
	snprintf(b.label, sizeof(b.label), "Test %s", serial);
	return ioctl(fd, WIREVIEW_IOC_BIND, &b) == 0 ? 0 : -errno;
}

static int send_frame(int fd, int mv)
{
	struct hwmon_data d;

	memset(&d, 0, sizeof(d));
	d.magic = 0x57565032;
	d.version = 3;
	for (int i = 0; i < 6; i++)
		d.voltage_mv[i] = mv;
	return write(fd, &d, sizeof(d)) == sizeof(d) ? 0 : -errno;
}

/* The hwmon directory of platform device pdev ("wireview_hwmon" or
 * "wireview_hwmon.N"), or "" if it has none. */
static void hwmon_dir(const char *pdev, char *out, size_t cap)
{
	char pat[256];
	glob_t g;

	out[0] = '\0';
	snprintf(pat, sizeof(pat), "/sys/devices/platform/%s/hwmon/hwmon*", pdev);
	if (glob(pat, 0, NULL, &g) == 0) {
		snprintf(out, cap, "%s", g.gl_pathv[0]);
		globfree(&g);
	}
}

/* Contents of attr (newline stripped) or "-ERRNO". */
static void attr(const char *dir, const char *name, char *out, size_t cap)
{
	char p[512];
	int fd;
	ssize_t n;

	snprintf(p, sizeof(p), "%s/%s", dir, name);
	fd = open(p, O_RDONLY);
	if (fd < 0) {
		snprintf(out, cap, "-%d", errno);
		return;
	}
	n = read(fd, out, cap - 1);
	if (n < 0) {
		snprintf(out, cap, "-%d", errno);
	} else {
		out[n] = '\0';
		out[strcspn(out, "\n")] = '\0';
	}
	close(fd);
}

static int count_instances(void)
{
	glob_t g;
	int n = 0;

	if (glob("/sys/devices/platform/wireview_hwmon*/hwmon/hwmon*", 0, NULL, &g) == 0) {
		n = (int)g.gl_pathc;
		globfree(&g);
	}
	return n;
}

/* Child: read every attribute of every wireview hwmon device, forever. */
static void reader(void)
{
	char buf[256];

	for (;;) {
		glob_t g;

		if (glob("/sys/class/hwmon/hwmon*/*", 0, NULL, &g) != 0)
			continue;
		for (size_t i = 0; i < g.gl_pathc; i++) {
			int fd = open(g.gl_pathv[i], O_RDONLY);

			if (fd >= 0) {
				(void)!read(fd, buf, sizeof(buf));
				close(fd);
			}
		}
		globfree(&g);
	}
}

int main(int argc, char **argv)
{
	char d0[256], d1[256], v[128];
	int fd0, fd1, fd2, fd3, rc;
	int stress = argc > 1 ? atoi(argv[1]) : 2000;

	hwmon_dir("wireview_hwmon", d0, sizeof(d0));
	CHECK(d0[0], "instance 0 exists at load: %s", d0);
	attr(d0, "name", v, sizeof(v));
	CHECK(strcmp(v, "wireview") == 0, "instance 0 name is wireview (%s)", v);
	attr(d0, "serial", v, sizeof(v));
	CHECK(strcmp(v, "-61") == 0, "unbound instance 0 serial is ENODATA (%s)", v);

	/* Legacy: an unbound file feeds instance 0. */
	fd0 = dev_open();
	CHECK(fd0 >= 0 && send_frame(fd0, 12001) == 0, "unbound write accepted");
	attr(d0, "in0_input", v, sizeof(v));
	CHECK(strcmp(v, "12001") == 0, "unbound write lands on instance 0 (%s)", v);

	/* BIND index 1 creates wireview_hwmon.1. */
	fd1 = dev_open();
	rc = bind_to(fd1, 1, "SER1");
	CHECK(rc == 0, "BIND 1 succeeds (%d)", rc);
	hwmon_dir("wireview_hwmon.1", d1, sizeof(d1));
	CHECK(d1[0], "instance 1 has a hwmon device: %s", d1);
	attr(d1, "name", v, sizeof(v));
	CHECK(strcmp(v, "wireview1") == 0, "instance 1 name is wireview1 (%s)", v);
	attr(d1, "serial", v, sizeof(v));
	CHECK(strcmp(v, "SER1") == 0, "instance 1 serial (%s)", v);
	attr(d1, "uid", v, sizeof(v));
	CHECK(strcmp(v, "UIDSER1") == 0, "instance 1 uid (%s)", v);
	attr(d1, "label", v, sizeof(v));
	CHECK(strcmp(v, "Test SER1") == 0, "instance 1 label (%s)", v);
	attr(d1, "in0_input", v, sizeof(v));
	CHECK(strcmp(v, "-61") == 0, "instance 1 has no data yet (%s)", v);
	CHECK(send_frame(fd1, 11500) == 0, "write on bound file accepted");
	attr(d1, "in0_input", v, sizeof(v));
	CHECK(strcmp(v, "11500") == 0, "bound write lands on instance 1 (%s)", v);
	attr(d0, "in0_input", v, sizeof(v));
	CHECK(strcmp(v, "12001") == 0, "instance 0 unchanged (%s)", v);

	rc = bind_to(fd1, 2, "X");
	CHECK(rc == -EBUSY, "second BIND on one file is EBUSY (%d)", rc);
	fd2 = dev_open();
	rc = bind_to(fd2, 1, "X");
	CHECK(rc == -EBUSY, "BIND of a taken index is EBUSY (%d)", rc);
	rc = bind_to(fd2, 32, "X");
	CHECK(rc == -EINVAL, "BIND 32 is EINVAL (%d)", rc);
	rc = bind_to(fd2, 0, "SER0");
	CHECK(rc == 0, "BIND 0 succeeds (%d)", rc);
	attr(d0, "serial", v, sizeof(v));
	CHECK(strcmp(v, "SER0") == 0, "instance 0 serial after BIND (%s)", v);
	fd3 = dev_open();
	rc = bind_to(fd3, 0, "X");
	CHECK(rc == -EBUSY, "BIND 0 while held is EBUSY (%d)", rc);
	CHECK(send_frame(fd3, 12345) == 0, "an unbound file still feeds instance 0");
	close(fd3);

	close(fd1);
	hwmon_dir("wireview_hwmon.1", d1, sizeof(d1));
	CHECK(!d1[0] && access("/sys/devices/platform/wireview_hwmon.1", F_OK) != 0,
	      "closing the bound file removes instance 1");
	close(fd2);
	attr(d0, "serial", v, sizeof(v));
	CHECK(strcmp(v, "-61") == 0, "closing the BIND 0 file clears instance 0 identity (%s)", v);
	hwmon_dir("wireview_hwmon", d0, sizeof(d0));
	CHECK(d0[0], "instance 0 stays");
	fd1 = dev_open();
	CHECK(bind_to(fd1, 1, "SER1b") == 0, "index 1 is free again after close");
	close(fd1);
	close(fd0);

	/* Stress: bind/write/close on 8 indexes while 4 readers hammer sysfs. */
	pid_t kids[4];
	for (int i = 0; i < 4; i++) {
		kids[i] = fork();
		if (kids[i] == 0) {
			reader();
			_exit(0);
		}
	}
	int errs = 0, max_seen = 0;
	for (int it = 0; it < stress; it++) {
		int fds[8];
		for (int k = 0; k < 8; k++) {
			char s[16];
			snprintf(s, sizeof(s), "S%d", k);
			fds[k] = dev_open();
			if (fds[k] < 0 || bind_to(fds[k], (unsigned)k, s) != 0 ||
			    send_frame(fds[k], 11000 + k) != 0)
				errs++;
		}
		int n = count_instances();
		if (n > max_seen)
			max_seen = n;
		for (int k = 0; k < 8; k++)
			if (fds[k] >= 0)
				close(fds[k]);
	}
	for (int i = 0; i < 4; i++) {
		kill(kids[i], SIGKILL);
		waitpid(kids[i], NULL, 0);
	}
	CHECK(errs == 0 && max_seen == 8,
	      "stress: %d rounds of 8 bind/write/close under concurrent sysfs reads (%d errors, max %d instances)",
	      stress, errs, max_seen);
	CHECK(count_instances() == 1, "stress: only instance 0 is left (%d)", count_instances());

	printf("bind_test: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
