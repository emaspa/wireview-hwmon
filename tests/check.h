/*
 * Minimal test helpers: no framework, just counters and a summary.
 *
 * CHECK(cond) records a pass or prints the failing expression with its
 * location. CHECK_EQ_INT/CHECK_EQ_STR/CHECK_EQ_MEM also print both values.
 * Finish main() with "return check_summary(name);".
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef WIREVIEW_TESTS_CHECK_H
#define WIREVIEW_TESTS_CHECK_H

#include <stdio.h>
#include <string.h>

static int check_pass, check_fail;

#define CHECK(cond) do {						\
	if (cond) {							\
		check_pass++;						\
	} else {							\
		check_fail++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n",			\
			__FILE__, __LINE__, #cond);			\
	}								\
} while (0)

#define CHECK_EQ_INT(got, want) do {					\
	long long g_ = (long long)(got), w_ = (long long)(want);	\
	if (g_ == w_) {							\
		check_pass++;						\
	} else {							\
		check_fail++;						\
		fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n",	\
			__FILE__, __LINE__, #got, g_, w_);		\
	}								\
} while (0)

#define CHECK_EQ_STR(got, want) do {					\
	const char *g_ = (got), *w_ = (want);				\
	if (strcmp(g_, w_) == 0) {					\
		check_pass++;						\
	} else {							\
		check_fail++;						\
		fprintf(stderr, "FAIL %s:%d: %s == \"%s\", want \"%s\"\n", \
			__FILE__, __LINE__, #got, g_, w_);		\
	}								\
} while (0)

#define CHECK_EQ_MEM(got, want, n) do {					\
	if (memcmp((got), (want), (n)) == 0) {				\
		check_pass++;						\
	} else {							\
		check_fail++;						\
		fprintf(stderr, "FAIL %s:%d: %s differs from %s (%zu bytes)\n", \
			__FILE__, __LINE__, #got, #want, (size_t)(n));	\
	}								\
} while (0)

/* Run stmt with stderr sent to /dev/null (for calls that are expected to
 * print an error message, so the test output stays readable). */
#define QUIET(stmt) do {						\
	fflush(stderr);							\
	int saved_ = dup(2);						\
	int null_ = open("/dev/null", O_WRONLY);			\
	if (null_ >= 0) { dup2(null_, 2); close(null_); }		\
	stmt;								\
	fflush(stderr);							\
	if (saved_ >= 0) { dup2(saved_, 2); close(saved_); }		\
} while (0)

static inline int check_summary(const char *name)
{
	printf("%s: %d passed, %d failed\n", name, check_pass, check_fail);
	return check_fail ? 1 : 0;
}

#endif /* WIREVIEW_TESTS_CHECK_H */
