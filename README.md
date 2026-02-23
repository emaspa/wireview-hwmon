# wireview-hwmon

Linux hwmon driver and daemon for the [Thermal Grizzly WireView Pro II](https://www.thermal-grizzly.com/en/wireview-pro-ii-gpu/s-tg-wv-p2) power monitor. Exposes voltage, current, power, and temperature sensor data through the standard Linux hwmon subsystem.

This is a standalone alternative to the [wireview-linux](https://github.com/emaspa/wireview-linux) GUI application. Use this if you want sensor data accessible to `sensors`, Grafana, conky, btop, and other monitoring tools without running the full app.

## How it works

```
WireView Pro II (USB) → wireviewd (serial) → kernel module → /sys/class/hwmon/ → monitoring tools
```

- **wireview_hwmon.ko** — Kernel module that creates a virtual hwmon device
- **wireviewd** — Userspace daemon that reads the device over serial and feeds the kernel module

## Requirements

- Linux with kernel headers (`linux-headers-$(uname -r)`)
- A Thermal Grizzly WireView Pro II device connected via USB
- `gcc` and `make`

## Build

```bash
make
```

This builds both the kernel module (`wireview_hwmon.ko`) and the daemon (`wireviewd`).

## Quick start

```bash
# Load the kernel module
sudo insmod wireview_hwmon.ko

# Run the daemon
sudo ./wireviewd

# In another terminal, check sensor data
sensors wireview-*
```

## Install (persistent)

```bash
sudo make install

# Load module and start daemon
sudo modprobe wireview_hwmon
sudo systemctl enable --now wireviewd

# Verify
sensors wireview-*
```

To auto-load the module on boot:

```bash
echo wireview_hwmon | sudo tee /etc/modules-load.d/wireview-hwmon.conf
```

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
| Pin 1-6 Current | `curr1_input` - `curr6_input` | milliamps |
| Total Power | `power1_input` | microwatts |
| Onboard Temp In | `temp1_input` | millidegrees C |
| Onboard Temp Out | `temp2_input` | millidegrees C |
| External Temp 1 | `temp3_input` | millidegrees C |
| External Temp 2 | `temp4_input` | millidegrees C |

## Example `sensors` output

```
wireview-virtual-0
Adapter: Virtual device
Pin 1:       +12.12 V
Pin 2:       +12.13 V
Pin 3:       +12.11 V
Pin 4:       +12.12 V
Pin 5:       +12.11 V
Pin 6:       +12.12 V
Pin 1:        5.23 A
Pin 2:        5.45 A
Pin 3:        5.12 A
Pin 4:        5.34 A
Pin 5:        5.56 A
Pin 6:        5.43 A
Total:      384.50 W
Onboard In:  +45.3°C
Onboard Out: +42.1°C
External 1:  +38.7°C
External 2:    N/A
```

## Notes

- The daemon and the wireview-linux GUI app both use the serial port, so only run one at a time.
- If the device is disconnected, the daemon will wait and reconnect automatically.
- Sensor readings become stale (report N/A) if no data is received for 5 seconds.

## License

GPL-2.0
