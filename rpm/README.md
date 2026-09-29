# RPM / COPR packaging

Fedora packaging, served via COPR. One spec produces three packages:

- **`wireview-hwmon`** - the `wireviewd` daemon, `wireviewctl` CLI, systemd unit
  and udev rule (compiled from source with Fedora's hardened build flags).
- **`wireview-hwmon-dkms`** (noarch) - the kernel module source, built on the
  user's machine via DKMS (`%post`/`%preun` scriptlets run `dkms build/install`
  and `dkms remove`).
- **`wireview-hwmon-firmware`** (noarch, `LicenseRef-Proprietary`) - Thermal
  Grizzly's device firmware image for `wireviewctl flash`, kept out of the GPL
  packages (see `../firmware/README.md`). The main package recommends it, so
  dnf installs it by default. It conflicts with `wireview-hwmon < 1.6.1`,
  which still owned the file, so an upgrade replaces both in one transaction.

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
# provides the GUI, daemon, module and firmware image
copr-cli build emaspa/wireview-linux ~/rpmbuild/SRPMS/wireview-hwmon-*.src.rpm
```

## Install (users)

```bash
sudo dnf copr enable emaspa/wireview-linux
sudo dnf install wireview-hwmon wireview-hwmon-dkms   # + wireview-hwmon-firmware (weak dependency)
sudo systemctl enable --now wireviewd
```

`--setopt=install_weak_deps=False`, or a later `dnf remove
wireview-hwmon-firmware`, leaves the proprietary image out; `wireviewctl flash
FILE` still works with an image you supply.

DKMS needs `kernel-devel` matching the running kernel (pulled as a dependency)
so the module can build.

## Notes

- Validated by building both RPMs and installing the daemon in a Fedora
  container (use a currently supported release, e.g. fedora:43); the kernel
  module compiles via `dkms build` against `kernel-devel`.
- Immutable distros (Bazzite, Silverblue) are not a target for the kernel module
  (DKMS doesn't fit rpm-ostree). Those users run the WireView GUI Flatpak in
  direct-serial mode.
