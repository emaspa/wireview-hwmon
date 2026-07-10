obj-m := wireview_hwmon.o

KDIR ?= /lib/modules/$(shell uname -r)/build
MDIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

all: module wireviewd wireviewctl

module:
	$(MAKE) -C $(KDIR) M=$(MDIR) modules

wireviewd: wireviewd.c sha256.c sha256.h
	$(CC) -Wall -Wextra -Wno-format-truncation -O2 -o wireviewd wireviewd.c sha256.c

wireviewctl: wireviewctl.c
	$(CC) -Wall -Wextra -Wno-format-truncation -O2 -o wireviewctl wireviewctl.c

clean:
	$(MAKE) -C $(KDIR) M=$(MDIR) clean
	rm -f wireviewd wireviewctl

install: all
	$(MAKE) -C $(KDIR) M=$(MDIR) modules_install
	depmod -a
	install -m 755 wireviewd /usr/local/bin/wireviewd
	install -m 755 wireviewctl /usr/local/bin/wireviewctl
	install -D -m 644 firmware/TG-WV-PRO2-FW.hex /usr/share/wireview/TG-WV-PRO2-FW.hex
	install -m 644 wireviewd.service /etc/systemd/system/wireviewd.service
	install -m 644 99-wireview-hwmon.rules /etc/udev/rules.d/99-wireview-hwmon.rules
	install -d /etc/modules-load.d
	echo wireview_hwmon > /etc/modules-load.d/wireview-hwmon.conf
	install -d /etc/avahi/services
	install -m 644 avahi-wireview.service /etc/avahi/services/wireview.service
	install -d -m 700 /etc/wireview
	install -d -m 750 /var/log/wireview
	[ -f /etc/wireview/config ] || install -m 600 wireview-config.sample /etc/wireview/config
	@echo "Note: the LAN listener is OFF by default. /etc/wireview/config holds the settings (a"
	@echo "      reference with the defaults is created on first install): set remote_enabled=1 (+ a"
	@echo "      secret) to publish; port= and log_days= as needed. Logs go to /var/log/wireview."
	@echo "      For 'wireviewctl top', list remote hosts (one host[:port] per line) in /etc/wireview/hosts."
	udevadm control --reload-rules
	systemctl daemon-reload

uninstall:
	systemctl stop wireviewd 2>/dev/null || true
	systemctl disable wireviewd 2>/dev/null || true
	rm -f /usr/local/bin/wireviewd
	rm -f /usr/local/bin/wireviewctl
	rm -f /etc/systemd/system/wireviewd.service
	rm -f /etc/udev/rules.d/99-wireview-hwmon.rules
	rm -f /usr/share/wireview/TG-WV-PRO2-FW.hex
	rmdir /usr/share/wireview 2>/dev/null || true
	rm -f /etc/modules-load.d/wireview-hwmon.conf
	rm -f /etc/avahi/services/wireview.service
	rm -f /lib/modules/$(shell uname -r)/extra/wireview_hwmon.ko
	depmod -a
	udevadm control --reload-rules
	systemctl daemon-reload

.PHONY: all module clean install uninstall
