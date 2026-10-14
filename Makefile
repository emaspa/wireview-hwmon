KDIR ?= /lib/modules/$(shell uname -r)/build
MDIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

# Single source of truth for the package version. The literals that tools
# require (dkms.conf, rpm Version:, PKGBUILD pkgver, debian/changelog) are
# checked against it by "make check-version".
VERSION := $(strip $(shell cat $(MDIR)/VERSION))

# Kernel module (kbuild reads this file when invoked with M=).
obj-m := wireview_hwmon.o
ccflags-y += -DWIREVIEW_PKG_VERSION=\"$(VERSION)\"

# Userspace: distro CFLAGS/CPPFLAGS/LDFLAGS (dpkg-buildflags, %set_build_flags,
# makepkg) replace the -O2 default; the warnings are always added on top.
# Kbuild ignores CFLAGS (older kernels even reject kbuild files that set it),
# so only default it when this file is read as a plain Makefile.
ifeq ($(KERNELRELEASE),)
CFLAGS ?= -O2
endif
WARNINGS := -Wall -Wextra -Wno-format-truncation
VERSION_DEF := -DWIREVIEW_PKG_VERSION=\"$(VERSION)\"

all: module wireviewd wireviewctl

module:
	$(MAKE) -C $(KDIR) M=$(MDIR) modules

wireviewd: wireviewd.c sha256.c sha256.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_DEF) $(CFLAGS) $(WARNINGS) $(LDFLAGS) -o $@ wireviewd.c sha256.c $(LDLIBS)

wireviewctl: wireviewctl.c sha256.c sha256.h VERSION
	$(CC) $(CPPFLAGS) $(VERSION_DEF) $(CFLAGS) $(WARNINGS) $(LDFLAGS) -o $@ wireviewctl.c sha256.c $(LDLIBS)

clean:
	$(MAKE) -C $(KDIR) M=$(MDIR) clean
	rm -f wireviewd wireviewctl

# Arch-based distros have no dialout group, their serial group is uucp. The
# serial port gets it from the distro's default rules; the DFU bootloader's
# line names dialout, which udev drops where that group does not exist, so the
# installed rules name uucp there.
#
# /etc/wireview is root:wireview 0750 and its config 0660, so the group can
# edit the daemon settings (wireviewctl config set). The config is only
# installed when missing, so the chgrp/chmod lines bring an install from an
# earlier release, where both were root-only, to the same modes (GNU install
# -d already does that for the directory; the explicit pair does not depend
# on it).
install: all
	$(MAKE) -C $(KDIR) M=$(MDIR) modules_install
	depmod -a
	getent group wireview >/dev/null || groupadd -r wireview
	install -m 755 wireviewd /usr/local/bin/wireviewd
	install -m 755 wireviewctl /usr/local/bin/wireviewctl
	install -D -m 644 firmware/TG-WV-PRO2-FW.hex /usr/share/wireview/TG-WV-PRO2-FW.hex
	install -m 644 wireviewd.service /etc/systemd/system/wireviewd.service
	install -m 644 99-wireview-hwmon.rules /etc/udev/rules.d/99-wireview-hwmon.rules
	if ! getent group dialout >/dev/null && getent group uucp >/dev/null; then \
		sed -i 's/GROUP="dialout"/GROUP="uucp"/g' /etc/udev/rules.d/99-wireview-hwmon.rules; \
	fi
	install -d /etc/modules-load.d
	echo wireview_hwmon > /etc/modules-load.d/wireview-hwmon.conf
	install -d /etc/avahi/services
	install -m 644 avahi-wireview.service /etc/avahi/services/wireview.service
	install -d -m 750 -g wireview /etc/wireview
	chgrp wireview /etc/wireview && chmod 750 /etc/wireview
	install -d -m 750 /var/log/wireview
	[ -f /etc/wireview/config ] || install -m 660 -g wireview wireview-config.sample /etc/wireview/config
	chgrp wireview /etc/wireview/config && chmod 660 /etc/wireview/config
	@echo "Note: the LAN listener is OFF by default. /etc/wireview/config holds the settings (a"
	@echo "      reference with the defaults is created on first install): to publish, run"
	@echo "      'wireviewctl config set remote_enabled=1' (+ secret=-, port=, log_days= as needed)."
	@echo "      Members of the wireview group may do that without sudo. Logs go to /var/log/wireview."
	@echo "      For 'wireviewctl top', list remote hosts (one host[:port] per line) in /etc/wireview/hosts."
	udevadm control --reload-rules
	systemctl daemon-reload

# modules_install puts external modules in updates/ (older kernels: extra/),
# compressed as .ko.{zst,xz,gz} when CONFIG_MODULE_COMPRESS_* is set.
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
	rm -f $(foreach d,updates extra,$(foreach x,ko ko.zst ko.xz ko.gz,\
		/lib/modules/$(shell uname -r)/$(d)/wireview_hwmon.$(x)))
	depmod -a
	udevadm control --reload-rules
	systemctl daemon-reload

# Fail if a version literal that packaging tools need has drifted from VERSION.
check-version:
	@v='$(VERSION)'; rc=0; \
	grep -qx "PACKAGE_VERSION=\"$$v\"" dkms.conf || { echo "dkms.conf: PACKAGE_VERSION is not $$v"; rc=1; }; \
	grep -Eqx "Version:[[:space:]]+$$v" rpm/wireview-hwmon.spec || { echo "rpm/wireview-hwmon.spec: Version is not $$v"; rc=1; }; \
	grep -qx "pkgver=$$v" aur/PKGBUILD || { echo "aur/PKGBUILD: pkgver is not $$v"; rc=1; }; \
	grep -qx "	pkgver = $$v" aur/.SRCINFO || { echo "aur/.SRCINFO: pkgver is not $$v"; rc=1; }; \
	head -n1 debian/changelog | grep -Fq "($$v" || { echo "debian/changelog: top entry is not $$v"; rc=1; }; \
	[ $$rc -eq 0 ] && echo "version $$v consistent"; exit $$rc

.PHONY: all module clean install uninstall check-version test

# Unit tests and the fake-device end-to-end test (see tests/Makefile).
test:
	$(MAKE) -C tests
