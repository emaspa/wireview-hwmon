#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Fake Thermal Grizzly WireView Pro II on a pseudo-terminal.

Speaks the device's serial protocol closely enough for wireviewd: one
command byte in, a fixed-size little-endian struct out (layouts as
documented in wireviewd.c). Write commands are accepted and recorded;
WRITE_CONFIG frames are reassembled from the byte stream by the config
size of the device's config version (see _write_len) and applied to the
config that READ_CONFIG returns.

As a library:

    dev = FakeWireView(fw_version=7)
    dev.start()
    ... run "wireviewd -d <dev.path>" ...
    dev.inject_corrupt()          # next sensor poll gets a corrupt frame
    dev.wait_for(lambda: dev.writes_of(CMD_SCREEN_CHANGE))
    dev.stop()

To simulate unplugging, start it with a stable link path (a new pty gets a
new /dev/pts number) and run "wireviewd -d <link>":

    dev.start(link="/tmp/x/ttyWV")
    dev.unplug()                  # pty closed, link removed
    dev.replug()                  # fresh pty behind the same link

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
CONFIG_CHUNK = 62           # data bytes per WRITE_CONFIG frame


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
        self.configs_written = []   # config after each complete write
        self.unknown = []           # command bytes we did not understand
        self._rx = bytearray()      # bytes of a command not complete yet
        self._corrupt_pending = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None
        self.master = self.slave = -1
        self.path = None            # the link if given, else the pty
        self.link = None
        self.plugs = 0              # ptys opened so far

    # -- control -------------------------------------------------------

    def start(self, link=None):
        """Open the pty and serve it. With link, a symlink at that path
        points at the pty and self.path is the link."""
        self.link = link
        self._open()
        return self.path

    def stop(self):
        self._close()
        if self.link and os.path.islink(self.link):
            os.unlink(self.link)

    def unplug(self):
        """Close the pty (the daemon's reads fail as on a USB unplug) and
        remove the link, so reopening it fails until replug()."""
        self._close()
        if self.link and os.path.islink(self.link):
            os.unlink(self.link)

    def replug(self):
        """Serve a new pty behind the same link."""
        self._open()
        return self.path

    def _open(self):
        self.master, self.slave = pty.openpty()
        tty.setraw(self.master)
        tty_path = os.ttyname(self.slave)
        if self.link:
            tmp = self.link + ".new"
            if os.path.lexists(tmp):
                os.unlink(tmp)
            os.symlink(tty_path, tmp)
            os.replace(tmp, self.link)
            self.path = self.link
        else:
            self.path = tty_path
        self.plugs += 1
        self._rx = bytearray()      # a new pty starts a new stream
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run,
                                        args=(self._stop, self.master),
                                        daemon=True)
        self._thread.start()

    def _close(self):
        self._stop.set()
        if self._thread:
            self._thread.join(2)
            self._thread = None
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

    def _write_len(self, rx):
        """Total length (command byte included) of the write command at
        the start of rx, None while more bytes are needed to know it, or
        0 for an unknown command byte.

        WRITE_CONFIG has no length field: [0x06][offset][data]. The real
        device takes the USB packet as the frame; a pty has no packets,
        so the frame is sized from what wireviewd sends: 62-byte chunks
        of a config of the size this device's config version has, i.e.
        min(62, size - offset) data bytes. That does not depend on how
        the bytes happen to be split across read() calls."""
        cmd = rx[0]
        if cmd in WRITE_LEN:
            return WRITE_LEN[cmd]
        if cmd != CMD_WRITE_CONFIG:
            return 0
        if len(rx) < 2:
            return None
        off = rx[1]
        if off >= len(self.config):
            return 2        # nothing to write there; recorded as bad
        return 2 + min(CONFIG_CHUNK, len(self.config) - off)

    def feed(self, data):
        """Parse bytes from the host; returns the reply bytes. Commands
        may be split across calls: an incomplete write command waits in
        the receive buffer for the rest."""
        self._rx += data
        out = bytearray()
        while self._rx:
            cmd = self._rx[0]
            reply = self._reply(cmd)
            if reply is not None:
                out += reply
                del self._rx[:1]
                continue
            n = self._write_len(self._rx)
            if n is None or len(self._rx) < n:
                break       # rest of the command still to come
            if n == 0:
                with self._lock:
                    self.unknown.append(cmd)
                del self._rx[:1]
                continue
            data = bytes(self._rx[1:n])
            del self._rx[:n]
            with self._lock:
                if cmd == CMD_WRITE_CONFIG:
                    off = data[0]
                    if off >= len(self.config):
                        self.unknown.append(cmd)
                        continue
                    self.config[off:off + len(data) - 1] = data[1:]
                    if off + len(data) - 1 == len(self.config):
                        self.configs_written.append(bytes(self.config))
                self.writes.append((cmd, data))
        return bytes(out)

    def _handle(self, master, chunk):
        reply = self.feed(chunk)
        if reply:
            os.write(master, reply)

    def _run(self, stop, master):
        while not stop.is_set():
            r, _, _ = select.select([master], [], [], 0.1)
            if not r:
                continue
            try:
                chunk = os.read(master, 4096)
            except OSError:
                return
            if chunk:
                try:
                    self._handle(master, chunk)
                except OSError:
                    return


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
