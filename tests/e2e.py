#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""End-to-end test: wireviewd and wireviewctl against a fake device.

Builds both with every host path (/dev/wireview-hwmon, /run/wireviewd.sock,
/var/log/wireview, the config file) redirected into a private temp dir, so
it runs unprivileged beside a live daemon and never touches real hardware.
The "hwmon device" is a FIFO the test drains, so like the real character
device every frame the daemon writes (one write() each) is kept in order,
across reconnects too; a plain file would be rewritten from offset 0 on
every reopen. The daemon reads its config from the temp dir too, with
the LAN listener on 127.0.0.1 and a free port, and wireviewctl reads a fake
hwmon sysfs directory through $WIREVIEW_HWMON_PATH.

The main run drives a WireView Pro II (product 5). The same build then
serves a Noctua Edition (product 6), which must be reported as such, and
a WireView II (product 7), which must be refused.

Environment: CC (default cc), SAN_FLAGS (extra compiler flags, e.g. ASan),
WIREVIEW_E2E_KEEP=1 keeps the temp dir.
"""

import errno
import glob
import grp
import http.client
import json
import os
import re
import select
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import fake_device as fd  # noqa: E402

WIREVIEW_MAGIC = 0x57565032
# Frame size per hwmon_data version. v3 is v2 with an int64 energy_uj
# appended, so every v2 offset holds in both.
FRAME_SIZE = {2: 148, 3: 156}
OFF_VOLTAGE, OFF_CURRENT, OFF_VDD, OFF_ENERGY = 8, 32, 136, 148

WCMD_GET_DEVICE_INFO = 0x01
WCMD_READ_CONFIG = 0x03
WCMD_WRITE_CONFIG = 0x04
WCMD_SCREEN_CMD = 0x05
WCMD_NVM_CMD = 0x06
WCMD_ENTER_BOOTLOADER = 0x08
WCMD_SUSPEND_SERIAL = 0x09
WCMD_RESUME_SERIAL = 0x0A
RESP_OK, RESP_NOT_CONNECTED, RESP_DENIED = 0, 2, 3
SECRET = "e2e-secret"   # POST /command on the loopback listener
IDLE_S = 2              # CLIENT_IDLE_S of the test build (60 in production)
MAX_CLIENTS = 8

FW_VERSION = 7
BUILD = b"FAKE build 1.0"
UID = bytes(range(0xA1, 0xAD))
PINS = ((12000, 8000), (12050, 8100), (11990, 7900),
        (12010, 8050), (12020, 0), (12030, 8200))
UID_HEX = UID.hex().upper()

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


# The privileged socket commands are gated on WIREVIEW_GROUP. The main test
# daemon is built with the test user's own primary group there, so the
# privileged path (config writes, NVM, serial handover) runs as an ordinary
# user; a second build names a group that does not exist, so the same user
# is an unprivileged peer (root is always privileged: those checks skip).
NO_GROUP = "wv-e2e-no-such-group"


def primary_group():
    try:
        return grp.getgrgid(os.getgid()).gr_name
    except KeyError:
        return None


def build(tmp, group, programs=("wireviewd", "wireviewctl")):
    cc = os.environ.get("CC", "cc")
    san = shlex.split(os.environ.get("SAN_FLAGS", ""))
    version = open(os.path.join(TOP, "VERSION")).read().strip()
    defs = [
        f'-DWIREVIEW_PKG_VERSION="{version}-e2e"',
        f'-DHWMON_DEV="{tmp}/hwmon"',
        f'-DSOCK_PATH="{tmp}/wireviewd.sock"',
        f'-DLOG_DIR="{tmp}/log"',
        f'-DCONFIG_PATH="{tmp}/config"',
        f'-DWIREVIEW_GROUP="{group}"',
        f'-DCLIENT_IDLE_S={IDLE_S}',
    ]
    flags = ["-g", "-O1", "-Wall", "-Wextra", "-Wno-format-truncation"] + san
    for out, srcs in (("wireviewd", ["wireviewd.c", "sha256.c"]),
                      ("wireviewctl", ["wireviewctl.c", "sha256.c"])):
        if out not in programs:
            continue
        cmd = [cc] + flags + defs + ["-o", os.path.join(tmp, out)] + \
            [os.path.join(TOP, s) for s in srcs]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"e2e: building {out} failed:\n{r.stderr}")


class Client:
    """A command-socket connection that stays open across requests."""

    def __init__(self, sock_path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(3)
        self.s.connect(sock_path)

    def request(self, cmd, payload=b""):
        s = self.s
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

    def closed_by_daemon(self, timeout=0.0):
        """True once the daemon has closed this connection (EOF or a
        reset), waiting up to timeout seconds for it."""
        r, _, _ = select.select([self.s], [], [], timeout)
        if not r:
            return False
        try:
            return self.s.recv(1) == b""
        except OSError:
            return True

    def close(self):
        self.s.close()


def request(sock_path, cmd, payload=b""):
    c = Client(sock_path)
    try:
        return c.request(cmd, payload)
    finally:
        c.close()


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http_get(port, path):
    """GET over the daemon's listener; returns (status, headers, text)."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        conn.request("GET", path)
        r = conn.getresponse()
        return r.status, dict(r.getheaders()), r.read().decode()
    finally:
        conn.close()


METRIC_RE = re.compile(r'^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{(.*)\})? (\S+)$')
LABEL_RE = re.compile(r'([a-zA-Z_][a-zA-Z0-9_]*)="((?:[^"\\]|\\.)*)"(?:,|$)')


def parse_metrics(text):
    """Parse the Prometheus text format; returns ({(name, labels): value},
    [lines that did not parse])."""
    samples, bad = {}, []
    for line in text.splitlines():
        if not line or line.startswith("# HELP ") or line.startswith("# TYPE "):
            continue
        m = METRIC_RE.match(line)
        if not m:
            bad.append(line)
            continue
        name, labels, value = m.groups()
        lab = ()
        if labels:
            pairs = LABEL_RE.findall(labels)
            if "".join(f'{k}="{v}",' for k, v in pairs).rstrip(",") != labels:
                bad.append(line)
                continue
            lab = tuple(sorted(pairs))
        try:
            samples[(name, lab)] = float(value)
        except ValueError:
            bad.append(line)
    return samples, bad


def metric(samples, name, **labels):
    return samples.get((name, tuple(sorted(labels.items()))))


class HwmonSink:
    """A FIFO at path standing in for /dev/wireview-hwmon. It is held open
    read-write, so the daemon's open(O_WRONLY) never blocks and a daemon
    close is no EOF here; a thread collects every byte written."""

    def __init__(self, path):
        os.mkfifo(path, 0o600)
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        self._data = bytearray()
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.is_set():
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    chunk = os.read(self.fd, 65536)
                except BlockingIOError:
                    continue
                with self._lock:
                    self._data += chunk

    def size(self):
        with self._lock:
            return len(self._data)

    def data(self):
        with self._lock:
            return bytes(self._data)

    def stop(self):
        time.sleep(0.1)     # let the thread drain what is left
        self._stop.set()
        self._thread.join(2)
        os.close(self.fd)


def read_frames(data):
    """Split the hwmon bytes into frames; returns (frames, error)."""
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


def ctl(tmp, *args, env=None):
    return subprocess.run([os.path.join(tmp, "wireviewctl"), *args],
                          capture_output=True, text=True, timeout=10,
                          env=env)


def fake_sysfs(tmp):
    """A hwmon directory as a current wireview_hwmon publishes it for PINS."""
    d = os.path.join(tmp, "sysfs", "hwmon9")
    os.makedirs(d)
    attrs = {"name": "wireview", "in6_input": 12016, "in7_input": 3300,
             "curr7_input": sum(ma for _, ma in PINS),
             "power1_input": sum(mv * ma for mv, ma in PINS),
             "temp1_input": 35500, "temp2_input": 40000,
             "pwm1": 107,                     # 42 %
             "fan1_input": 99,                # deprecated, must be ignored
             "power1_cap": 600000000,         # uW
             "psu_cap": 3,                    # deprecated, must be ignored
             "energy1_input": 5250000,        # 5.25 J
             "fault_status_raw": 0, "fault_log_raw": 0}
    for i, (mv, ma) in enumerate(PINS):
        attrs[f"in{i}_input"] = mv
        attrs[f"curr{i + 1}_input"] = ma
        attrs[f"power{i + 2}_input"] = mv * ma
    for name, value in attrs.items():
        with open(os.path.join(d, name), "w") as f:
            f.write(f"{value}\n")
    return d


def daemon_env():
    env = dict(os.environ)
    for k in ("WIREVIEW_LISTEN", "WIREVIEW_SECRET", "WIREVIEW_HWMON_PATH"):
        env.pop(k, None)
    return env


def stop_daemon(daemon, what):
    if daemon.poll() is None:
        daemon.send_signal(signal.SIGTERM)
        try:
            rc = daemon.wait(5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            rc = daemon.wait()
        check(rc == 0, f"{what} exits 0 on SIGTERM", f"exit status {rc}")


def run(tmp):
    hwmon = HwmonSink(os.path.join(tmp, "hwmon"))
    sock = os.path.join(tmp, "wireviewd.sock")

    # LAN listener on loopback only; the secret enables POST /command.
    port = free_port()
    with open(os.path.join(tmp, "config"), "w") as f:
        f.write(f"remote_enabled=1\nbind=127.0.0.1\nport={port}\n"
                f"secret={SECRET}\n")

    dev = fd.FakeWireView(fw_version=FW_VERSION, uid=UID, build=BUILD,
                          sensors=fd.pack_sensors(pins=PINS))
    dev.start(link=os.path.join(tmp, "ttyWV"))
    out = open(os.path.join(tmp, "daemon.out"), "w")
    daemon = subprocess.Popen(
        [os.path.join(tmp, "wireviewd"), "-i", "100", "-d", dev.path],
        env=daemon_env(), stdout=out, stderr=subprocess.STDOUT)
    unplug_at = 0       # frames written before the device went away
    try:
        exercise(tmp, dev, daemon, hwmon, sock)
        exercise_config(tmp, dev, sock, port)
        exercise_http(tmp, dev, daemon, port)
        exercise_ctl_json(tmp, port)
        exercise_idle(tmp, dev, sock)
        exercise_client_limit(tmp, sock)
        unplug_at = exercise_unplug(tmp, dev, daemon, hwmon, sock, port)
    finally:
        stop_daemon(daemon, "daemon")
        out.close()
        dev.stop()
        hwmon.stop()
    log = open(os.path.join(tmp, "daemon.out")).read()
    check("wireviewd: stopped" in log, "daemon logs a clean stop")
    check(f"wireviewd: WireView Pro II (EF05), FW v{FW_VERSION}, config v2"
          in log, "daemon read the fake device info at connect and names "
          "the edition")
    check(not dev.unknown, "daemon sent only known command bytes",
          f"unknown: {dev.unknown}")

    # The frames written to the "hwmon device" after the daemon stopped.
    frames, err = read_frames(hwmon.data())
    check(err is None and len(frames) >= 3,
          "hwmon file holds whole frames with magic and a known version",
          err or f"{len(frames)} frames")
    if frames:
        mv = struct.unpack_from("<6i", frames[0], OFF_VOLTAGE)
        ma = struct.unpack_from("<6i", frames[0], OFF_CURRENT)
        check(mv == tuple(p[0] for p in PINS) and
              ma == tuple(p[1] for p in PINS),
              "first frame carries the fake per-pin voltage and current",
              f"mV {mv} mA {ma}")
        # The corrupt frame had vdd 4321 and fan 105; nothing past the
        # sanity check may reach hwmon. Same offsets in v2 and v3.
        vdd_fan = [struct.unpack_from("<iB", f, OFF_VDD) for f in frames]
        check(all(v == (3300, 42) for v in vdd_fan),
              "no corrupt frame reached hwmon",
              f"vdd/fan seen: {sorted(set(vdd_fan))}")
        version = struct.unpack_from("<I", frames[0], 4)[0]
        check(version == 3 and len(frames[0]) == 156,
              "the daemon writes v3 (156-byte) records",
              f"version {version}, {len(frames[0])} bytes")
        if version == 3:
            energy = [struct.unpack_from("<q", f, OFF_ENERGY)[0]
                      for f in frames]
            check(energy[0] == 0,
                  "the first frame carries no energy (clock just started)",
                  str(energy[:3]))
            check(all(b >= a for a, b in zip(energy, energy[1:])) and
                  energy[-1] > 0,
                  "energy_uj grows and never goes back, across the reconnect",
                  f"first {energy[:3]} last {energy[-3:]}")
            k = unplug_at
            check(0 < k < len(energy) and energy[k] == energy[k - 1] and
                  energy[k + 1] > energy[k],
                  "after a reconnect energy resumes from where it was, "
                  "without counting the time the device was gone",
                  f"around frame {k}: {energy[max(k - 2, 0):k + 3]}")
            # ~484 W polled every 100 ms or a little more: about 48 J
            # per step (the upper bound leaves room for a loaded runner).
            steps = [b - a for a, b in zip(energy, energy[1:]) if b > a]
            check(steps and 40e6 < sorted(steps)[len(steps) // 2] < 500e6,
                  "a typical energy step is PINS power x the poll interval",
                  f"median step {sorted(steps)[len(steps) // 2] if steps else None} uJ")


def exercise(tmp, dev, daemon, hwmon, sock):
    if not check(wait_until(lambda: os.path.exists(sock) or
                            daemon.poll() is not None, 10) and
                 daemon.poll() is None,
                 "daemon starts and opens its socket"):
        raise RuntimeError("daemon did not come up")

    frame_min = min(FRAME_SIZE.values())
    check(wait_until(lambda: hwmon.size() >= 3 * frame_min, 5),
          "daemon writes sensor frames to the hwmon device",
          f"{hwmon.size()} bytes")

    # GET_DEVICE_INFO over the socket: fw, config version, uid, build.
    status, data = request(sock, WCMD_GET_DEVICE_INFO)
    check(status == RESP_OK and len(data) >= 14 and data[0] == FW_VERSION,
          "GET_DEVICE_INFO returns the fake firmware version",
          f"status {status} data {data!r}")
    check(data[1:2] == b"\x02" and data[2:14] == UID,
          "GET_DEVICE_INFO returns the config version and UID")
    check(data[14:].split(b"\0")[0] == BUILD,
          "GET_DEVICE_INFO returns the build string", repr(data[14:]))
    check(data[14:] == BUILD + b"\0\xef\x05",
          "GET_DEVICE_INFO ends with vendor EF, product 05 after the "
          "build string's NUL", repr(data[14:]))

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

    # Privileged commands: this daemon takes the test user as privileged
    # (see build()); exercise_unprivileged() covers the denial.
    status, _ = request(sock, WCMD_NVM_CMD, b"\x02")
    check(status == RESP_OK and dev.wait_for(
          lambda: dev.writes_of(fd.CMD_NVM_CONFIG)) ==
          [b"\x55\xaa\x55\xaa\x02"],
          "privileged NVM store reaches the device", f"status {status}")

    # The daemon locks the port (TIOCEXCL); a handover lifts the lock.
    def open_errno():
        try:
            os.close(os.open(dev.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK))
            return 0
        except OSError as e:
            return e.errno
    err = open_errno()
    check(err == errno.EBUSY, "a second open of the port is refused while "
          "the daemon polls", os.strerror(err) if err else "opened")
    status, _ = request(sock, WCMD_SUSPEND_SERIAL, struct.pack("<H", 5))
    err = open_errno()
    check(status == RESP_OK and err == 0,
          "the port opens during a SUSPEND_SERIAL handover",
          f"status {status}, {os.strerror(err) if err else 'opened'}")
    status, _ = request(sock, WCMD_RESUME_SERIAL)
    err = open_errno()
    check(status == RESP_OK and err == errno.EBUSY,
          "the port is locked again after RESUME_SERIAL",
          f"status {status}, {os.strerror(err) if err else 'opened'}")

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


def config_pattern(size, version, seed):
    """A config image with every byte position distinct from the last
    one written; byte 2 is the ConfigStruct version, as on the device."""
    b = bytearray((i * seed + 11) & 0xFF for i in range(size))
    b[2] = version
    return bytes(b)


def check_fake_reassembly():
    """The fake device's WRITE_CONFIG framing must not depend on how the
    bytes happen to be split across read() calls."""
    for ver, size in sorted(fd.CONFIG_SIZE.items()):
        want = config_pattern(size, ver, 53)
        stream = bytearray()
        for off in range(0, size, fd.CONFIG_CHUNK):
            stream += bytes([fd.CMD_WRITE_CONFIG, off]) + \
                want[off:off + fd.CONFIG_CHUNK]
        stream += bytes([fd.CMD_READ_CONFIG])
        bad = []
        for split in range(0, len(stream) + 1):
            dev = fd.FakeWireView(config_version=ver)
            reply = dev.feed(stream[:split]) + dev.feed(stream[split:])
            if not (reply == want and dev.configs_written == [want] and
                    not dev.unknown):
                bad.append(split)
        dev = fd.FakeWireView(config_version=ver)
        reply = b"".join(dev.feed(stream[i:i + 1])
                         for i in range(len(stream)))
        if reply != want:
            bad.append("bytewise")
        check(not bad, f"fake device reassembles a v{ver} ({size}-byte) "
              "config write however the stream is split",
              f"bad splits: {bad[:5]}")


def exercise_config(tmp, dev, sock, port):
    """Config writes over the socket, wireviewctl and POST /command, each
    read back from the device; the test user is privileged here."""
    size, ver = len(dev.config), dev.config_version
    check(size == 96 and ver == 2, "fake device has a 96-byte v2 config")
    written = len(dev.configs_written)

    new = config_pattern(size, ver, 37)
    before = len(dev.writes_of(fd.CMD_WRITE_CONFIG))
    status, _ = request(sock, WCMD_WRITE_CONFIG, bytes([ver]) + new)
    check(status == RESP_OK and
          dev.wait_for(lambda: bytes(dev.config) == new),
          "privileged WRITE_CONFIG reaches the device byte-exact",
          f"status {status}")
    frames = dev.writes_of(fd.CMD_WRITE_CONFIG)[before:]
    check(frames == [bytes([0]) + new[:62], bytes([62]) + new[62:]],
          "the config goes out as frames at offset 0 (62 bytes) and 62 "
          "(34 bytes)", str([(f[0], len(f) - 1) for f in frames]))
    status, data = request(sock, WCMD_READ_CONFIG)
    check(status == RESP_OK and data == bytes([ver]) + new,
          "READ_CONFIG returns the config just written",
          f"status {status} {data.hex()}")

    new2 = config_pattern(size, ver, 101)
    path = os.path.join(tmp, "config2.hex")
    with open(path, "w") as f:
        f.write(new2.hex() + "\n")
    r = ctl(tmp, "write-config", path)
    check(r.returncode == 0 and
          "config written (96 bytes, version 2)" in r.stdout and
          dev.wait_for(lambda: bytes(dev.config) == new2),
          "wireviewctl write-config writes the file's config byte-exact",
          r.stdout + r.stderr)
    r = ctl(tmp, "read-config")
    check(r.returncode == 0 and r.stdout.strip() == new2.hex(),
          "wireviewctl read-config prints it back", r.stdout + r.stderr)

    # The signed LAN path: POST /command writeConfig, then GET /config.
    new3 = config_pattern(size, ver, 199)
    path = os.path.join(tmp, "config3.hex")
    with open(path, "w") as f:
        f.write(new3.hex() + "\n")
    host = f"127.0.0.1:{port}"
    env = dict(os.environ, WIREVIEW_SECRET=SECRET)
    r = ctl(tmp, "--host", host, "write-config", path, env=env)
    check(r.returncode == 0 and
          dev.wait_for(lambda: bytes(dev.config) == new3),
          "wireviewctl --host write-config (signed POST /command) writes "
          "the config byte-exact", r.stdout + r.stderr)
    r = ctl(tmp, "--host", host, "read-config")
    check(r.returncode == 0 and r.stdout.strip() == new3.hex(),
          "GET /config (wireviewctl --host read-config) returns it",
          r.stdout + r.stderr)
    check(dev.configs_written[written:] == [new, new2, new3] and
          not dev.unknown,
          "the device saw exactly three complete config writes",
          f"{len(dev.configs_written) - written} writes, "
          f"unknown {dev.unknown}")


def start_edition(tmp, product_id, listen, name):
    """Serve a fake device of product_id to the main test build, now that
    run() has stopped its daemon (the build's paths are fixed at compile
    time). Returns (daemon, dev, hwmon, out, port); port is None unless
    listen."""
    hwpath = os.path.join(tmp, "hwmon")
    if os.path.exists(hwpath):
        os.unlink(hwpath)
    hwmon = HwmonSink(hwpath)
    port = free_port() if listen else None
    with open(os.path.join(tmp, "config"), "w") as f:
        f.write(f"remote_enabled=1\nbind=127.0.0.1\nport={port}\n"
                if listen else "remote_enabled=0\n")
    dev = fd.FakeWireView(fw_version=FW_VERSION, uid=UID, build=BUILD,
                          product_id=product_id,
                          sensors=fd.pack_sensors(pins=PINS))
    dev.start()
    out = open(os.path.join(tmp, name), "w")
    daemon = subprocess.Popen(
        [os.path.join(tmp, "wireviewd"), "-i", "100", "-d", dev.path],
        env=daemon_env(), stdout=out, stderr=subprocess.STDOUT)
    return daemon, dev, hwmon, out, port


def run_noctua(tmp):
    """The Noctua Edition (product 6) connects like the Pro II and is
    reported as itself on the socket, /sensors and /metrics."""
    sock = os.path.join(tmp, "wireviewd.sock")
    daemon, dev, hwmon, out, port = start_edition(tmp, 6, True,
                                                  "daemon6.out")
    try:
        if not check(wait_until(lambda: (os.path.exists(sock) and
                                         hwmon.size() >= 3 * 156) or
                                daemon.poll() is not None, 10) and
                     daemon.poll() is None,
                     "Noctua Edition (EF06): daemon connects and writes "
                     "hwmon frames"):
            return
        status, data = request(sock, WCMD_GET_DEVICE_INFO)
        check(status == RESP_OK and data[0] == FW_VERSION and
              data[2:14] == UID and data[14:] == BUILD + b"\0\xef\x06",
              "Noctua Edition: GET_DEVICE_INFO ends with EF 06 after the "
              "build string's NUL", f"{status} {data!r}")
        # An old client stops at the NUL: wireviewctl info shows the build
        # string alone, with none of the trailing bytes.
        r = ctl(tmp, "info")
        check(r.returncode == 0 and
              f"\nbuild: {BUILD.decode()}\n" in r.stdout and
              "\xef" not in r.stdout and "\x06" not in r.stdout,
              "Noctua Edition: wireviewctl info prints the build string "
              "cleanly", repr(r.stdout + r.stderr))

        status, _, body = http_get(port, "/sensors")
        doc = json.loads(body) if status == 200 else {}
        d = (doc.get("devices") or [{}])[0]
        check(d.get("hwRev") == "EF06" and
              d.get("name") == "WireView Pro II Noctua Edition" and
              d.get("id") == UID_HEX and d.get("buildString") ==
              BUILD.decode(),
              "Noctua Edition: GET /sensors hwRev EF06, name WireView Pro "
              "II Noctua Edition", body[:300])
        _, _, body = http_get(port, "/metrics")
        samples, bad = parse_metrics(body)
        check(not bad and
              metric(samples, "wireview_firmware_info", device=UID_HEX,
                     version=str(FW_VERSION), build=BUILD.decode(),
                     product="EF06",
                     edition="WireView Pro II Noctua Edition") == 1,
              "Noctua Edition: wireview_firmware_info has product EF06 "
              "and the edition", f"bad {bad[:3]}")
    finally:
        stop_daemon(daemon, "Noctua Edition daemon")
        out.close()
        dev.stop()
        hwmon.stop()
    log = open(os.path.join(tmp, "daemon6.out")).read()
    check("wireviewd: WireView Pro II Noctua Edition (EF06), "
          f"FW v{FW_VERSION}, config v2" in log,
          "Noctua Edition: the connect line names the edition", log[-400:])
    check(not dev.unknown, "Noctua Edition: only known command bytes sent",
          f"unknown: {dev.unknown}")


def run_wireview2(tmp):
    """A WireView II (product 7) is refused with one clear message, however
    often the daemon retries, and nothing reaches hwmon."""
    sock = os.path.join(tmp, "wireviewd.sock")
    msg = "WireView II (product 7) is not supported yet"
    daemon, dev, hwmon, out, _ = start_edition(tmp, 7, False,
                                               "daemon7.out")
    try:
        # The daemon retries every 2 s: wait for a second attempt.
        check(wait_until(lambda: dev.vendor_queries >= 2 or
                         daemon.poll() is not None, 6) and
              daemon.poll() is None,
              "WireView II: daemon keeps running and retries",
              f"{dev.vendor_queries} vendor-data queries")
        status, _ = request(sock, WCMD_GET_DEVICE_INFO)
        check(status == RESP_NOT_CONNECTED,
              "WireView II: GET_DEVICE_INFO gets status 2", str(status))
    finally:
        stop_daemon(daemon, "WireView II daemon")
        out.close()
        dev.stop()
        hwmon.stop()
    log = open(os.path.join(tmp, "daemon7.out")).read()
    check(log.count(msg) == 1 and log.count("wireviewd: using ") == 1 and
          "device info query failed" not in log,
          "WireView II: one 'not supported yet' line across the retries",
          log[-600:])
    check(audit_log(tmp).count(msg) == 1,
          "WireView II: the audit log has it once too")
    check(hwmon.size() == 0 and dev.polls == 0 and not dev.writes,
          "WireView II: no sensor read, no frame written to hwmon",
          f"{hwmon.size()} hwmon bytes, {dev.polls} polls")


def run_unprivileged(tmp):
    """A daemon built with a WIREVIEW_GROUP that does not exist, so the
    test user is an unprivileged peer: privileged commands get status 3
    and never reach its (separate) fake device."""
    if os.getuid() == 0:
        print("skip unprivileged-peer checks: running as root")
        return
    utmp = os.path.join(tmp, "u")
    os.mkdir(utmp)
    build(utmp, NO_GROUP, programs=("wireviewd",))
    with open(os.path.join(utmp, "config"), "w") as f:
        f.write("remote_enabled=0\n")
    hwmon = HwmonSink(os.path.join(utmp, "hwmon"))
    sock = os.path.join(utmp, "wireviewd.sock")
    dev = fd.FakeWireView(fw_version=FW_VERSION, uid=UID, build=BUILD)
    dev.start()
    out = open(os.path.join(utmp, "daemon.out"), "w")
    daemon = subprocess.Popen(
        [os.path.join(utmp, "wireviewd"), "-i", "100", "-d", dev.path],
        env=daemon_env(), stdout=out, stderr=subprocess.STDOUT)
    try:
        if not check(wait_until(lambda: (os.path.exists(sock) and
                                         dev.polls >= 2) or
                                daemon.poll() is not None, 10) and
                     daemon.poll() is None,
                     "unprivileged-peer daemon starts and polls its device"):
            return
        ver = dev.config_version
        orig = bytes(dev.config)
        new = config_pattern(len(orig), ver, 37)
        status, _ = request(sock, WCMD_WRITE_CONFIG, bytes([ver]) + new)
        check(status == RESP_DENIED,
              "WRITE_CONFIG from an unprivileged peer gets status 3",
              f"status {status}")
        status, data = request(sock, WCMD_READ_CONFIG)
        check(status == RESP_OK and data == bytes([ver]) + orig,
              "READ_CONFIG (open to everyone) shows the config unchanged",
              f"status {status}")
        denied = {name: request(sock, cmd, payload)[0]
                  for name, cmd, payload in (
                      ("NVM", WCMD_NVM_CMD, b"\x02"),
                      ("ENTER_BOOTLOADER", WCMD_ENTER_BOOTLOADER, b""),
                      ("SUSPEND_SERIAL", WCMD_SUSPEND_SERIAL,
                       struct.pack("<H", 5)),
                      ("RESUME_SERIAL", WCMD_RESUME_SERIAL, b""))}
        check(all(s == RESP_DENIED for s in denied.values()),
              "NVM, bootloader and serial handover get status 3 too",
              str(denied))
        polls = dev.polls
        check(dev.wait_for(lambda: dev.polls >= polls + 3),
              "polling carries on: no handover was granted")
        check(not dev.writes and not dev.unknown,
              "nothing an unprivileged peer sent reached the device",
              f"writes {dev.writes} unknown {dev.unknown}")
        check(wait_until(lambda: f"socket cmd 0x04 from uid {os.getuid()} "
                         "denied" in audit_log(utmp), 2),
              "the denied config write is audited with the peer's uid")
    finally:
        stop_daemon(daemon, "unprivileged-peer daemon")
        out.close()
        dev.stop()
        hwmon.stop()


def exercise_http(tmp, dev, daemon, port):
    log = open(os.path.join(tmp, "daemon.out")).read()
    check(f"network listener ENABLED on 127.0.0.1:{port}" in log,
          "config bind= and port= place the listener on 127.0.0.1",
          log[-400:])

    status, hdrs, body = http_get(port, "/sensors")
    doc = json.loads(body) if status == 200 else {}
    devs = doc.get("devices") or [{}]
    d = devs[0]
    check(status == 200 and len(devs) == 1 and d.get("id") == UID_HEX and
          d.get("fwVer") == str(FW_VERSION) and
          d.get("buildString") == BUILD.decode(),
          "GET /sensors describes the fake device", body[:300])
    check(d.get("hwRev") == "EF05" and d.get("name") == "WireView Pro II",
          "GET /sensors: hwRev EF05, name WireView Pro II", body[:300])
    check(d.get("pinVoltage") == [mv / 1000 for mv, _ in PINS] and
          d.get("pinCurrent") == [ma / 1000 for _, ma in PINS],
          "GET /sensors carries the pin readings", body[:300])
    check("energyJ" in d and list(d)[-1] == "energyJ",
          "GET /sensors ends with energyJ", str(list(d)))
    check(wait_until(lambda: json.loads(http_get(port, "/sensors")[2])
                     ["devices"][0]["energyJ"] > 0, 3),
          "energyJ counts up while the device reports")

    status, hdrs, body = http_get(port, "/metrics")
    samples, bad = parse_metrics(body)
    check(status == 200 and hdrs.get("Content-Type", "").startswith(
          "text/plain; version=0.0.4"),
          "GET /metrics answers in the Prometheus text format",
          f"{status} {hdrs.get('Content-Type')}")
    check(not bad and len(samples) > 20, "every /metrics sample line parses",
          f"{len(samples)} samples, bad: {bad[:3]}")
    check(metric(samples, "wireview_up", device=UID_HEX) == 1,
          "wireview_up is 1 with the device UID label")
    check(metric(samples, "wireview_pin_voltage_volts", device=UID_HEX,
                 pin="2") == 12.05 and
          metric(samples, "wireview_pin_current_amps", device=UID_HEX,
                 pin="5") == 0.0,
          "per-pin metrics carry the fake readings")
    check((metric(samples, "wireview_energy_joules_total", device=UID_HEX)
           or 0) > 0 and
          "# TYPE wireview_energy_joules_total counter" in body,
          "wireview_energy_joules_total is a counter above 0")
    check(metric(samples, "wireview_firmware_info", device=UID_HEX,
                 version=str(FW_VERSION), build=BUILD.decode(),
                 product="EF05", edition="WireView Pro II") == 1,
          "wireview_firmware_info labels the version, build string, "
          "product and edition")
    check(metric(samples, "wireview_temperature_celsius", device=UID_HEX,
                 sensor="onboard_in") == 35.5 and
          metric(samples, "wireview_temperature_celsius", device=UID_HEX,
                 sensor="external_1") is None,
          "disconnected temperature sensors are left out of /metrics")

    status, _, _ = http_get(port, "/nope")
    check(status == 404, "an unknown path is 404", str(status))


def exercise_ctl_json(tmp, port):
    """wireviewctl sensors [--json] locally (fake sysfs) and over --host."""
    _, _, body = http_get(port, "/sensors")
    daemon_doc = json.loads(body)
    want_top = list(daemon_doc)
    want_dev = list(daemon_doc["devices"][0])

    env = dict(os.environ, WIREVIEW_HWMON_PATH=fake_sysfs(tmp))
    r = ctl(tmp, "sensors", "--json", env=env)
    try:
        doc = json.loads(r.stdout)
    except ValueError as e:
        doc = {}
        check(False, "wireviewctl sensors --json prints JSON",
              f"{e}: {r.stdout[:300]!r} {r.stderr}")
    devs = doc.get("devices") or [{}]
    d = devs[0]
    check(r.returncode == 0 and len(devs) == 1,
          "wireviewctl sensors --json reads the fake sysfs device",
          f"rc {r.returncode} {r.stdout[:200]} {r.stderr}")
    check(list(doc) == want_top and list(d) == want_dev,
          "sensors --json has the daemon's /sensors keys, in its order",
          f"\n  ctl    {list(d)}\n  daemon {want_dev}")
    check(doc.get("appVersion") == "wireviewctl" and d.get("connected") is True,
          "sensors --json says who wrote it and that the device is live")
    check(d.get("pinVoltage") == [mv / 1000 for mv, _ in PINS] and
          d.get("pinCurrent") == [ma / 1000 for _, ma in PINS] and
          d.get("tempInC") == 35.5 and d.get("ext1C") == 0.0,
          "sensors --json converts sysfs units back to V, A and degC",
          json.dumps(d)[:300])
    check(d.get("fan") == 42 and d.get("psuCapW") == 600 and
          d.get("energyJ") == 5.25,
          "sensors --json: pwm1 as percent, power1_cap in W, energy in J",
          f"fan {d.get('fan')} psuCapW {d.get('psuCapW')} "
          f"energyJ {d.get('energyJ')}")
    # id, fwVer and buildString come from the running daemon's socket.
    check(d.get("id") == UID_HEX and d.get("fwVer") == str(FW_VERSION) and
          d.get("buildString") == BUILD.decode(),
          "sensors --json fills id/fwVer/build from the daemon socket",
          f"{d.get('id')} {d.get('fwVer')} {d.get('buildString')}")

    r = ctl(tmp, "sensors", env=env)
    check(r.returncode == 0 and "pin2_voltage_mv: 12050" in r.stdout and
          "fan_duty: 42" in r.stdout and "psu_cap: 600W" in r.stdout and
          "energy_uj: 5250000" in r.stdout,
          "wireviewctl sensors prints the fake sysfs device", r.stdout)

    empty = os.path.join(tmp, "sysfs", "empty")
    os.makedirs(empty)
    r = ctl(tmp, "sensors", "--json",
            env=dict(os.environ, WIREVIEW_HWMON_PATH=empty))
    try:
        doc = json.loads(r.stdout)
    except ValueError:
        doc = None
    check(r.returncode == 1 and doc is not None and doc["devices"] == [],
          "sensors --json without a hwmon device: valid JSON, no devices",
          f"rc {r.returncode} {r.stdout!r}")

    # --host: the daemon's own document, and parse_remote on it.
    host = f"127.0.0.1:{port}"
    r = ctl(tmp, "--host", host, "sensors", "--json")
    try:
        doc = json.loads(r.stdout)
    except ValueError:
        doc = {}
    check(r.returncode == 0 and doc.get("appVersion") == "wireviewd" and
          list(doc.get("devices", [{}])[0]) == want_dev,
          "wireviewctl --host sensors --json passes /sensors through",
          f"rc {r.returncode} {r.stdout[:200]} {r.stderr}")
    r = ctl(tmp, "--host", host, "sensors")
    check(r.returncode == 0 and "pin1_voltage_mv: 12000" in r.stdout and
          "pin5_current_ma: 0" in r.stdout and
          "pin2_power_uw: 97605000" in r.stdout and
          "temp_onboard_in_mc: 35500" in r.stdout and
          "temp_external1_mc" not in r.stdout and
          "psu_cap: 600W" in r.stdout and
          re.search(r"^energy_uj: [1-9]\d*$", r.stdout, re.M),
          "wireviewctl --host sensors reads the daemon's readings",
          r.stdout + r.stderr)


def exercise_idle(tmp, dev, sock):
    """Idle clients are closed after IDLE_S; a busy client is not, nor is
    the one holding a serial handover until the handover ends. All three
    run side by side on one clock."""
    idle, busy, owner = Client(sock), Client(sock), Client(sock)
    try:
        s_idle, _ = idle.request(WCMD_GET_DEVICE_INFO)
        s_owner, _ = owner.request(WCMD_SUSPEND_SERIAL, struct.pack("<H", 3))
        t0 = time.monotonic()
        polls = dev.polls
        check(s_idle == RESP_OK and s_owner == RESP_OK,
              "idle test: clients connected, the owner holds a 3 s handover",
              f"{s_idle} {s_owner}")
        busy_status, closed_at, next_busy = [], {}, 0.0
        polls_during = None     # sensor polls 0.5 s past the idle timeout
        while time.monotonic() - t0 < IDLE_S + 3:
            now = time.monotonic() - t0
            if polls_during is None and now >= IDLE_S + 0.5:
                polls_during = dev.polls - polls
            if now >= next_busy:
                try:
                    busy_status.append(busy.request(WCMD_GET_DEVICE_INFO)[0])
                except (OSError, EOFError) as e:
                    busy_status.append(repr(e))
                next_busy = now + IDLE_S / 4
            for name, c in (("idle", idle), ("owner", owner)):
                if name not in closed_at and c.closed_by_daemon():
                    closed_at[name] = time.monotonic() - t0
            if len(closed_at) == 2 and now > IDLE_S + 0.5:
                break
            time.sleep(0.05)
        t = closed_at.get("idle")
        check(t is not None and IDLE_S - 0.3 <= t <= IDLE_S + 1.0,
              f"an idle client is closed after {IDLE_S} s", f"closed at {t}")
        check(busy_status and all(s == RESP_OK for s in busy_status) and
              not busy.closed_by_daemon(),
              "a client sending a request every IDLE_S/4 stays connected",
              str(busy_status))
        t = closed_at.get("owner")
        check(t is not None and 3.0 - 0.3 <= t <= 3.0 + 1.0,
              "the handover owner is kept past the idle timeout, then "
              "closed when its 3 s handover ends", f"closed at {t}")
        check(polls_during is not None and polls_during <= 1 and
              dev.wait_for(lambda: dev.polls >= polls + polls_during + 3),
              "polling pauses for the handover and resumes after it",
              f"{polls_during} polls during")
        uid = os.getuid()
        n = len(re.findall(rf"\[INFO\] socket client uid {uid}: idle for "
                           r"\d+ s, disconnecting$", audit_log(tmp), re.M))
        check(n == 2, "each idle close is audited with the uid",
              f"{n} lines")
        status, _ = busy.request(WCMD_SCREEN_CMD, b"\xe1")
        check(status == RESP_OK and
              dev.wait_for(lambda: b"\xe1" in dev.writes_of(
                  fd.CMD_SCREEN_CHANGE)),
              "the busy client still reaches the device afterwards",
              str(status))
    finally:
        for c in (idle, busy, owner):
            c.close()


def exercise_client_limit(tmp, sock):
    """MAX_CLIENTS connections are served at once; one more replaces the
    client idle longest, and the socket keeps serving."""
    clients = []
    try:
        statuses = []
        for _ in range(MAX_CLIENTS):
            c = Client(sock)
            clients.append(c)
            statuses.append(c.request(WCMD_GET_DEVICE_INFO)[0])
        check(statuses == [RESP_OK] * MAX_CLIENTS,
              f"{MAX_CLIENTS} clients connected at once are all served "
              "(the 5th to 8th were refused before)", str(statuses))
        # clients[0] is the oldest connection but the most recently
        # active, so the longest idle one is clients[1].
        clients[0].request(WCMD_GET_DEVICE_INFO)
        before = audit_log(tmp).count("disconnecting to make room")
        ninth = Client(sock)
        clients.append(ninth)
        status, _ = ninth.request(WCMD_GET_DEVICE_INFO)
        check(status == RESP_OK, f"connection {MAX_CLIENTS + 1} is served",
              str(status))
        check(clients[1].closed_by_daemon(1.0),
              "it replaced the client idle longest")
        others = []
        for i, c in enumerate(clients):
            if i == 1:
                continue
            try:
                others.append(c.request(WCMD_GET_DEVICE_INFO)[0])
            except (OSError, EOFError) as e:
                others.append(repr(e))
        check(others == [RESP_OK] * MAX_CLIENTS,
              "every other client is still connected and served",
              str(others))
        check(wait_until(lambda: audit_log(tmp).count(
                  "disconnecting to make room") == before + 1, 2) and
              re.search(rf"\[INFO\] socket client uid {os.getuid()}: idle "
                        r"for \d+ s, disconnecting to make room",
                        audit_log(tmp)),
              "the eviction is audited with the uid")
    finally:
        for c in clients:
            c.close()
    status, _ = request(sock, WCMD_GET_DEVICE_INFO)
    r = ctl(tmp, "info")
    check(status == RESP_OK and r.returncode == 0,
          "the socket keeps serving new connections afterwards",
          f"{status} {r.stderr}")


def exercise_unplug(tmp, dev, daemon, hwmon, sock, port):
    """The command socket and its clients outlive a device disappearance.
    Returns the number of frames written before the device went away."""
    size = 0

    def failures():
        return open(os.path.join(tmp, "daemon.out")).read().count(
            "read failed, reconnecting")

    client = Client(sock)

    def keepalive():
        """A request now and then, as the GUI's own traffic would be:
        the test build closes a client idle for IDLE_S."""
        client.request(WCMD_GET_DEVICE_INFO)
        return True

    try:
        status, _ = client.request(WCMD_GET_DEVICE_INFO)
        check(status == RESP_OK, "long-lived client: RESP_OK before unplug",
              str(status))

        before = failures()
        dev.unplug()
        check(wait_until(lambda: keepalive() and failures() > before, 5),
              "the daemon notices the device is gone")

        status, _ = client.request(WCMD_GET_DEVICE_INFO)
        check(status == RESP_NOT_CONNECTED,
              "long-lived client: status 2 while the device is gone",
              str(status))
        status, _ = client.request(WCMD_SCREEN_CMD, b"\xe0")
        check(status == RESP_NOT_CONNECTED,
              "a device command is refused with status 2, not relayed",
              str(status))
        check(os.path.exists(sock) and
              request(sock, WCMD_GET_DEVICE_INFO)[0] == RESP_NOT_CONNECTED,
              "new clients can still connect while the device is gone")
        _, _, body = http_get(port, "/metrics")
        samples, _ = parse_metrics(body)
        check(metric(samples, "wireview_up", device=UID_HEX) == 0 and
              len(samples) == 1,
              "/metrics: only wireview_up 0, still labelled with the UID",
              body)
        _, _, body = http_get(port, "/sensors")
        check(json.loads(body)["devices"] == [],
              "/sensors lists no device while it is gone", body)
        keepalive()
        time.sleep(0.2)     # let the sink drain the last frame
        size = hwmon.size()
        time.sleep(0.3)
        check(hwmon.size() == size,
              "no frames reach hwmon while the device is gone")
        keepalive()

        polls = dev.polls
        dev.replug()

        def back():
            return client.request(WCMD_GET_DEVICE_INFO)[0] == RESP_OK
        check(wait_until(back, 12),
              "long-lived client: RESP_OK again after the device is back")
        status, data = client.request(WCMD_GET_DEVICE_INFO)
        check(status == RESP_OK and data[0] == FW_VERSION and
              data[2:14] == UID,
              "GET_DEVICE_INFO on the same connection after the reconnect",
              f"{status} {data!r}")
        check(dev.wait_for(lambda: dev.polls >= polls + 3, 5) and
              wait_until(lambda: hwmon.size() >= size + 3 * 156, 5),
              "polling and hwmon frames resume on the new pty")
        _, _, body = http_get(port, "/metrics")
        samples, _ = parse_metrics(body)
        check(metric(samples, "wireview_up", device=UID_HEX) == 1,
              "/metrics: wireview_up back to 1")
    finally:
        client.close()
    check(daemon.poll() is None and dev.plugs == 2,
          "daemon still running after the unplug/replug")
    return size // 156


def main():
    tmp = make_tmpdir()
    try:
        group = primary_group()
        if group is None:
            print(f"e2e: gid {os.getgid()} has no name; privileged "
                  "checks will fail")
        build(tmp, group or NO_GROUP)
        check_fake_reassembly()
        for step in (run, run_noctua, run_wireview2, run_unprivileged):
            try:
                step(tmp)
            except Exception as e:  # report, then go on to the summary
                check(False, f"e2e {step.__name__}",
                      f"{type(e).__name__}: {e}")
        if failed:
            for name in ("daemon.out", "daemon6.out", "daemon7.out",
                         "u/daemon.out"):
                p = os.path.join(tmp, name)
                if os.path.exists(p):
                    print(f"--- {name}\n" + open(p).read())
            print("--- audit log\n" + audit_log(tmp))
            print("--- u/audit log\n" + audit_log(os.path.join(tmp, "u")))
    finally:
        if os.environ.get("WIREVIEW_E2E_KEEP"):
            print(f"e2e: kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f"e2e: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
