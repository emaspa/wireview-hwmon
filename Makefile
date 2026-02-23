obj-m := wireview_hwmon.o

KDIR ?= /lib/modules/$(shell uname -r)/build
MDIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

all: module wireviewd

module:
	$(MAKE) -C $(KDIR) M=$(MDIR) modules

wireviewd: wireviewd.c
	$(CC) -Wall -Wextra -Wno-format-truncation -O2 -o wireviewd wireviewd.c

clean:
	$(MAKE) -C $(KDIR) M=$(MDIR) clean
	rm -f wireviewd

install: all
	$(MAKE) -C $(KDIR) M=$(MDIR) modules_install
	depmod -a
	install -m 755 wireviewd /usr/local/bin/wireviewd
	install -m 644 wireviewd.service /etc/systemd/system/wireviewd.service
	install -m 644 99-wireview-hwmon.rules /etc/udev/rules.d/99-wireview-hwmon.rules
	udevadm control --reload-rules
	systemctl daemon-reload

uninstall:
	systemctl stop wireviewd 2>/dev/null || true
	systemctl disable wireviewd 2>/dev/null || true
	rm -f /usr/local/bin/wireviewd
	rm -f /etc/systemd/system/wireviewd.service
	rm -f /etc/udev/rules.d/99-wireview-hwmon.rules
	rm -f /lib/modules/$(shell uname -r)/extra/wireview_hwmon.ko
	depmod -a
	udevadm control --reload-rules
	systemctl daemon-reload

.PHONY: all module clean install uninstall
