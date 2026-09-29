# Unit tests and the fake-device end-to-end test.
#
#   make -C tests          build and run everything
#   make -C tests unit     unit tests only
#   make -C tests e2e      end-to-end test only (needs python3)
#
# SANITIZE=0 builds without ASan/UBSan. WIREVIEW_SKIP_E2E=1 skips the e2e
# test, which is also skipped when python3 is missing.

TOP := ..
VERSION := $(strip $(shell cat $(TOP)/VERSION))

TEST_CFLAGS := -g -O1 -Wall -Wextra -Wno-format-truncation -fno-omit-frame-pointer
# Paths the daemon would otherwise touch on the host; nothing in the unit
# tests uses them, but they must never point at the live service.
TEST_DEFS := -DWIREVIEW_PKG_VERSION=\"$(VERSION)-test\" \
	-DHWMON_DEV=\"/nonexistent/wireview-hwmon\" \
	-DSOCK_PATH=\"/nonexistent/wireviewd.sock\" \
	-DLOG_DIR=\"/nonexistent/wireview-log\" \
	-DCONFIG_PATH=\"/nonexistent/wireview-config\"

SANITIZE ?= 1
ifeq ($(SANITIZE),1)
SAN_FLAGS := $(shell printf 'int main(void){return 0;}\n' | \
	$(CC) -x c -fsanitize=address,undefined -o /dev/null - 2>/dev/null && \
	echo -fsanitize=address,undefined -fno-sanitize-recover=undefined)
endif

UNIT := test_wireviewd test_wireviewctl

all: unit e2e

unit: $(UNIT)
	@rc=0; for t in $(UNIT); do ./$$t || rc=1; done; exit $$rc

test_wireviewd: test_wireviewd.c check.h $(TOP)/wireviewd.c $(TOP)/sha256.c $(TOP)/sha256.h
	$(CC) $(TEST_CFLAGS) $(SAN_FLAGS) $(TEST_DEFS) -I$(TOP) -o $@ test_wireviewd.c $(TOP)/sha256.c

# WV_SRCDIR: where the test reads wireviewd.c to compare "sensors --json"
# keys with the daemon's GET /sensors.
test_wireviewctl: test_wireviewctl.c check.h $(TOP)/wireviewctl.c $(TOP)/wireviewd.c $(TOP)/sha256.c $(TOP)/sha256.h
	$(CC) $(TEST_CFLAGS) $(SAN_FLAGS) $(TEST_DEFS) -DWV_SRCDIR=\"$(abspath $(TOP))\" -I$(TOP) -o $@ test_wireviewctl.c $(TOP)/sha256.c

e2e:
	@if [ -n "$(WIREVIEW_SKIP_E2E)" ]; then \
		echo "e2e: skipped (WIREVIEW_SKIP_E2E is set)"; \
	elif ! command -v python3 >/dev/null 2>&1; then \
		echo "e2e: skipped (python3 not found)"; \
	else \
		CC="$(CC)" SAN_FLAGS="$(SAN_FLAGS)" python3 ./e2e.py; \
	fi

clean:
	rm -f $(UNIT)

.PHONY: all unit e2e clean
