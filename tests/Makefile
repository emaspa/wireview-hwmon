# Unit tests.
#
#   make -C tests          build and run everything
#   make -C tests unit     unit tests only
#
# SANITIZE=0 builds without ASan/UBSan.

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

all: unit

unit: $(UNIT)
	@rc=0; for t in $(UNIT); do ./$$t || rc=1; done; exit $$rc

test_wireviewd: test_wireviewd.c check.h $(TOP)/wireviewd.c $(TOP)/sha256.c $(TOP)/sha256.h
	$(CC) $(TEST_CFLAGS) $(SAN_FLAGS) $(TEST_DEFS) -I$(TOP) -o $@ test_wireviewd.c $(TOP)/sha256.c

test_wireviewctl: test_wireviewctl.c check.h $(TOP)/wireviewctl.c
	$(CC) $(TEST_CFLAGS) $(SAN_FLAGS) $(TEST_DEFS) -I$(TOP) -o $@ test_wireviewctl.c

clean:
	rm -f $(UNIT)

.PHONY: all unit clean
