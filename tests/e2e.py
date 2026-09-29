#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""End-to-end test: wireviewd and wireviewctl against a fake device.

Builds both with every host path (/dev/wireview-hwmon, /run/wireviewd.sock,
/var/log/wireview, the config file) redirected into a private temp dir, so
it runs unprivileged beside a live daemon and never touches real hardware.
The "hwmon device" is a plain file: each frame the daemon writes is
appended to it.

Environment: CC (default cc), SAN_FLAGS (extra compiler flags, e.g. ASan),
WIREVIEW_E2E_KEEP=1 keeps the temp dir.
"""

import glob
import grp
import os
import pwd
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import fake_device as fd  # noqa: E402

WIREVIEW_MAGIC = 0x57565032
# Frame size per hwmon_data version. v3 may grow the struct; the leading
# magic/version/voltage/current fields are assumed to stay put.
FRAME_SIZE = {2: 148, 3: 156}

WCMD_GET_DEVICE_INFO = 0x01
WCMD_SCREEN_CMD = 0x05
WCMD_NVM_CMD = 0x06
RESP_OK, RESP_DENIED = 0, 3

FW_VERSION = 7
BUILD = b"FAKE build 1.0"
UID = bytes(range(0xA1, 0xAD))
PINS = ((12000, 8000), (12050, 8100), (11990, 7900),
        (12010, 8050), (12020, 0), (12030, 8200))

passed = failed = 0


def check(cond, what, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"ok   {what}")
    else:
        failed += 1
        print(f"FAIL {what}" + (f": {detail}" if detail else ""))
    return cond


def make_tmpdir():
    # Unix socket paths are limited to 108 bytes: prefer the short runtime
    # dir, then $TMPDIR, then /tmp.
    for base in (os.environ.get("XDG_RUNTIME_DIR"), None, "/tmp"):
        if base is not None and not os.access(base, os.W_OK):
            continue
        d = tempfile.mkdtemp(prefix="wv-e2e-", dir=base)
        if len(os.path.join(d, "wireviewd.sock")) < 100:
            return d
        shutil.rmtree(d)
    sys.exit("e2e: no temp dir short enough for a Unix socket path")


def build(tmp):
    cc = os.environ.get("CC", "cc")
    san = shlex.split(os.environ.get("SAN_FLAGS", ""))
    version = open(os.path.join(TOP, "VERSION")).read().strip()
    defs = [
        f'-DWIREVIEW_PKG_VERSION="{version}-e2e"',
        f'-DHWMON_DEV="{tmp}/hwmon"',
        f'-DSOCK_PATH="{tmp}/wireviewd.sock"',
        f'-DLOG_DIR="{tmp}/log"',
        f'-DCONFIG_PATH="{tmp}/config"',
    ]
    flags = ["-g", "-O1", "-Wall", "-Wextra", "-Wno-format-truncation"] + san
    for out, srcs in (("wireviewd", ["wireviewd.c", "sha256.c"]),
                      ("wireviewctl", ["wireviewctl.c"])):
        cmd = [cc] + flags + defs + ["-o", os.path.join(tmp, out)] + \
            [os.path.join(TOP, s) for s in srcs]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"e2e: building {out} failed:\n{r.stderr}")


def request(sock_path, cmd, payload=b""):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(3)
        s.connect(sock_path)
        s.sendall(bytes([cmd]) + struct.pack("<H", len(payload)) + payload)
        hdr = b""
        while len(hdr) < 3:
            c = s.recv(3 - len(hdr))
            if not c:
                raise EOFError("daemon closed the connection")
            hdr += c
        status, n = struct.unpack("<BH", hdr)
        data = b""
        while len(data) < n:
            c = s.recv(n - len(data))
            if not c:
                raise EOFError("short response")
            data += c
        return status, data


def privileged():
    """Mirror wireviewd's peer check: root, or the wireview group."""
    if os.getuid() == 0:
        return True
    try:
        gid = grp.getgrnam("wireview").gr_gid
    except KeyError:
        return False
    pw = pwd.getpwuid(os.getuid())
    return gid == os.getgid() or gid in os.getgrouplist(pw.pw_name, pw.pw_gid)


def read_frames(path):
    """Split the hwmon file into frames; returns (frames, error)."""
    data = open(path, "rb").read()
    if len(data) < 8:
        return [], f"only {len(data)} bytes"
    magic, version = struct.unpack_from("<II", data)
    size = FRAME_SIZE.get(version)
    if magic != WIREVIEW_MAGIC or size is None:
        return [], f"first frame magic 0x{magic:08x} version {version}"
    if len(data) % size:
        return [], f"{len(data)} bytes is not a multiple of {size}"
    frames = [data[i:i + size] for i in range(0, len(data), size)]
    for i, f in enumerate(frames):
        if struct.unpack_from("<II", f) != (magic, version):
            return [], f"frame {i} has a bad header"
    return frames, None


def wait_until(cond, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


def audit_log(tmp):
    text = ""
    for p in sorted(glob.glob(os.path.join(tmp, "log", "wireviewd-*.log"))):
        text += open(p, errors="replace").read()
    return text


def ctl(tmp, *args):
    return subprocess.run([os.path.join(tmp, "wireviewctl"), *args],
                          capture_output=True, text=True, timeout=10)


def run(tmp):
    hwmon = os.path.join(tmp, "hwmon")
    sock = os.path.join(tmp, "wireviewd.sock")
    open(hwmon, "wb").close()

    dev = fd.FakeWireView(fw_version=FW_VERSION, uid=UID, build=BUILD,
                          sensors=fd.pack_sensors(pins=PINS))
    dev.start()
    env = dict(os.environ, WIREVIEW_LISTEN="0")
    env.pop("WIREVIEW_SECRET", None)
    out = open(os.path.join(tmp, "daemon.out"), "w")
    daemon = subprocess.Popen(
        [os.path.join(tmp, "wireviewd"), "-i", "100", "-d", dev.path],
        env=env, stdout=out, stderr=subprocess.STDOUT)
    try:
        exercise(tmp, dev, daemon, hwmon, sock)
    finally:
        if daemon.poll() is None:
            daemon.send_signal(signal.SIGTERM)
            try:
                rc = daemon.wait(5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                rc = daemon.wait()
            check(rc == 0, "daemon exits 0 on SIGTERM", f"exit status {rc}")
        out.close()
        dev.stop()
    log = open(os.path.join(tmp, "daemon.out")).read()
    check("wireviewd: stopped" in log, "daemon logs a clean stop")
    check(f"FW v{FW_VERSION}, config v2" in log,
          "daemon read the fake device info at connect")
    check(not dev.unknown, "daemon sent only known command bytes",
          f"unknown: {dev.unknown}")

    # The frames written to the "hwmon device" after the daemon stopped.
    frames, err = read_frames(hwmon)
    check(err is None and len(frames) >= 3,
          "hwmon file holds whole frames with magic and a known version",
          err or f"{len(frames)} frames")
    if frames:
        mv = struct.unpack_from("<6i", frames[0], 8)
        ma = struct.unpack_from("<6i", frames[0], 32)
        check(mv == tuple(p[0] for p in PINS) and
              ma == tuple(p[1] for p in PINS),
              "first frame carries the fake per-pin voltage and current",
              f"mV {mv} mA {ma}")
        # The corrupt frame had vdd 4321 and fan 105; nothing past the
        # sanity check may reach hwmon. Offsets are those of v2.
        if struct.unpack_from("<I", frames[0], 4)[0] == 2:
            vdd_fan = [struct.unpack_from("<iB", f, 136) for f in frames]
            check(all(v == (3300, 42) for v in vdd_fan),
                  "no corrupt frame reached hwmon",
                  f"vdd/fan seen: {sorted(set(vdd_fan))}")


def exercise(tmp, dev, daemon, hwmon, sock):
    if not check(wait_until(lambda: os.path.exists(sock) or
                            daemon.poll() is not None, 10) and
                 daemon.poll() is None,
                 "daemon connects to the fake device and opens its socket"):
        raise RuntimeError("daemon did not come up")

    frame_min = min(FRAME_SIZE.values())
    check(wait_until(lambda: os.path.getsize(hwmon) >= 3 * frame_min, 5),
          "daemon writes sensor frames to the hwmon device",
          f"{os.path.getsize(hwmon)} bytes")

    # GET_DEVICE_INFO over the socket: fw, config version, uid, build.
    status, data = request(sock, WCMD_GET_DEVICE_INFO)
    check(status == RESP_OK and len(data) >= 14 and data[0] == FW_VERSION,
          "GET_DEVICE_INFO returns the fake firmware version",
          f"status {status} data {data!r}")
    check(data[1:2] == b"\x02" and data[2:14] == UID,
          "GET_DEVICE_INFO returns the config version and UID")
    check(data[14:].rstrip(b"\0") == BUILD,
          "GET_DEVICE_INFO returns the build string", repr(data[14:]))

    r = ctl(tmp, "info")
    check(r.returncode == 0 and f"firmware: {FW_VERSION}" in r.stdout and
          f"build: {BUILD.decode()}" in r.stdout and
          "uid: " + UID.hex() in r.stdout,
          "wireviewctl info shows the fake device", r.stdout + r.stderr)

    # A screen command reaches the device, raw and through the CLI.
    status, _ = request(sock, WCMD_SCREEN_CMD, b"\xe0")
    check(status == RESP_OK and
          dev.wait_for(lambda: b"\xe0" in dev.writes_of(fd.CMD_SCREEN_CHANGE)),
          "socket SCREEN_CMD reaches the device as 0C E0")
    r = ctl(tmp, "screen", "temp")
    check(r.returncode == 0 and
          dev.wait_for(lambda: b"\xe3" in dev.writes_of(fd.CMD_SCREEN_CHANGE)),
          "wireviewctl screen temp reaches the device as 0C E3",
          r.stdout + r.stderr)

    # clear-faults sends keep-masks: everything cleared -> 0/0; clearing
    # status bit 0 only -> keep FFFE, log 0.
    r = ctl(tmp, "clear-faults")
    check(r.returncode == 0 and dev.wait_for(
          lambda: b"\x00\x00\x00\x00" in dev.writes_of(fd.CMD_CLEAR_FAULTS)),
          "wireviewctl clear-faults sends keep-masks 0000/0000",
          f"{r.stderr} {dev.writes_of(fd.CMD_CLEAR_FAULTS)}")
    r = ctl(tmp, "clear-faults", "1", "0")
    check(r.returncode == 0 and dev.wait_for(
          lambda: b"\xfe\xff\xff\xff" in dev.writes_of(fd.CMD_CLEAR_FAULTS)),
          "wireviewctl clear-faults 1 0 sends keep-masks FFFE/FFFF",
          f"{r.stderr} {dev.writes_of(fd.CMD_CLEAR_FAULTS)}")

    # Privileged commands: refused unless root or in the wireview group.
    status, _ = request(sock, WCMD_NVM_CMD, b"\x02")
    if privileged():
        check(status == RESP_OK and dev.wait_for(
              lambda: dev.writes_of(fd.CMD_NVM_CONFIG)) ==
              [b"\x55\xaa\x55\xaa\x02"],
              "privileged NVM store reaches the device")
    else:
        time.sleep(0.2)
        check(status == RESP_DENIED and not dev.writes_of(fd.CMD_NVM_CONFIG),
              "NVM from an unprivileged peer is denied and not relayed",
              f"status {status}")

    # A corrupt frame is dropped and audited; polling carries on.
    polls = dev.polls
    dev.inject_corrupt()
    check(dev.wait_for(lambda: dev.corrupt_sent == 1 and
                       dev.polls >= polls + 3, 5),
          "fake device served a corrupt frame and polling continued")
    check(wait_until(lambda: "discarded corrupt sensor frame (fan=105 pad1=122"
                     in audit_log(tmp), 3),
          "audit log records the discarded corrupt frame")
    check(daemon.poll() is None, "daemon still running after the corrupt frame")


def main():
    tmp = make_tmpdir()
    try:
        build(tmp)
        try:
            run(tmp)
        except Exception as e:  # report, then fall through to the summary
            check(False, "e2e run", f"{type(e).__name__}: {e}")
        if failed:
            for name in ("daemon.out",):
                p = os.path.join(tmp, name)
                if os.path.exists(p):
                    print(f"--- {name}\n" + open(p).read())
            print("--- audit log\n" + audit_log(tmp))
    finally:
        if os.environ.get("WIREVIEW_E2E_KEEP"):
            print(f"e2e: kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f"e2e: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
