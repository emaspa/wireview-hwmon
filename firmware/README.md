# Bundled device firmware

`TG-WV-PRO2-FW.hex` is the official Thermal Grizzly WireView Pro II firmware
(currently v05, build TG-WV-PRO2-FW_20260902_0741; sha256
`1431acd2ba06de2337f2c49a2015f124406a3ea3df4a78e6124436bd58341cfa`), taken
unmodified from the upstream WireView2 1.0.8 Windows release. It is what
`wireviewctl flash` uses when no file argument is given. It runs on the
device, never on the host.

One image serves both editions: the WireView Pro II (vendor and product id
`EF05`) and the WireView Pro II Noctua Edition (`EF06`). The image carries
product `EF05`; a Noctua Edition device takes it through the same alias the
upstream app uses. Builds before 2026-09-02 do not know the Noctua Edition.

## License

The image is proprietary Thermal Grizzly software. It is not covered by this
repository's GPL, and no license text accompanies it. Thermal Grizzly is aware
of this project; the image is redistributed unmodified on that basis and will
be removed from the repository and the packages if they ask. This project
grants no further permission.

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
`wireviewctl flash` reads the image's BuildStruct at image offset 192 (vendor
id, product id, version byte, 32-byte product name, 32-byte build string) and
prints it next to the device's firmware before the confirm prompt; it refuses
the build the device already runs and older ones, so a stale copy is visible,
and refused, before flashing.
