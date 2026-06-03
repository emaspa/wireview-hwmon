# wireview-hwmon

Linux hwmon driver and daemon for the [Thermal Grizzly WireView Pro II](https://www.thermal-grizzly.com/en/wireview-pro-ii-gpu/s-tg-wv-p2) power monitor. Exposes voltage, current, power, and temperature sensor data through the standard Linux hwmon subsystem.

Works standalone or alongside the [wireview-linux](https://github.com/emaspa/wireview-linux) GUI application. When both are used together, the app reads sensor data from hwmon and sends commands through the daemon's Unix socket — giving you full app functionality plus system-wide sensor integration.

## How it works

```
WireView Pro II (USB) → wireviewd (serial) → kernel module → /sys/class/hwmon/ → monitoring tools
```

- **wireview_hwmon.ko** — Kernel module that creates a virtual hwmon device
- **wireviewd** — Userspace daemon that reads the device over serial and feeds the kernel module
- **wireviewctl** — CLI tool for querying sensors and sending commands via the daemon

## Installation

### Ubuntu 24.04 / 26.04 (PPA)

```bash
sudo add-apt-repository ppa:sparvoli/wireview-hwmon
sudo apt update
sudo apt install wireview-hwmon wireview-hwmon-dkms
```

This installs the daemon, CLI tool, kernel module (via DKMS), systemd service, and udev rules. The module auto-rebuilds on kernel updates and the daemon starts automatically.

### Ubuntu / Debian (.deb packages)

Pre-built `.deb` packages are available on the [Releases](https://github.com/emaspa/wireview-hwmon/releases) page. Download and install:

```bash
sudo apt install ./wireview-hwmon_1.3.2_amd64.deb ./wireview-hwmon-dkms_1.3.2_all.deb
```

### Build from source

#### Requirements

- Linux with kernel headers (`linux-headers-$(uname -r)`)
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
# Install udev rules (serial port access + hwmon device permissions)
sudo cp 99-wireview-hwmon.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger

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

# Reboot — MokManager will prompt you to enroll the key

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
wireviewd [-i interval_ms] [-d /dev/ttyACMx]

  -i  Poll interval in milliseconds (default: 1000)
  -d  Serial device path (default: auto-detect)
```

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
| Fan Duty | `fan1_input` | % (0-100) |
| Fault Status | `intrusion0_alarm` (`intrusion0_label`) | 0/1 |
| Fault Log | `intrusion1_alarm` (`intrusion1_label`) | 0/1 |
| Fault Status (raw) | `fault_status_raw` | bitmask |
| Fault Log (raw) | `fault_log_raw` | bitmask |
| PSU Capability | `psu_cap` | enum (0-4) |

All voltage, current, power, and temperature channels also expose `_label` attributes for tool-friendly names.

## Example `sensors` output

```
wireview-isa-0000
Adapter: ISA adapter
Pin 1:       +12.12 V
Pin 2:       +12.13 V
Pin 3:       +12.11 V
Pin 4:       +12.12 V
Pin 5:       +12.11 V
Pin 6:       +12.12 V
Average:     +12.12 V
Vdd:          +3.30 V
Pin 1:        5.23 A
Pin 2:        5.45 A
Pin 3:        5.12 A
Pin 4:        5.34 A
Pin 5:        5.56 A
Pin 6:        5.43 A
Total:       32.13 A
Total:      384.50 W
Pin 1:       63.40 W
Pin 2:       66.11 W
Pin 3:       62.01 W
Pin 4:       64.68 W
Pin 5:       67.33 W
Pin 6:       65.82 W
Onboard In:  +45.3°C
Onboard Out: +42.1°C
External 1:  +38.7°C
External 2:    N/A
fan1:           75
Fault Status:  ALARM
Fault Log:     OK
```

## CLI tool

`wireviewctl` lets you query sensor data and send device commands from the terminal or scripts.

```
wireviewctl <command> [args]

Commands (require wireviewd running):
  info              Show device firmware, UID, and build info
  clear-faults      Clear all fault status and log
  read-config       Read device config (hex to stdout)
  write-config FILE Write device config (hex from file)
  screen CMD        Change display (main|simple|current|temp|status|same|pause|resume)
  nvm CMD           NVM operation (load|store|reset|load-cal|store-cal|load-cal-factory|store-cal-factory)
  build             Show firmware build string
  bootloader        Enter DFU bootloader mode

Commands (require wireview_hwmon module):
  sensors           Show all sensor readings from hwmon sysfs
```

### Examples

```bash
# Show device info
wireviewctl info

# Read all sensors (scriptable key: value format)
wireviewctl sensors

# Switch to simple display
wireviewctl screen simple

# Back up and restore config
wireviewctl read-config > config.hex
wireviewctl write-config config.hex

# Store config to NVM
wireviewctl nvm store

# Use in scripts
POWER=$(wireviewctl sensors | grep total_power_uw | cut -d' ' -f2)
echo "Total power: $((POWER / 1000000)) W"
```

## Daemon socket

The daemon listens on a Unix socket at `/run/wireviewd.sock`, allowing external programs (including the [wireview-linux](https://github.com/emaspa/wireview-linux) app) to send commands to the device without direct serial access. Supported commands:

| Command | Description |
|---------|-------------|
| GET_DEVICE_INFO | Query firmware version, config version, UID, build string |
| CLEAR_FAULTS | Clear fault status and/or fault log |
| READ_CONFIG | Read the device configuration |
| WRITE_CONFIG | Write a new device configuration |
| SCREEN_CMD | Send a screen command (change display page) |
| NVM_CMD | Send an NVM command (store/recall configuration) |
| READ_BUILD | Read the firmware build string |
| ENTER_BOOTLOADER | Restart the device into DFU bootloader mode |

The socket uses a binary protocol: request `[type:u8][len:u16 LE][payload]`, response `[status:u8][len:u16 LE][payload]`.

## Notes

- When used with the wireview-linux app, the daemon handles the serial port and the app communicates through hwmon (sensors) and the daemon socket (commands). Both can run simultaneously.
- When used standalone (without the app), the daemon owns the serial port exclusively.
- If the device is disconnected, the daemon will wait and reconnect automatically.
- Sensor readings become stale (report N/A) if no data is received for 5 seconds.

## License

GPL-2.0
