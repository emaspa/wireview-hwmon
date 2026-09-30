# wireview-hwmon

Linux hwmon driver and daemon for the [Thermal Grizzly WireView Pro II](https://www.thermal-grizzly.com/en/wireview-pro-ii-gpu/s-tg-wv-p2) power monitor. Exposes voltage, current, power, and temperature sensor data through the standard Linux hwmon subsystem.

Both editions of the WireView Pro II are supported: the original and the WireView Pro II Noctua Edition, which speaks the same protocol. The WireView II and its Phanteks Edition are different devices and are not supported yet; the daemon says so once in its log and does not drive them.

Works standalone or alongside the [wireview-linux](https://github.com/emaspa/wireview-linux) GUI application. When both are used together, the app reads sensor data from hwmon and sends commands through the daemon's Unix socket - giving you full app functionality plus system-wide sensor integration.

## How it works

```
WireView Pro II (USB) → wireviewd (serial) → kernel module → /sys/class/hwmon/ → monitoring tools
                                 └─────────→ HTTP /sensors, /metrics (opt-in) → LAN: other hosts, the GUI app, wireviewctl, Prometheus
```

- **wireview_hwmon.ko** - Kernel module that creates a virtual hwmon device
- **wireviewd** - Userspace daemon that reads the device over serial, feeds the kernel module, and (opt-in) publishes readings + accepts authenticated commands over the LAN. It tells the two editions apart by the product id the device reports (`EF05` WireView Pro II, `EF06` Noctua Edition) and passes it on to clients
- **wireviewctl** - CLI tool for querying sensors, sending commands, firmware flashing (`flash`), and a `top` live monitor

## Installation

### Ubuntu 24.04 / 26.04 (PPA)

```bash
sudo add-apt-repository ppa:sparvoli/wireview-hwmon
sudo apt update
sudo apt install wireview-hwmon wireview-hwmon-dkms
```

This installs the daemon, CLI tool, kernel module (via DKMS), systemd service, and udev rules. The module auto-rebuilds on kernel updates and the daemon starts automatically.

### Ubuntu / Debian (.deb packages)

Pre-built `.deb` packages are available on the [Releases](https://github.com/emaspa/wireview-hwmon/releases) page. Download both and install from the download directory:

```bash
sudo apt install ./wireview-hwmon_*_amd64.deb ./wireview-hwmon-dkms_*_all.deb
```

### Fedora (COPR)

```bash
sudo dnf copr enable emaspa/wireview-linux
sudo dnf install wireview-hwmon wireview-hwmon-dkms
sudo systemctl enable --now wireviewd
```

The same COPR repo also provides the [WireView GUI](https://github.com/emaspa/wireview-linux) (`wireview-linux`). The DKMS module pulls `kernel-devel` for your running kernel and rebuilds automatically on kernel updates.

### Fedora (.rpm packages)

Pre-built `.rpm` packages are on the [Releases](https://github.com/emaspa/wireview-hwmon/releases) page (one set works on Fedora 43-44):

```bash
sudo dnf install ./wireview-hwmon-*.x86_64.rpm ./wireview-hwmon-dkms-*.noarch.rpm
sudo systemctl enable --now wireviewd
```

### Arch / CachyOS / EndeavourOS (AUR)

```bash
paru -S wireview-hwmon wireview-hwmon-dkms   # or: yay -S
sudo modprobe wireview_hwmon
sudo systemctl enable --now wireviewd
```

Verify with `sensors` (or `wireviewctl info`); a `wireview`-named hwmon device should appear. From the next boot onward everything comes up automatically: the dkms package registers the module in `modules-load.d` and the service also modprobes it on start.

DKMS needs the matching kernel headers (`linux-headers`, `linux-cachyos-headers`, …) installed; the module then rebuilds automatically on kernel updates.

> **Immutable / atomic distros** (Bazzite, Silverblue, Kinoite) are not supported for the kernel module - DKMS doesn't fit rpm-ostree. On those, run the [WireView GUI Flatpak](https://github.com/emaspa/wireview-linux) in direct-serial mode, which doesn't need this module.

### Firmware image

`wireviewctl flash` with no file argument flashes Thermal Grizzly's official
firmware image from `/usr/share/wireview/TG-WV-PRO2-FW.hex`: v05, build
`TG-WV-PRO2-FW_20260902_0741`, from the upstream WireView2 1.0.8 Windows
release. The one image serves both the WireView Pro II and the Noctua Edition.
It comes with the `wireview-hwmon` package and with `make install`. The image
is proprietary and not covered by the GPL, see
[firmware/README.md](firmware/README.md).

### Build from source

#### Requirements

- Linux 6.8 or newer with kernel headers (`linux-headers-$(uname -r)`); older kernels are not supported
- A Thermal Grizzly WireView Pro II device connected via USB
- `gcc` and `make`

## Build

```bash
git clone https://github.com/emaspa/wireview-hwmon.git
cd wireview-hwmon
make
```

This builds the kernel module (`wireview_hwmon.ko`), the daemon (`wireviewd`), and the CLI tool (`wireviewctl`).

## Quick start

```bash
# Install udev rules (serial port and DFU bootloader access)
sudo cp 99-wireview-hwmon.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger   # or replug the device: new permissions apply on the next device event

# Load the kernel module
sudo insmod wireview_hwmon.ko

# Run the daemon
sudo ./wireviewd

# In another terminal, check sensor data
sensors wireview-isa-0000
```

## Install (persistent)

```bash
sudo make install  # installs module, daemon, udev rules, and systemd service

# Auto-load module on boot
echo wireview_hwmon | sudo tee /etc/modules-load.d/wireview-hwmon.conf

# Enable and start the daemon service
sudo systemctl enable --now wireviewd

# Load the module now (or reboot)
sudo modprobe wireview_hwmon

# Verify
sensors wireview-isa-0000
```

After rebooting, both the module and daemon will start automatically.

### Upgrading a source install

`make install` puts the udev rules and the systemd unit in `/etc/udev/rules.d/`
and `/etc/systemd/system/`, and those copies override the packaged ones under
`/usr/lib`. After pulling a new version, re-run `sudo make install`,
then `sudo systemctl restart wireviewd` and replug the device (or run
`sudo udevadm trigger`). If you have switched to a distro package, delete the
old copies instead:

```bash
sudo rm /etc/udev/rules.d/99-wireview-hwmon.rules /etc/systemd/system/wireviewd.service
sudo udevadm control --reload-rules && sudo systemctl daemon-reload
```

### Secure Boot

If Secure Boot is enabled, the kernel will refuse to load unsigned modules (`Key was rejected by service`). You have two options:

**Option 1: Disable Secure Boot** (easiest)

Reboot, enter BIOS/UEFI settings, disable Secure Boot, then boot back in and load the module normally.

**Option 2: Sign the module** (keeps Secure Boot enabled)

```bash
# Generate a signing key (one-time)
sudo mkdir -p /var/lib/shim-signed/mok
sudo openssl req -new -x509 -newkey rsa:2048 \
  -keyout /var/lib/shim-signed/mok/MOK.priv \
  -outform DER -out /var/lib/shim-signed/mok/MOK.der \
  -nodes -days 36500 -subj "/CN=Local Module Signing/"

# Enroll the key (requires reboot to confirm in MokManager)
sudo mokutil --import /var/lib/shim-signed/mok/MOK.der

# Reboot - MokManager will prompt you to enroll the key

# After reboot, sign the module
sudo /usr/src/linux-headers-$(uname -r)/scripts/sign-file sha256 \
  /var/lib/shim-signed/mok/MOK.priv \
  /var/lib/shim-signed/mok/MOK.der \
  /lib/modules/$(uname -r)/updates/wireview_hwmon.ko

# Now it loads
sudo modprobe wireview_hwmon
```

You only need to generate and enroll the key once. After a kernel update, re-sign the module or rebuild with `sudo make install` and sign again.

## Uninstall

```bash
sudo make uninstall
```

## Daemon options

```
wireviewd [-i interval_ms] [-d /dev/ttyACMx] [-V]

  -i  Poll interval in milliseconds (default: 1000)
  -d  Serial device path (default: auto-detect)
  -V  Print the version and exit
```

### Service sandboxing

The systemd unit runs `wireviewd` as root but sandboxed and with no
capabilities. In practice:

- It can write only to `/run` (the command socket) and `/var/log/wireview`;
  audit logs are created mode `600`. `/etc/wireview` is read-only to it.
- A `port=` below 1024 in `/etc/wireview/config` fails to bind, since that
  needs `CAP_NET_BIND_SERVICE` and the unit drops all capabilities.
- The device policy allows only CDC-ACM serial ports and `/dev/wireview-hwmon`,
  so `-d` accepts only `/dev/ttyACM*` devices.

The unit deliberately does not use a private user namespace: the daemon
identifies command-socket clients by their real user id to check `wireview`
group membership.

Running `./wireviewd` by hand, as in the Quick start, is not sandboxed.

## Exposed sensors

| Sensor | hwmon attribute | Unit |
|--------|----------------|------|
| Pin 1-6 Voltage | `in0_input` - `in5_input` | millivolts |
| Average Voltage | `in6_input` | millivolts |
| Supply Voltage (Vdd) | `in7_input` | millivolts |
| Pin 1-6 Current | `curr1_input` - `curr6_input` | milliamps |
| Total Current | `curr7_input` | milliamps |
| Total Power | `power1_input` | microwatts |
| Pin 1-6 Power | `power2_input` - `power7_input` | microwatts |
| Onboard Temp In | `temp1_input` | millidegrees C |
| Onboard Temp Out | `temp2_input` | millidegrees C |
| External Temp 1 | `temp3_input` | millidegrees C |
| External Temp 2 | `temp4_input` | millidegrees C |
| Total Energy | `energy1_input` | microjoules since wireviewd started |
| PSU Capability | `power1_cap` | microwatts (600/450/300/150 W; no data if unknown) |
| Fan Duty | `pwm1` | 0-255 |
| Per-channel alarms | `temp1_alarm` - `temp4_alarm`, `curr1_alarm` - `curr7_alarm`, `power1_alarm` | 0/1 |
| Fault Status | `intrusion0_alarm` (`intrusion0_label`) | 0/1 |
| Fault Log | `intrusion1_alarm` (`intrusion1_label`) | 0/1 |
| Fault Status (raw) | `fault_status_raw` | bitmask |
| Fault Log (raw) | `fault_log_raw` | bitmask |

All voltage, current, power, temperature, and energy channels also expose `_label` attributes for tool-friendly names.

The alarms follow the active fault bits (`fault_status`, debounced by the
daemon), not the fault log:

| Bit | Fault | Alarms |
|-----|-------|--------|
| 0 | Chip over-temperature | `temp1_alarm`, `temp2_alarm` |
| 1 | Sensor over-temperature | `temp3_alarm`, `temp4_alarm` |
| 2 | Over-current (total) | `curr7_alarm` |
| 3 | Wire over-current | `curr1_alarm` - `curr6_alarm` |
| 4 | Over-power | `power1_alarm` |
| 5 | Current imbalance | `curr1_alarm` - `curr7_alarm` |

The device does not say which wire tripped bit 3, so all six per-pin alarms
report it.

### Energy

The device reports power, not energy, so `wireviewd` integrates it: each
accepted frame adds the total power times the time since the previous frame.
The counter survives device reconnects and resets when the daemon restarts. A
gap longer than 5 s (or three poll intervals, if `-i` makes that longer), such
as an unplug, a serial handover or a system suspend, is not counted. It is
published as `energy1_input`, as `"energyJ"` in `GET /sensors` and as
`wireview_energy_joules_total` on [`GET /metrics`](#prometheus-get-metrics).

Module and daemon must both be this release or newer. An older module has no
`energy1_input` (the daemon notices and keeps feeding it the old record), and
with an older daemon the new module reports no data (`ENODATA`) for it.

## Example `sensors` output

```
wireview-isa-0000
Adapter: ISA adapter
Pin 1:         12.12 V
Pin 2:         12.13 V
Pin 3:         12.11 V
Pin 4:         12.12 V
Pin 5:         12.11 V
Pin 6:         12.12 V
Average:       12.12 V
Vdd:            3.30 V
Onboard In:    +45.3°C
Onboard Out:   +42.1°C
External 1:    +38.7°C
External 2:        N/A
Total:        389.36 W  (cap = 600.00 W)
Pin 1:         63.39 W
Pin 2:         66.11 W
Pin 3:         62.00 W
Pin 4:         64.72 W
Pin 5:         67.33 W
Pin 6:         65.81 W
Total:          1.37 MJ
Pin 1:          5.23 A
Pin 2:          5.45 A
Pin 3:          5.12 A
Pin 4:          5.34 A
Pin 5:          5.56 A
Pin 6:          5.43 A
Total:         32.13 A
pwm1:              75%
Fault Status: OK
Fault Log:    ALARM
```

`sensors` prints `pwm1` as a percentage (191 of 255 shows as 75%). A channel
whose alarm is set gets `ALARM` at the end of its line.

## CLI tool

`wireviewctl` lets you query sensor data and send device commands from the terminal or scripts.

```
Usage: wireviewctl [--host H[:port] [--secret-file FILE]] <command> [args]

Global options:
  --host H[:port]   Talk to wireviewd on H over HTTP (default port 9876)
                    instead of the local socket: info, build, sensors and
                    read-config read; screen, nvm, clear-faults and
                    write-config are signed with the shared secret.
                    bootloader and flash are local only.
  --secret-file FILE  Shared secret for signed writes: the passphrase, or a
                    file with a secret= line (like /etc/wireview/config).
                    Without it, $WIREVIEW_SECRET is used.

Commands (require wireviewd running):
  info              Show device firmware, UID, build, product and edition
  clear-faults [STATUS_MASK [LOG_MASK]]
                    Clear faults. Masks are hex bits to clear (default FFFF,
                    i.e. all active faults and the whole fault log)
  read-config       Read device config (hex to stdout)
  write-config FILE Write device config (hex from file)
  screen CMD        Change display (main|simple|current|temp|status|same|pause|resume)
  nvm CMD           NVM operation (load|store|reset|load-cal|store-cal|load-cal-factory|store-cal-factory)
  build             Show firmware build string
  bootloader        Enter DFU bootloader mode
  flash [FILE] [-y] [--force]
                    Flash firmware (.hex or .bin) via DFU (needs dfu-util;
                    works without the daemon if the bootloader is already up).
                    Without FILE, flashes the bundled image at
                    /usr/share/wireview/TG-WV-PRO2-FW.hex
                    Refuses an image for another product, the build the
                    device runs, an older build, and on a Noctua Edition
                    a build before 2026-09-02; --force overrides that.
                    -y only skips the confirmation prompt.

Commands (require wireview_hwmon module):
  sensors [--json]  Show all sensor readings from hwmon sysfs; --json prints
                    the same schema as the daemon's GET /sensors

Monitor:
  top [--host H[:port][,H2...]]... [--interval MS]
                    Live dashboard: the local device plus remote hosts.
                    --host repeats and/or takes a comma/space list; hosts are
                    also read from /etc/wireview/hosts, and the global
                    --host (before "top") adds one more. Press q to quit.

Other:
  -V, --version     Print the wireviewctl version
```

`info` ends with the device's product and edition:

```
product: EF06
edition: WireView Pro II Noctua Edition
```

A daemon that does not report the product yet (wireview-hwmon 1.6.0 and
earlier) only runs Pro II devices, so `info` prints `product: EF05 (assumed:
this wireviewd does not report it)` and `edition: WireView Pro II` for it.
`top` and `sensors --json` show the edition name the same way.

`clear-faults` with no arguments clears every active fault and the whole fault
log (masks `FFFF FFFF`). `STATUS_MASK` and `LOG_MASK` are 16-bit hex bitmasks,
with or without `0x`: a set bit clears that fault, and a missing mask defaults
to `FFFF`. So `wireviewctl clear-faults 0 FFFF` clears only the log, and
`wireviewctl clear-faults 0x0004` clears status bit 2 plus the whole log.
(On the wire the firmware takes the inverse, a keep-mask; `wireviewctl` does
the inversion, the raw socket and HTTP `clearFaults` do not.)

`flash` checks the whole image before it runs `dfu-util` or asks the daemon
for the bootloader. In an Intel HEX file every record must hold only hex
digits, exactly the bytes its count declares and a correct checksum; the file
must end with an end-of-file record (type 01); data must fall in the 4 MiB
flash window from `0x08000000`, and no address may be given twice. The error
names the file and line (`wireviewctl: fw.hex: line 812: bad checksum`) and
nothing reaches the device. A `.bin` has no checksums to check: it is loaded
at `0x08000000` as is.

Then `flash` compares the image with the device, before it asks anything of
the device or the bootloader. It reads the image's BuildStruct (vendor and
product id, firmware version, product name and build string, at image offset
192) and asks the daemon for the device's, and prints both with a verdict:

```
device:  WireView Pro II (EF05, assumed), firmware v05, build TG-WV-PRO2-FW_20260706_1047
image:   Thermal Grizzly WireView Pro II (EF05), firmware v05, build TG-WV-PRO2-FW_20260902_0741
verdict: newer: the image is newer than the device firmware
```

- **Product**: the image vendor must be the device's, and the image product
  the device's product after the upstream alias table, which maps the Noctua
  Edition (`EF06`) to the Pro II image (`EF05`). An image for another product,
  or one too short to carry the ids, is refused.
- **Noctua Edition**: images built before 2026-09-02 do not know that
  edition, so a Noctua Edition device refuses them, and any image whose build
  string has no date.
- **Build**: the firmware version byte decides first; with the same version,
  the build time in the build string (`..._yyyyMMdd_HHmm`, as upstream reads
  it). An older image is refused, and so is the build the device already
  runs. A newer image goes on to the usual confirmation. When either build
  string has no date, the verdict is `unknown`: `flash` says which one and
  asks as usual, with a warning.

`--force` flashes despite a refusal. `-y` only skips the confirmation prompt
and never overrides a gate, so `wireviewctl flash -y` stays a safe headless
update: it flashes a newer build and refuses the rest. When the daemon is not
running or has no device (for example when the bootloader is already up),
`flash` says that it cannot verify the image against the device and relies on
the confirmation or `-y`, as before. A daemon that does not report the product
(wireview-hwmon 1.6.0 and earlier) only runs Pro II devices, so the device
counts as `EF05` there.

### Permissions: the `wireview` group

Commands that can change or brick the device are accepted only from root or
members of the `wireview` system group: `bootloader`, `flash`, `nvm` and
`write-config` (and, for other clients such as the GUI app, the serial
handover used for log reads, theme uploads and in-app flashing). Everything
else (`info`, `sensors`, `top`, `read-config`, `screen`, `clear-faults`,
`build`) works for any user.

The deb, rpm and AUR packages and `make install` create the group. Add
yourself; it takes effect on the next connection, no re-login needed:

```bash
sudo usermod -aG wireview $USER
```

`flash` needs the group only to ask the daemon to enter the bootloader. The
flashing itself runs `dfu-util` as you, which needs the seat or `dialout`
access described below; with the device already in DFU mode, that is all it
needs.

### Device access (udev)

The udev rules give the serial port (`0483:5740`) and the STM32 DFU bootloader
(`0483:df11`, the device after `wireviewctl bootloader`) mode `660`, group
`dialout`, plus an ACL for the user logged in at the local seat (`uaccess`). In
a local desktop session the GUI app's direct-serial mode, `wireviewctl flash`
and `dfu-util` therefore work without sudo. SSH and other remote sessions get
no seat ACL; join `dialout` and log in again:

```bash
sudo usermod -aG dialout $USER
```

`/dev/wireview-hwmon`, the node wireviewd feeds readings into, is root-only
(`600`) on purpose: anything that can write to it can inject fake readings into
hwmon, and wireviewd is its only writer. Monitoring tools read
`/sys/class/hwmon/`, which stays world-readable.

The rules also tell ModemManager to ignore the device. Without that it probes
every new CDC-ACM port as a possible modem for about half a minute, which
keeps the port busy and delays `wireviewd` after a plug-in or a restart.

### Examples

```bash
# Show device info: firmware, UID, build, product and edition
wireviewctl info

# Read all sensors (scriptable key: value format)
wireviewctl sensors

# Live dashboard of this host and remotes (q to quit) - see "LAN monitoring" below
wireviewctl top --host 192.168.1.50

# Switch to simple display
wireviewctl screen simple

# Update the device firmware to the bundled image (no download needed);
# add -y to skip the confirmation prompt for headless updates. The same or
# an older build, or an image for another product, is refused (--force
# overrides).
# Unofficial tool, not affiliated with Thermal Grizzly: flash at your own
# risk. A power loss mid-flash can leave the device unbootable until
# reflashed manually.
wireviewctl flash

# Clear only the fault log, keep active faults
wireviewctl clear-faults 0 FFFF

# Back up and restore config (write-config needs the wireview group)
wireviewctl read-config > config.hex
wireviewctl write-config config.hex

# Store config to NVM
wireviewctl nvm store

# Use in scripts
POWER=$(wireviewctl sensors | grep total_power_uw | cut -d' ' -f2)
echo "Total power: $((POWER / 1000000)) W"

# Same readings as JSON, for jq and friends
wireviewctl sensors --json | jq '.devices[0].sumPowerW'

# Switch the display of a WireView on another machine
wireviewctl --host 192.168.1.50 --secret-file ~/.config/wireview-secret screen simple
```

### `wireviewctl sensors` output

`wireviewctl sensors` prints one `key: value` line per reading, in sysfs units
(`_mv`, `_ma`, `_uw`, `_mc`), followed by:

```
fan_duty: 75
fault_status: 0
fault_log: 4
psu_cap: 600W
energy_uj: 1372480512000
alarm_temp_onboard_in: 0
...
alarm_total_power: 0
```

- `fan_duty` is in % (0-100), from `pwm1`, or from `fan1_input` on an older
  module.
- `psu_cap` is from `power1_cap`, or the `psu_cap` enum on an older module;
  `unknown` when the device reports no PSU capability.
- `energy_uj` appears when the module has `energy1_input`.
- The `alarm_<name>` lines, one per [alarm attribute](#exposed-sensors), are
  named after the channel: `alarm_temp_onboard_in`, `alarm_temp_onboard_out`,
  `alarm_temp_external1`, `alarm_temp_external2`, `alarm_pin1_current` -
  `alarm_pin6_current`, `alarm_total_current` and `alarm_total_power`.

`wireviewctl sensors --json` prints the document `GET /sensors` serves
(pretty-printed here):

```json
{"host": "gpu-box", "appVersion": "wireviewctl", "devices": [{
  "id": "0032001F3133510B37363235", "name": "WireView Pro II", "connected": true,
  "hwRev": "EF05", "fwVer": "5", "buildString": "TG-WV-PRO2-FW_20260902_0741",
  "timestamp": "2026-09-29T14:22:07Z",
  "pinVoltage": [12.120, 12.130, 12.110, 12.120, 12.110, 12.120],
  "pinCurrent": [5.230, 5.450, 5.120, 5.340, 5.560, 5.430],
  "tempInC": 45.3, "tempOutC": 42.1, "ext1C": 38.7, "ext2C": 0.0,
  "psuCapW": 600, "fan": 75, "faultStatus": 0, "faultLog": 4,
  "sumCurrentA": 32.130, "sumPowerW": 389.363, "energyJ": 1372480.512}]}
```

`id`, `fwVer` and `buildString` come from the daemon and are `""` when it is
not running. So do `name` and `hwRev` (`"EF06"`, `"WireView Pro II Noctua
Edition"`) when the daemon reports the device's product; otherwise `name` is
`"WireView Pro II"` and `hwRev` is `""`, as an older daemon writes them. A
disconnected temperature sensor reads `0.0`, `psuCapW` is `0` when unknown,
and `energyJ` is left out when the module has no `energy1_input`. `connected` is `false` when the module's readings are stale.
Without the module the command prints `{"devices":[]}` (with `host` and
`appVersion`) and exits 1.

`WIREVIEW_HWMON_PATH=DIR` makes `sensors` and `top` read DIR (a directory with
a `name` file and the attribute files) instead of searching
`/sys/class/hwmon`, for testing without the module.

### Remote hosts (`--host`)

With `--host H[:port]` before the command, `wireviewctl` talks to `wireviewd`
on H over HTTP instead of the local socket (port 9876 by default; write IPv6
addresses as `[addr]:port`). That daemon needs its
[LAN listener](#lan-monitoring-remote-access) enabled.

- `info`, `build`, `sensors [--json]` and `read-config` read `GET /sensors` and
  `GET /config`, which need no secret. `sensors --json` passes the daemon's
  document through unchanged.
- `GET /sensors` has fewer fields than the local sysfs. It has no average or
  Vdd voltage and no per-channel alarms, so remote `sensors` prints no
  `avg_voltage_mv`, `vdd_mv` or `alarm_*` lines, and remote `info` has no
  `config_version` line. Remote `info` takes the product and edition from
  `hwRev` and `name`. Any other field the document lacks is left out too
  (`top` shows it as `--`), never printed as `0`: `psu_cap: unknown` means the
  device reported `psuCapW: 0`. When the document says `"connected": false`,
  `sensors` prints a note on stderr that the readings may be stale.
- `screen`, `nvm`, `clear-faults` and `write-config` go out as a signed
  `POST /command`. The secret comes from `--secret-file FILE` (the passphrase
  alone, or a file with a `secret=` line, such as `/etc/wireview/config`) or
  from `$WIREVIEW_SECRET`. There is no `--secret` option on purpose: a secret on
  the command line shows up in `ps`. Over HTTP the signature is the permission
  check; the `wireview` group does not apply.
- `bootloader` and `flash` refuse: the daemon never exposes the bootloader
  over the network.
- `wireviewctl --host H top` adds H to the `top` view, like `top --host H`.

A non-2xx reply prints the HTTP status and the daemon's error, and the command
exits 1.

## Daemon socket

The daemon listens on a Unix socket at `/run/wireviewd.sock` that any local user can connect to, allowing external programs (including the [wireview-linux](https://github.com/emaspa/wireview-linux) app) to send commands to the device without direct serial access. Commands marked * are privileged: the daemon checks the peer's credentials and accepts them only from root or members of the [`wireview` group](#permissions-the-wireview-group); anyone else gets status 3 (denied). Supported commands:

| Command | Description |
|---------|-------------|
| GET_DEVICE_INFO | Query firmware version, config version, UID, build string, vendor and product id |
| CLEAR_FAULTS | Clear fault status and/or fault log (payload: status keep-mask, log keep-mask; u16 LE each, `fault &= mask`, so 0 clears all and a set bit keeps that fault) |
| READ_CONFIG | Read the device configuration |
| WRITE_CONFIG * | Write a new device configuration |
| SCREEN_CMD | Send a screen command (change display page) |
| NVM_CMD * | Send an NVM command (store/recall configuration) |
| READ_BUILD | Read the firmware build string |
| ENTER_BOOTLOADER * | Restart the device into DFU bootloader mode |
| SUSPEND_SERIAL * | Pause daemon polling and release the serial port for a client (1-300 s, re-armable) |
| RESUME_SERIAL * | End a serial handover early and resume polling |

The socket uses a binary protocol: request `[type:u8][len:u16 LE][payload]`, response `[status:u8][len:u16 LE][payload]`. Status is 0 (ok), 1 (error), 2 (device not connected) or 3 (denied: privileged command from a peer that is not root or in the `wireview` group).

The GET_DEVICE_INFO payload is:

| Bytes | Field |
|-------|-------|
| 1 | firmware version |
| 1 | config version |
| 12 | UID |
| n + 1 | build string, NUL-terminated (at most 32 characters) |
| 1 | vendor id (`0xEF`, Thermal Grizzly) |
| 1 | product id (`0x05` WireView Pro II, `0x06` WireView Pro II Noctua Edition) |

The vendor and product id came in with wireview-hwmon 1.7.0. They follow the
build string's NUL, so a client that reads the build string up to the first
NUL, as the GUI up to 1.2.5.0 and `wireviewctl` up to 1.6.0 do, sees no
change. A reply that ends at the NUL comes from an older daemon, which only
ever attached a WireView Pro II: read it as vendor `0xEF`, product `0x05`.

The socket exists for the daemon's whole life, device or not. Clients stay
connected across an unplug and replug; while no device is present every
command gets status 2 (a privileged command from an unprivileged peer still
gets 3), and the same connection works again once the device is back.

Up to 8 clients can be connected at once. A client that sends no request for
60 seconds (counted from the connection, then from its last complete request)
is disconnected, and when all 8 slots are taken a new connection replaces the
client that has been idle longest. A client should therefore expect the daemon
to close a connection it left idle, and reconnect and retry when that happens:
the GUI does this from wireview-linux 1.2.5.0. A request in progress is not
idle, but it must arrive in full within 2 seconds of its first byte or the
client is disconnected. The client that requested a SUSPEND_SERIAL handover is
neither closed as idle nor replaced until the handover ends, since the GUI
renews it only once a minute. A new connection is refused (status 1, then
closed) only while every connected client is mid-request or holds the
handover. Idle disconnects and replacements are logged in the
[audit log](#audit-log) with the client's uid.

## Notes

- When used with the wireview-linux app, the daemon handles the serial port and the app communicates through hwmon (sensors) and the daemon socket (commands). Both can run simultaneously.
- When used standalone (without the app), the daemon owns the serial port exclusively.
- If the device is disconnected, the daemon will wait and reconnect automatically. The command socket and the LAN listener keep serving meanwhile.
- Sensor readings become stale (report N/A) if no data is received for 5 seconds.

## LAN monitoring (remote access)

The daemon can publish its readings over the LAN and accept authenticated
commands, so you can monitor and control WireViews on other machines - from the
[GUI app](https://github.com/emaspa/wireview-linux), from `wireviewctl top` and
`wireviewctl --host`, from Prometheus, or from any HTTP client. **It is off by
default**: no port is opened unless you enable it.

### Endpoints (port 9876 by default)

- **`GET /sensors`** - JSON snapshot of the device (per-pin V/I, power, temps,
  faults, fan duty, PSU cap, energy, firmware build; see the example under
  [`wireviewctl sensors` output](#wireviewctl-sensors-output)). Open, read-only;
  the same endpoint the GUI app and `wireviewctl top` consume. With no device,
  `devices` is empty. `name` is the edition (`"WireView Pro II"` or
  `"WireView Pro II Noctua Edition"`) and `hwRev` its vendor and product id in
  uppercase hex, as Thermal Grizzly's own client writes them (`"EF05"`,
  `"EF06"`); daemons before 1.7.0 sent `"WireView Pro II"` and `""`.
- **`GET /metrics`** - the same readings for Prometheus (see
  [below](#prometheus-get-metrics)). Open, read-only.
- **`GET /config`** - the device's current configuration, so a remote editor can
  load it.
- **`POST /command`** - write commands (screen / NVM / clear-faults / writeConfig).
  **Authenticated** (see below); the firmware bootloader is never reachable
  over the network.

### Enabling it

Edit `/etc/wireview/config` (a documented reference with the defaults is installed by
the deb, rpm and AUR packages and by `make install`; upgrades keep your edits):

```ini
# open the listener (default 0 = off)
remote_enabled=1
# listener port (default)
port=9876
# listen address (default: all, IPv4 and IPv6)
#bind=127.0.0.1
# shared secret for authenticated remote writes (empty = reads only)
secret=your-passphrase
# audit-log retention in days
log_days=14
```

Comments go on their own line: text after a value is part of the value.

Then `sudo systemctl restart wireviewd`. Reads stay open; **writes require the secret**.

`bind=` takes a numeric IPv4 or IPv6 address (`127.0.0.1` for a local
Prometheus only, `::1`, `192.168.1.10`). Unset, the listener takes every
address with one dual-stack socket, or IPv4 only on hosts without IPv6. A value
that is not a numeric address (a typo, a host name) disables the listener
rather than opening it on every address; the journal says why.

### Prometheus (`GET /metrics`)

`GET /metrics` serves the Prometheus text format (0.0.4) from the listener's
port. It is read-only and, like `/sensors`, not logged. Every sample carries
`device="<UID>"`, the uppercase hex UID that `/sensors` reports as `id`.

| Metric | Type | Other labels | Value |
|--------|------|--------------|-------|
| `wireview_up` | gauge | | 1 while the device is connected and reporting |
| `wireview_pin_voltage_volts` | gauge | `pin="1"` - `"6"` | Voltage per pin |
| `wireview_pin_current_amps` | gauge | `pin` | Current per pin |
| `wireview_pin_power_watts` | gauge | `pin` | Power per pin |
| `wireview_power_watts` | gauge | | Total power, sum of the pins |
| `wireview_current_amps` | gauge | | Total current |
| `wireview_voltage_average_volts` | gauge | | Average pin voltage |
| `wireview_vdd_volts` | gauge | | Device supply voltage |
| `wireview_temperature_celsius` | gauge | `sensor="onboard_in"`, `"onboard_out"`, `"external_1"`, `"external_2"` | Temperature; disconnected sensors are left out |
| `wireview_fan_duty_ratio` | gauge | | Fan duty, 0 to 1 |
| `wireview_psu_cap_watts` | gauge | | PSU capability, 0 = unknown |
| `wireview_fault_status` | gauge | | Active fault bitmask (debounced) |
| `wireview_fault_log` | gauge | | Latched fault bitmask (debounced) |
| `wireview_fault_active` | gauge | `fault="chip_over_temp"`, `"sensor_over_temp"`, `"over_current"`, `"wire_over_current"`, `"over_power"`, `"current_imbalance"` | 1 while that fault (bits 0-5) is active |
| `wireview_energy_joules_total` | counter | | Energy since `wireviewd` started |
| `wireview_firmware_info` | gauge | `version`, `build`, `product` (`"EF05"`, `"EF06"`), `edition` | Always 1 |

Without a device only `wireview_up 0` is served, labelled with the last device
seen (unlabelled before the first one), so an `up == 0` alert stays on the same
series across a disconnect. A minimal scrape config:

```yaml
scrape_configs:
  - job_name: wireview
    static_configs:
      - targets: ['192.168.1.50:9876']
```

### mDNS discovery (optional)

`make install` also drops `avahi-wireview.service` into `/etc/avahi/services/`,
so avahi-daemon advertises the listener as `_wireview._tcp` and the GUI app
finds the host without typing an address. The packages don't ship it (the
listener is off by default). If you installed a package and enabled the
listener, copy it from the source tree:

```bash
sudo install -Dm644 avahi-wireview.service /etc/avahi/services/wireview.service
```

The file advertises port 9876; edit its `<port>` if you changed `port=`.

### Security model

- **Opt-in** - nothing is exposed unless `remote_enabled=1`; `bind=` limits
  the listener to one address.
- **Writes are signed with HMAC-SHA256.** Each `POST /command` carries a
  timestamp, nonce, and signature over the body using the shared `secret`, so
  the secret never travels on the wire and replays are rejected (±30 s window).
  An empty secret disables writes (the daemon answers `403`).
- **Flood protection** - request-size cap and bounded handling.
- **Audit log** (below). No TLS: this is built for a trusted LAN - the HMAC keeps
  the secret off the wire, but anyone on the LAN can read `/sensors` and
  `/metrics` while the listener is on.

### `wireviewctl top` - live monitor

A `btop`-style terminal dashboard of every WireView, local and remote:

```bash
wireviewctl top                                  # local device only
wireviewctl top --host 192.168.1.50              # add a remote host
wireviewctl top --host 192.168.1.50:9876,nas     # several (comma/space list, host[:port])
wireviewctl top --interval 500                   # refresh every 500 ms
wireviewctl --host 192.168.1.50 top              # the global --host works too
```

Each device gets a panel with colored power/current bar gauges (green/yellow/red
by load), the energy counter in Wh (when the module or remote daemon publishes
it), a per-pin Volts/Amps/Watts breakdown, temperatures, fan duty, and fault
state. A two-device view (one local, one remote) looks like:

```
 WireView top  2 devices  14:22:07  refresh 1.0s  q to quit

╭─ local ─ WireView Pro II ─ fw3 ─ cap 600W
│ Power   ████████████████░░░░░░░░░░░░    372.4 W     381.245 Wh
│ Current █████████████████░░░░░░░░░░░     31.2 A
│ Pin          1       2       3       4       5       6
│ Volts    12.11   12.13   12.10   12.12   12.11   12.12
│ Amps      5.21    5.40    5.08    5.33    5.55    4.63
│ Watts     63.1    65.5    61.4    64.6    67.2    56.1
│ Temp   In 45.3° Out 42.1° E1 38.7° E2 --   Fan 75%  Faults none
╰─
╭─ 192.168.1.50 ─ WireView Pro II ─ fw3 ─ cap 450W
│ Power   ██████░░░░░░░░░░░░░░░░░░░░░░    118.6 W      52.907 Wh
│ Current ███████░░░░░░░░░░░░░░░░░░░░░      9.8 A
│ Pin          1       2       3       4       5       6
│ Volts    12.09   12.10   12.08   12.11   12.09   12.10
│ Amps      1.62    1.70    1.55    1.68    1.61    1.64
│ Watts     19.6    20.6    18.7    20.3    19.5    19.8
│ Temp   In 39.8° Out 36.2° E1 -- E2 --   Fan 40%  Faults none
╰─
```

(An unreachable host, or a local device whose readings are stale, shows a single
red `offline` line instead of stalling the view.)

Remote hosts can also be listed (one `host[:port]` per line) in
`/etc/wireview/hosts`, which you create yourself (nothing installs it). The
local device is read straight from hwmon sysfs; remotes via `GET /sensors`.
Press **q** to quit.

### Audit log

When the listener is enabled, command activity is recorded to a daily-rotating
file in `/var/log/wireview/` (retention from `log_days`):

```
2026-06-10T09:41:08 [INFO] wireviewd 1.5.1 started; listener enabled on [::]:9876, remote writes enabled, log retention 14 days
2026-06-10T09:41:12 [INFO] command from 192.168.1.20: op=screen -> executed
2026-06-10T09:41:15 [WARN] command from 192.168.1.33 rejected: bad signature
```

High-frequency `/sensors` and `/metrics` polls are not logged.

## Configuration files

| File | Purpose |
|------|---------|
| `/etc/wireview/config` | Daemon settings: `remote_enabled`, `port`, `bind`, `secret`, `log_days`. Mode `600`. Read at (re)start. A commented reference is installed by the packages and `make install`. |
| `/etc/wireview/hosts` | Optional remote-host list for `wireviewctl top` (one `host[:port]` per line). Not installed; create it yourself if you want one. |
| `/var/log/wireview/` | Daily-rotating audit logs, mode `600` (created by the daemon on its first log write). |
| `/etc/avahi/services/wireview.service` | Optional mDNS advertisement of the listener. Installed by `make install` only; see [mDNS discovery](#mdns-discovery-optional). |

## Testing

```bash
make test
```

runs the unit tests for the daemon and CLI parsers, built with ASan/UBSan when
the compiler supports them, and an end-to-end test: `tests/e2e.py` runs a test
build of `wireviewd` against `tests/fake_device.py`, a simulated WireView Pro II
on a pseudo-terminal, with the hwmon node, socket, log directory and config
moved into a temporary directory, so it never touches an installed daemon.
`WIREVIEW_SKIP_E2E=1` skips the end-to-end test (so does a missing `python3`),
and `SANITIZE=0` builds without the sanitizers.

CI runs the same tests on every push and pull request, plus a `-Werror` build
of the daemon and CLI, a `W=1` module build that fails on any warning, and lint
(shellcheck, checkpatch, the version check).

## Roadmap

- **Multi-device support** (deferred). One hwmon device per WireView. The
  `/sensors` schema already has a `devices[]` array, but the daemon handles one
  device: this needs a misc node per device or a device id in the record, and a
  decision on how the hwmon devices are named.
- **WireView II and its Phanteks Edition** (deferred, products `EF07` and
  `EF08`). They are a different device: a 104-byte sensor frame with a fan
  tachometer, two temperature sensors, their own config layout and no display
  commands. `wireviewd` refuses them with a clear message until hardware or
  captures are available to test against.
- **Remote readings on a par with local ones**: carry average voltage, Vdd and
  the alarms in `GET /sensors`, so `wireviewctl --host h sensors` prints what
  the local command does.
- **Man pages** for `wireviewd` and `wireviewctl`.

### Known issues and small follow-ups

- `wireviewd` logs "device info query failed" once right after a restart and succeeds on the retry two seconds later. ModemManager probing the port was ruled out as the cause.
- A raw `.bin` firmware must be under 4 MiB, while a `.hex` may fill exactly 4 MiB.
- The deb postinst runs a bare `udevadm trigger`, which re-triggers every device; it should match only the WireView.
- A system that once had a 1.6.0 deb installed keeps an empty `/lib.usr-is-merged` directory after purge.
- In the GUI ([wireview-linux](https://github.com/emaspa/wireview-linux)): the deb and rpm ship the single-file binary and the unused loose libraries side by side, doubling the download; the deb declares no `Depends`; and a second launch while the app sits in the tray exits without raising the window.

## License

GPL-2.0
