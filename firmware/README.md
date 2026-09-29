# Bundled device firmware

`TG-WV-PRO2-FW.hex` is the official Thermal Grizzly WireView Pro II firmware
(currently v05, build TG-WV-PRO2-FW_20260706_1047), taken unmodified from the
upstream WireView2 1.0.7 Windows release. It is what `wireviewctl flash` uses
when no file argument is given. It runs on the device, never on the host.

## License

The image is proprietary Thermal Grizzly software. It is not covered by this
repository's GPL, and no license text accompanies it: it may be redistributed
only as Thermal Grizzly permits, and this project grants no further
permission.

To keep it separable from the free code, the distro packages ship it on its
own:

| Packaging | Package | License field |
|-----------|---------|---------------|
| deb (PPA, GitHub releases) | `wireview-hwmon-firmware`, recommended by `wireview-hwmon` | `other` in `debian/copyright` |
| rpm (COPR, GitHub releases) | `wireview-hwmon-firmware`, recommended by `wireview-hwmon` | `LicenseRef-Proprietary` |
| AUR | `wireview-hwmon-firmware`, an optional dependency of `wireview-hwmon` | `LicenseRef-Proprietary` |

Each installs it as `/usr/share/wireview/TG-WV-PRO2-FW.hex`, and so does
`make install`. The daemon, CLI and kernel module work without it;
`wireviewctl flash FILE` flashes an image you supply.

## Release checklist: keep the two copies in sync

The same hex is bundled in two repos:

- this repo: `firmware/TG-WV-PRO2-FW.hex` (headless flashing via wireviewctl)
- wireview-linux: `WireView2/Firmware/TG-WV-PRO2-FW.hex` (in-app flashing)

When TG ships new firmware, update BOTH copies in the same release cycle.
`wireviewctl flash` prints the image version and build string at the confirm
prompt (parsed from the hex: version byte at image offset 194, 32-byte build
string at offset 227), so a stale copy is visible before flashing.
