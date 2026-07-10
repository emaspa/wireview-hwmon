# Bundled device firmware

`TG-WV-PRO2-FW.hex` is the official Thermal Grizzly WireView Pro II firmware
(currently v05, build TG-WV-PRO2-FW_20260706_1047), taken unmodified from the
upstream WireView2 1.0.7 Windows release. It is installed to
`/usr/share/wireview/TG-WV-PRO2-FW.hex` by `make install` and by all distro
packages (deb/rpm/AUR), and is what `wireviewctl flash` uses when no file
argument is given.

## Release checklist: keep the two copies in sync

The same hex is bundled in two repos:

- this repo: `firmware/TG-WV-PRO2-FW.hex` (headless flashing via wireviewctl)
- wireview-linux: `WireView2/Firmware/TG-WV-PRO2-FW.hex` (in-app flashing)

When TG ships new firmware, update BOTH copies in the same release cycle.
`wireviewctl flash` prints the image version and build string at the confirm
prompt (parsed from the hex: version byte at image offset 194, 32-byte build
string at offset 227), so a stale copy is visible before flashing.
