# AUR packaging

A split AUR package (one `pkgbase`, three installable packages) for Arch and
Arch-based distros (CachyOS, EndeavourOS):

- **`wireview-hwmon`** - the `wireviewd` daemon, `wireviewctl` CLI, systemd unit
  and udev rule (built from source - small C programs).
- **`wireview-hwmon-dkms`** - the kernel module, built on the user's machine via
  DKMS. Arch's `dkms` pacman hooks build/install it automatically on install and
  rebuild it on kernel upgrades, so no custom `.install` is needed.
- **`wireview-hwmon-firmware`** (`any`, `LicenseRef-Proprietary`) - Thermal
  Grizzly's proprietary device firmware image for `wireviewctl flash`, an
  optional dependency of `wireview-hwmon`. `firmware/README.md` goes to
  `/usr/share/licenses/wireview-hwmon-firmware/` in place of a license text.
  Releases up to 1.6.0 shipped the image in `wireview-hwmon`; pacman moves it
  when both packages are upgraded or installed in one transaction. AUR
  helpers upgrade the split packages already installed, so upgraders add
  this one by hand (`paru -S wireview-hwmon-firmware`).

## Files

| File | Purpose |
|------|---------|
| `PKGBUILD` | Split recipe pulling the GitHub release tarball (sha256-pinned). |
| `.SRCINFO` | Generated metadata (`makepkg --printsrcinfo`); regenerate on every change. |
| `wireview-hwmon.sysusers` | Creates the `wireview` group (installed as `/usr/lib/sysusers.d/wireview-hwmon.conf`). |

## Publish (first time)

The AUR repo is named after the `pkgbase` (`wireview-hwmon`). Requires an AUR
account with your SSH key registered.

```bash
git clone ssh://aur@aur.archlinux.org/wireview-hwmon.git
cp PKGBUILD .SRCINFO wireview-hwmon.sysusers wireview-hwmon/
cd wireview-hwmon
git add PKGBUILD .SRCINFO wireview-hwmon.sysusers
git commit -m "Update to wireview-hwmon 1.5.0"
git push
```

## Update for a new release

```bash
# bump pkgver in PKGBUILD to match ../VERSION (check with: make -C .. check-version)
updpkgsums
makepkg --printsrcinfo > .SRCINFO
makepkg -f            # verify it still builds
git commit -am "wireview-hwmon X.Y.Z" && git push
```

## Install (users)

```bash
paru -S wireview-hwmon wireview-hwmon-dkms wireview-hwmon-firmware
sudo systemctl enable --now wireviewd     # Arch does not auto-enable services
```

DKMS needs the matching kernel headers installed (`linux-headers`,
`linux-lts-headers`, `linux-cachyos-headers`, …) so the module can build.

## Notes

- Validated with `makepkg` + `pacman -U` in an Arch container, and the kernel
  module compiles via `dkms build` against installed kernel headers.
- Immutable distros are not a target for the kernel module (DKMS doesn't fit
  rpm-ostree). Those users run the WireView GUI Flatpak in direct-serial mode.
