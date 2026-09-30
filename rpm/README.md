# RPM / COPR packaging

Fedora packaging, served via COPR. One spec produces two packages:

- **`wireview-hwmon`** - the `wireviewd` daemon, `wireviewctl` CLI, systemd unit
  and udev rule (compiled from source with Fedora's hardened build flags).
- **`wireview-hwmon-dkms`** (noarch) - the kernel module source, built on the
  user's machine via DKMS (`%post`/`%preun` scriptlets run `dkms build/install`
  and `dkms remove`).

COPR/mock only need to *package* the module source - the actual module build
happens on the user's machine at install time, so no kernel is required to build
the RPMs.

## Build the SRPM / RPMs

Requires `rpm-build` + `gcc` (e.g. in a Fedora container or on a Fedora host):

```bash
spectool -g -R rpm/wireview-hwmon.spec     # download Source0 into ~/rpmbuild/SOURCES
rpmbuild -ba rpm/wireview-hwmon.spec
```

## Publish to COPR

```bash
# Shared with the GUI, so one `dnf copr enable emaspa/wireview-linux`
# provides the GUI, the daemon and the module
copr-cli build emaspa/wireview-linux ~/rpmbuild/SRPMS/wireview-hwmon-*.src.rpm
```

## Install (users)

```bash
sudo dnf copr enable emaspa/wireview-linux
sudo dnf install wireview-hwmon wireview-hwmon-dkms
sudo systemctl enable --now wireviewd
```

DKMS needs `kernel-devel` matching the running kernel (pulled as a dependency)
so the module can build.

## Notes

- Validated by building both RPMs and installing the daemon in a Fedora
  container (use a currently supported release, e.g. fedora:43); the kernel
  module compiles via `dkms build` against `kernel-devel`.
- Immutable distros (Bazzite, Silverblue) are not a target for the kernel module
  (DKMS doesn't fit rpm-ostree). Those users run the WireView GUI Flatpak in
  direct-serial mode.
