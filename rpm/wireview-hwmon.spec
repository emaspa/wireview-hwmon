%{!?_udevrulesdir: %global _udevrulesdir %{_prefix}/lib/udev/rules.d}

Name:           wireview-hwmon
Version:        1.5.0
Release:        1%{?dist}
Summary:        WireView Pro II hwmon daemon, CLI and DKMS kernel module

License:        GPL-2.0-only
URL:            https://github.com/emaspa/wireview-hwmon
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  systemd-rpm-macros

# wireviewctl flash updates the device firmware over DFU via dfu-util
Recommends:     dfu-util

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
cc $CFLAGS $LDFLAGS -Wall -Wextra -Wno-format-truncation -o wireviewd wireviewd.c sha256.c
cc $CFLAGS $LDFLAGS -Wall -Wextra -Wno-format-truncation -o wireviewctl wireviewctl.c

%install
install -Dm0755 wireviewd %{buildroot}%{_bindir}/wireviewd
install -Dm0755 wireviewctl %{buildroot}%{_bindir}/wireviewctl
install -Dm0644 debian/wireviewd.service %{buildroot}%{_unitdir}/wireviewd.service
install -Dm0644 99-wireview-hwmon.rules %{buildroot}%{_udevrulesdir}/99-wireview-hwmon.rules
install -Dm0644 firmware/TG-WV-PRO2-FW.hex %{buildroot}%{_datadir}/wireview/TG-WV-PRO2-FW.hex

# DKMS module source
install -Dm0644 wireview_hwmon.c %{buildroot}%{_usrsrc}/%{name}-%{version}/wireview_hwmon.c
install -Dm0644 dkms.conf       %{buildroot}%{_usrsrc}/%{name}-%{version}/dkms.conf
install -Dm0644 Makefile.dkms   %{buildroot}%{_usrsrc}/%{name}-%{version}/Makefile

%files
%doc README.md
%{_bindir}/wireviewd
%{_bindir}/wireviewctl
%{_unitdir}/wireviewd.service
%{_udevrulesdir}/99-wireview-hwmon.rules
%dir %{_datadir}/wireview
%{_datadir}/wireview/TG-WV-PRO2-FW.hex

%post
%systemd_post wireviewd.service

%preun
%systemd_preun wireviewd.service

%postun
%systemd_postun_with_restart wireviewd.service

%files dkms
%{_usrsrc}/%{name}-%{version}/

%post dkms
# Register, build and install the module for the running kernel.
dkms add -m %{name} -v %{version} --rpm_safe_upgrade 2>/dev/null || true
dkms build -m %{name} -v %{version} 2>/dev/null || true
dkms install -m %{name} -v %{version} --force 2>/dev/null || true

%preun dkms
if [ $1 -eq 0 ]; then
    dkms remove -m %{name} -v %{version} --all --rpm_safe_upgrade 2>/dev/null || true
fi

%changelog
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
