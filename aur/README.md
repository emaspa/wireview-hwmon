# AUR packaging

A split AUR package (one `pkgbase`, two installable packages) for Arch and
Arch-based distros (CachyOS, EndeavourOS):

- **`wireview-hwmon`** — the `wireviewd` daemon, `wireviewctl` CLI, systemd unit
  and udev rule (built from source — small C programs).
- **`wireview-hwmon-dkms`** — the kernel module, built on the user's machine via
  DKMS. Arch's `dkms` pacman hooks build/install it automatically on install and
  rebuild it on kernel upgrades, so no custom `.install` is needed.

## Files

| File | Purpose |
|------|---------|
| `PKGBUILD` | Split recipe pulling the GitHub release tarball (sha256-pinned). |
| `.SRCINFO` | Generated metadata (`makepkg --printsrcinfo`); regenerate on every change. |

## Publish (first time)

The AUR repo is named after the `pkgbase` (`wireview-hwmon`). Requires an AUR
account with your SSH key registered.

```bash
git clone ssh://aur@aur.archlinux.org/wireview-hwmon.git
cp PKGBUILD .SRCINFO wireview-hwmon/
cd wireview-hwmon
git add PKGBUILD .SRCINFO
git commit -m "Initial import: wireview-hwmon 1.3.2"
git push
```

## Update for a new release

```bash
# bump pkgver in PKGBUILD (and ensure dkms.conf's PACKAGE_VERSION matches)
updpkgsums
makepkg --printsrcinfo > .SRCINFO
makepkg -f            # verify it still builds
git commit -am "wireview-hwmon X.Y.Z" && git push
```

## Install (users)

```bash
paru -S wireview-hwmon wireview-hwmon-dkms
sudo systemctl enable --now wireviewd     # Arch does not auto-enable services
```

DKMS needs the matching kernel headers installed (`linux-headers`,
`linux-lts-headers`, `linux-cachyos-headers`, …) so the module can build.

## Notes

- Validated with `makepkg` + `pacman -U` in an Arch container, and the kernel
  module compiles via `dkms build` against installed kernel headers.
- Immutable distros are not a target for the kernel module (DKMS doesn't fit
  rpm-ostree). Those users run the WireView GUI Flatpak in direct-serial mode.
