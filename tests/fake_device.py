#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Fake Thermal Grizzly WireView Pro II on a pseudo-terminal.

Speaks the device's serial protocol closely enough for wireviewd: one
command byte in, a fixed-size little-endian struct out (layouts as
documented in wireviewd.c). Write commands are accepted and recorded.

As a library:

    dev = FakeWireView(fw_version=7)
    dev.start()
    ... run "wireviewd -d <dev.path>" ...
    dev.inject_corrupt()          # next sensor poll gets a corrupt frame
    dev.wait_for(lambda: dev.writes_of(CMD_SCREEN_CHANGE))
    dev.stop()

Standalone, for manual testing ("wireviewd -d <path printed>"):

    python3 tests/fake_device.py [--fw 7] [--build "FAKE 1.0"]
"""

import argparse
import os
import pty
import select
import signal
import struct
import sys
import threading
import time
import tty

CMD_READ_VENDOR_DATA = 0x01
CMD_READ_UID = 0x02
CMD_READ_SENSOR_VALUES = 0x04
CMD_READ_CONFIG = 0x05
CMD_WRITE_CONFIG = 0x06
CMD_SCREEN_CHANGE = 0x0C
CMD_READ_BUILD_INFO = 0x0D
CMD_CLEAR_FAULTS = 0x0E
CMD_BOOTLOADER = 0xF1
CMD_NVM_CONFIG = 0xF2

# Total length (command byte included) of the fixed-size write commands.
WRITE_LEN = {
    CMD_SCREEN_CHANGE: 2,   # [0x0C][screen]
    CMD_CLEAR_FAULTS: 5,    # [0x0E][status_keep u16][log_keep u16]
    CMD_NVM_CONFIG: 6,      # [0xF2][55 AA 55 AA][op]
    CMD_BOOTLOADER: 1,
}

# SensorStruct (Pack=4): ts[4] i16, vdd u16, fan u8, pad1 u8,
# 6 x PowerSensor {voltage i16, pad u16, current u32, power u32},
# total_power u32, total_current u32, avg_voltage u16, hpwr_cap u8,
# pad2 u8, fault_status u16, fault_log u16.
SENSOR_FMT = "<4hHBB" + "hHII" * 6 + "IIHBBHH"
SENSOR_SIZE = struct.calcsize(SENSOR_FMT)
assert SENSOR_SIZE == 100

VENDOR_ID = (0xEF, 0x05)    # VendorDataStruct bytes 0-1, checked by wireviewd
BUILD_SIZE = 68             # VendorData(3) + ProductName(32) + BuildInfo(32) + len(1)
CONFIG_SIZE = {0: 72, 1: 74, 2: 96}


def pack_sensors(ts=(355, 400, 2001, 2001), vdd=3300, fan=42, pad1=0,
                 pins=((12000, 8000),) * 6, hpwr_cap=0, pad2=0,
                 fault_status=0, fault_log=0):
    """Build a raw SensorStruct. pins are (mV, mA) pairs; ts is 0.1 degC."""
    fields = list(ts) + [vdd, fan, pad1]
    total_mw = total_ma = 0
    for mv, ma in pins:
        mw = mv * ma // 1000
        fields += [mv, 0, ma, mw]
        total_mw += mw
        total_ma += ma
    avg_mv = sum(mv for mv, _ in pins) // len(pins)
    fields += [total_mw, total_ma, avg_mv, hpwr_cap, pad2, fault_status,
               fault_log]
    return struct.pack(SENSOR_FMT, *fields)


class FakeWireView:
    def __init__(self, fw_version=7, uid=bytes(range(1, 13)),
                 product=b"WireView Pro II", build=b"FAKE build 1.0",
                 config_version=2, sensors=None):
        self.fw_version = fw_version
        self.uid = uid
        self.product = product
        self.build = build
        self.config_version = config_version
        self.config = bytearray(CONFIG_SIZE[config_version])
        self.config[2] = config_version     # ConfigStruct Version field
        self.sensors = sensors if sensors is not None else pack_sensors()
        self.corrupt_frame = pack_sensors(vdd=4321, fan=105, pad1=122,
                                          fault_status=0x0607)
        self.polls = 0              # sensor reads answered
        self.corrupt_sent = 0
        self.writes = []            # (cmd, bytes) of every write command
        self.unknown = []           # command bytes we did not understand
        self._corrupt_pending = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None
        self.master = self.slave = -1
        self.path = None

    # -- control -------------------------------------------------------

    def start(self):
        self.master, self.slave = pty.openpty()
        tty.setraw(self.master)
        self.path = os.ttyname(self.slave)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self.path

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(2)
        for fd in (self.master, self.slave):
            if fd >= 0:
                os.close(fd)
        self.master = self.slave = -1

    def inject_corrupt(self, count=1):
        """Answer the next count sensor reads with a corrupt frame
        (fan=105, pad1=122, status=0x0607, as seen in the field)."""
        with self._lock:
            self._corrupt_pending += count

    def writes_of(self, cmd):
        with self._lock:
            return [data for c, data in self.writes if c == cmd]

    def wait_for(self, cond, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = cond()
            if result:
                return result
            time.sleep(0.02)
        return cond()

    # -- protocol ------------------------------------------------------

    def _reply(self, cmd):
        if cmd == CMD_READ_VENDOR_DATA:
            return bytes([*VENDOR_ID, self.fw_version])
        if cmd == CMD_READ_UID:
            return self.uid
        if cmd == CMD_READ_BUILD_INFO:
            b = bytearray(BUILD_SIZE)
            b[0:3] = bytes([*VENDOR_ID, self.fw_version])
            b[3:3 + len(self.product[:32])] = self.product[:32]
            b[35:35 + len(self.build[:32])] = self.build[:32]
            b[67] = len(self.product[:32])
            return bytes(b)
        if cmd == CMD_READ_CONFIG:
            return bytes(self.config)
        if cmd == CMD_READ_SENSOR_VALUES:
            with self._lock:
                self.polls += 1
                if self._corrupt_pending:
                    self._corrupt_pending -= 1
                    self.corrupt_sent += 1
                    return self.corrupt_frame
            return self.sensors
        return None

    def _handle(self, chunk):
        """Handle one read() worth of bytes. Write commands with a variable
        length (WRITE_CONFIG) take the rest of the chunk, like the USB
        packet the real device receives."""
        i = 0
        while i < len(chunk):
            cmd = chunk[i]
            reply = self._reply(cmd)
            if reply is not None:
                os.write(self.master, reply)
                i += 1
                continue
            if cmd == CMD_WRITE_CONFIG:
                n = len(chunk) - i
            elif cmd in WRITE_LEN:
                n = WRITE_LEN[cmd]
            else:
                with self._lock:
                    self.unknown.append(cmd)
                i += 1
                continue
            data = bytes(chunk[i + 1:i + n])
            if cmd == CMD_WRITE_CONFIG and len(data) >= 1:
                off = data[0]
                self.config[off:off + len(data) - 1] = data[1:]
            with self._lock:
                self.writes.append((cmd, data))
            i += n

    def _run(self):
        while not self._stop.is_set():
            r, _, _ = select.select([self.master], [], [], 0.1)
            if not r:
                continue
            try:
                chunk = os.read(self.master, 4096)
            except OSError:
                return
            if chunk:
                self._handle(chunk)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fw", type=int, default=7, help="firmware version")
    ap.add_argument("--build", default="FAKE build 1.0", help="build string")
    args = ap.parse_args()

    dev = FakeWireView(fw_version=args.fw, build=args.build.encode())
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    print(dev.start(), flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        dev.stop()
        print(f"polls={dev.polls} writes={dev.writes}", file=sys.stderr)


if __name__ == "__main__":
    main()
