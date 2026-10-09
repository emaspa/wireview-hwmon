%{!?_udevrulesdir: %global _udevrulesdir %{_prefix}/lib/udev/rules.d}

Name:           wireview-hwmon
# Must match the top-level VERSION file ("make check-version").
Version:        1.7.2
Release:        1%{?dist}
Summary:        WireView Pro II hwmon daemon, CLI and DKMS kernel module

License:        GPL-2.0-only
URL:            https://github.com/emaspa/wireview-hwmon
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  systemd-rpm-macros
# %%pre creates the wireview group
Requires(pre):  shadow-utils

# wireviewctl flash updates the device firmware over DFU via dfu-util
Recommends:     dfu-util
# 1.7.0 shipped the firmware image in a package of its own
Obsoletes:      %{name}-firmware < 1.7.1
Provides:       %{name}-firmware = %{version}-%{release}

%description
Userspace daemon and CLI tool for the Thermal Grizzly WireView Pro II GPU power
monitor. The daemon reads the device over USB serial and feeds sensor data to
the wireview_hwmon kernel module. Includes wireviewd (daemon), wireviewctl
(CLI), a systemd service and udev rules.

%package dkms
Summary:        WireView Pro II hwmon kernel module (DKMS)
BuildArch:      noarch
Requires:       dkms
Requires:       gcc, make
# The matching kernel-devel/kernel-headers for the running kernel must be
# present for DKMS to build the module.
Requires:       (kernel-devel or kernel-headers)
Supplements:    %{name}

%description dkms
Linux hwmon kernel module for the Thermal Grizzly WireView Pro II GPU power
monitor, exposing voltage, current, power and temperature through
/sys/class/hwmon/. Built and rebuilt automatically via DKMS.

%prep
%autosetup

%build
%set_build_flags
# Userspace only; the Makefile picks up the exported CFLAGS/LDFLAGS.
%make_build wireviewd wireviewctl

%install
install -Dm0755 wireviewd %{buildroot}%{_bindir}/wireviewd
install -Dm0755 wireviewctl %{buildroot}%{_bindir}/wireviewctl
install -Dm0644 debian/wireviewd.service %{buildroot}%{_unitdir}/wireviewd.service
install -Dm0644 99-wireview-hwmon.rules %{buildroot}%{_udevrulesdir}/99-wireview-hwmon.rules
install -Dm0644 firmware/TG-WV-PRO2-FW.hex %{buildroot}%{_datadir}/wireview/TG-WV-PRO2-FW.hex
install -Dm0600 wireview-config.sample %{buildroot}%{_sysconfdir}/wireview/config

# DKMS module source (version baked into dkms.conf and MODULE_VERSION)
install -Dm0644 wireview_hwmon.c %{buildroot}%{_usrsrc}/%{name}-%{version}/wireview_hwmon.c
sed 's/^PACKAGE_VERSION=.*/PACKAGE_VERSION="%{version}"/' dkms.conf \
    > %{buildroot}%{_usrsrc}/%{name}-%{version}/dkms.conf
sed 's/@VERSION@/%{version}/' Makefile.dkms \
    > %{buildroot}%{_usrsrc}/%{name}-%{version}/Makefile
chmod 0644 %{buildroot}%{_usrsrc}/%{name}-%{version}/dkms.conf \
    %{buildroot}%{_usrsrc}/%{name}-%{version}/Makefile

%files
%doc README.md
%{_bindir}/wireviewd
%{_bindir}/wireviewctl
%{_unitdir}/wireviewd.service
%{_udevrulesdir}/99-wireview-hwmon.rules
%dir %{_datadir}/wireview
%{_datadir}/wireview/TG-WV-PRO2-FW.hex
%dir %attr(0700,root,root) %{_sysconfdir}/wireview
%config(noreplace) %attr(0600,root,root) %{_sysconfdir}/wireview/config

%pre
# Members of this group may send wireviewd's privileged socket commands
# (bootloader, NVM, config write, serial handover).
getent group wireview >/dev/null || groupadd -r wireview
exit 0

%post
%systemd_post wireviewd.service

%preun
%systemd_preun wireviewd.service

%postun
%systemd_postun_with_restart wireviewd.service

%files dkms
%{_usrsrc}/%{name}-%{version}/

%post dkms
# Heal orphaned registrations left by upgrades from releases whose %%preun
# only deregistered on erase: the old version stayed in /var/lib/dkms with a
# dangling source symlink and broke the dkms kernel prerm hook.
for dir in /var/lib/dkms/%{name}/*/; do
    [ -d "$dir" ] || continue
    v=$(basename "$dir")
    case "$v" in kernel-*) continue ;; esac
    [ -e "/var/lib/dkms/%{name}/$v/source/dkms.conf" ] || \
        rm -rf "/var/lib/dkms/%{name}/$v"
done
for link in /var/lib/dkms/%{name}/kernel-*; do
    if [ -L "$link" ] && [ ! -e "$link" ]; then
        rm -f "$link"
    fi
done
# Register, build and install the module for the running kernel.
dkms add -m %{name} -v %{version} --rpm_safe_upgrade 2>/dev/null || true
# Build errors stay visible; || true keeps a failed build from aborting the
# transaction.
dkms build -m %{name} -v %{version} || true
dkms install -m %{name} -v %{version} --force || true
dkms status -m %{name} -v %{version} -k "$(uname -r)" 2>/dev/null | grep -q ': installed' || \
    echo "wireview-hwmon-dkms: module not installed for kernel $(uname -r); install kernel-devel for it and run: dkms install -m %{name} -v %{version}" >&2

%preun dkms
# Run on erase AND upgrade: --rpm_safe_upgrade coordinates the remove/add
# pair during upgrades so the outgoing version is deregistered before rpm
# deletes its /usr/src tree. Gating this on $1 -eq 0 left orphans behind.
dkms remove -m %{name} -v %{version} --all --rpm_safe_upgrade 2>/dev/null || true

%changelog
* Fri Oct 09 2026 Emanuele Sparvoli <sparvoli@gmail.com> - 1.7.2-1
- wireviewd locks the serial port (TIOCEXCL) while it runs, so another
  program can no longer open it and split the device's replies. A
  SUSPEND_SERIAL handover lifts the lock and polling takes it back.
- A disconnected temperature sensor is published as -100.0, the value the
  device reports, instead of 0.0 in /sensors and wireviewctl sensors --json.
  LAN viewers showed 0.0 as a real reading. wireviewctl reads any sensor
  below -40 as absent.
- Noctua Edition support is now tested on a real unit, provided by Thermal
  Grizzly.

* Wed Sep 30 2026 Emanuele Sparvoli <sparvoli@gmail.com> - 1.7.1-1
- The firmware image is back in the wireview-hwmon package. 1.7.0 had moved
  it to wireview-hwmon-firmware; upgrading removes that package and keeps
  the image.

* Wed Sep 30 2026 Emanuele Sparvoli <sparvoli@gmail.com> - 1.7.0-1
- Fix: members of the "wireview" group were refused every privileged
  command when wireviewd ran under systemd (1.6.0). The unit's
  PrivateUsers=yes made every socket peer except root appear as uid 65534,
  so the group check never matched. The option is removed.
- WireView Pro II Noctua Edition (product EF06) is supported. wireviewd
  reports the vendor and product id to clients (GET_DEVICE_INFO, /sensors
  hwRev and name, /metrics labels); wireviewctl shows product and edition.
  WireView II and its Phanteks Edition are refused with a clear message.
- Bundled firmware updated to v05 build 20260902_0741 (upstream 1.0.8),
  now shipped in its own package wireview-hwmon-firmware, recommended by
  wireview-hwmon.
- wireviewctl flash checks the image against the device before anything is
  sent: product match (the Noctua Edition takes the Pro II image), no
  downgrade by version or build date, every Intel HEX record validated.
  New --force overrides a refusal; -y only skips the prompt.
- hwmon: the deprecated fan1_input and psu_cap attributes are removed; use
  pwm1 and power1_cap. Supported kernels start at Linux 6.8.
- wireviewd: idle command-socket clients are closed after 60 s, 8 clients
  are served and the longest-idle one is evicted when the table is full.
  Use wireview-linux 1.2.5.0 or later, which reconnects and retries.
- wireviewd: POST /command matches JSON keys only at the top level of the
  request; config keys accept blanks around "=".
- wireviewd: frames with physically impossible readings are discarded as
  corrupt (a pin outside -1..20 V or above 60 A, Vdd above 6 V). This check
  is preventive: no such frame has been observed, and the energy counter
  was verified against a real load.
- wireviewctl: remote /sensors and /config are read by structure, so braces
  or key-like text in a string cannot break top or --host; readings the
  remote does not send are left out instead of shown as zero.
- sha256: ct_str_equal no longer treats strings whose lengths differ by a
  multiple of 256 as equal.
- udev: ModemManager is told to ignore the device; it used to probe the
  serial port for about half a minute after every plug-in.
- packaging: the systemd unit and udev rules are installed under /usr/lib;
  the postinst no longer fails without udev.

* Tue Sep 29 2026 Emanuele Sparvoli <sparvoli@gmail.com> - 1.6.0-1
- hwmon: standard attributes: pwm1 (fan duty), power1_cap (PSU capability),
  energy1_input (energy since daemon start), and temp/curr/power alarms
  derived from the device fault bits. fan1_input and psu_cap are
  deprecated and will be removed after this release. The module accepts
  both the old 148-byte and the new 156-byte sensor record.
- wireviewd: privileged socket commands (bootloader, NVM, config write,
  serial handover) now require root or the new "wireview" system group;
  other peers get status 3 (denied). The package creates the group; add
  users with "usermod -aG wireview USER".
- wireviewd: command socket persists across device unplug/replug; clients
  are served non-blocking with a 2 s request deadline; HTTP requests are
  bounded to 3 s; energy integration; GET /metrics (Prometheus); bind=
  config key with IPv6 support; -V prints the version.
- wireviewd: bug fixes: -d path kept across reconnects, per-bit fault
  debounce, JSON escaping of host and build string, ordered journal
  output, config keys accept blanks around "=".
- wireviewctl: clear-faults sends the correct keep-masks and takes
  optional bit arguments; sensors --json; --host remote mode with
  HMAC-signed writes (--secret-file / WIREVIEW_SECRET); new sensor lines
  for pwm1, power1_cap, energy and alarms; -V/--version; top no longer
  spins with stdin at EOF; denied commands are reported as failures.
- systemd: sandboxed unit (ProtectSystem=strict, no capabilities, device
  policy limited to ttyACM and misc). udev: serial and DFU devices are
  0660 root:dialout plus seat uaccess; the hwmon node is root-only.
- packaging: /etc/wireview/config shipped as a conffile; debhelper-compat
  13 with dh_installsystemd owning the service; hardened build flags via
  dpkg-buildflags; single VERSION file; copyright lists the CC0 sha256
  code and the proprietary firmware image; DKMS output no longer hidden
  (rpm).
- tests: unit tests, a pty-based end-to-end test with a fake device, and
  a GitHub Actions workflow.

* Sat Jul 18 2026 Emanuele Sparvoli <sparvoli@gmail.com> - 1.5.1-1
- dkms: deregister on upgrade as well as erase (drop the $1 -eq 0 guard on
  preun); upgrades used to leave the old version registered in /var/lib/dkms
  with a dangling source symlink, breaking the dkms kernel prerm hook.
- dkms: post heals orphaned registrations left by earlier upgrades.
- wireviewctl flash: state unofficial, at-your-own-risk in the confirm prompt.

* Fri Jul 10 2026 Emanuele Sparvoli <sparvoli@gmail.com> - 1.5.0-1
- wireviewd: serial handover protocol (suspend/resume serial) so clients can
  borrow the USB serial port for log reads, theme uploads and firmware
  flashing while the daemon pauses polling
- wireviewctl: new "flash" command, DFU firmware update via dfu-util; with no
  file argument it flashes the bundled image, headless with -y
- Ship the official firmware image (v05) at /usr/share/wireview/TG-WV-PRO2-FW.hex
- Recommend dfu-util

* Thu Jun 11 2026 Emanuele Sparvoli <sparvoli@gmail.com> - 1.4.1-1
- Fix phantom fault alerts: discard corrupt (desynced) serial frames via
  padding/fan-duty sanity checks and debounce fault bits across two
  consecutive frames; suppressed/discarded frames are audit-logged

* Wed Jun 10 2026 Emanuele Sparvoli <sparvoli@gmail.com> - 1.4.0-1
- LAN fleet monitoring: opt-in /sensors publisher, HMAC-authenticated remote
  writes and config, wireviewctl top live monitor, daily audit logging,
  configurable listener port, reference /etc/wireview/config.

* Sat Jun 06 2026 Emanuele Sparvoli <sparvoli@gmail.com> - 1.3.2-1
- Initial RPM / COPR packaging (daemon + CLI + DKMS kernel module)
