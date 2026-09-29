%{!?_udevrulesdir: %global _udevrulesdir %{_prefix}/lib/udev/rules.d}

Name:           wireview-hwmon
# Must match the top-level VERSION file ("make check-version").
Version:        1.6.0
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

# wireviewctl flash updates the device firmware over DFU via dfu-util, with
# the image from the firmware subpackage unless given a file
Recommends:     dfu-util
Recommends:     %{name}-firmware

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

%package firmware
Summary:        WireView Pro II device firmware image (proprietary)
# Thermal Grizzly's image ships with no license text, so no redistribution
# terms are claimed beyond Thermal Grizzly's own (see firmware/README.md).
License:        LicenseRef-Proprietary
BuildArch:      noarch
# The image moved here from the main package after 1.6.0. Conflicts (not
# Obsoletes, which would replace the main package) makes an old main
# package upgrade in the same transaction instead of clashing on the file.
Conflicts:      %{name} < 1.6.1

%description firmware
Official firmware image for the Thermal Grizzly WireView Pro II GPU power
monitor, redistributed unmodified from Thermal Grizzly's WireView2 Windows
release. "wireviewctl flash" writes it to the device over USB DFU when no
other image is given; it runs on the device, never on the host. The image is
proprietary Thermal Grizzly software, not covered by the GPL.

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

%files firmware
%doc firmware/README.md
%dir %{_datadir}/wireview
%{_datadir}/wireview/TG-WV-PRO2-FW.hex

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
