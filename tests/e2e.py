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

Environment: CC (default cc), SAN_FLAGS (extra compiler flags, e.g. ASan),
WIREVIEW_E2E_KEEP=1 keeps the temp dir.
"""

import glob
import grp
import http.client
import json
import os
import pwd
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
WCMD_SCREEN_CMD = 0x05
WCMD_NVM_CMD = 0x06
RESP_OK, RESP_NOT_CONNECTED, RESP_DENIED = 0, 2, 3

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
                      ("wireviewctl", ["wireviewctl.c", "sha256.c"])):
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


def run(tmp):
    hwmon = HwmonSink(os.path.join(tmp, "hwmon"))
    sock = os.path.join(tmp, "wireviewd.sock")

    # LAN listener on loopback only, read-only (no secret).
    port = free_port()
    with open(os.path.join(tmp, "config"), "w") as f:
        f.write(f"remote_enabled=1\nbind=127.0.0.1\nport={port}\n")

    dev = fd.FakeWireView(fw_version=FW_VERSION, uid=UID, build=BUILD,
                          sensors=fd.pack_sensors(pins=PINS))
    dev.start(link=os.path.join(tmp, "ttyWV"))
    env = dict(os.environ)
    for k in ("WIREVIEW_LISTEN", "WIREVIEW_SECRET", "WIREVIEW_HWMON_PATH"):
        env.pop(k, None)
    out = open(os.path.join(tmp, "daemon.out"), "w")
    daemon = subprocess.Popen(
        [os.path.join(tmp, "wireviewd"), "-i", "100", "-d", dev.path],
        env=env, stdout=out, stderr=subprocess.STDOUT)
    unplug_at = 0       # frames written before the device went away
    try:
        exercise(tmp, dev, daemon, hwmon, sock)
        exercise_http(tmp, dev, daemon, port)
        exercise_ctl_json(tmp, port)
        unplug_at = exercise_unplug(tmp, dev, daemon, hwmon, sock, port)
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
        hwmon.stop()
    log = open(os.path.join(tmp, "daemon.out")).read()
    check("wireviewd: stopped" in log, "daemon logs a clean stop")
    check(f"FW v{FW_VERSION}, config v2" in log,
          "daemon read the fake device info at connect")
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
                 version=str(FW_VERSION), build=BUILD.decode()) == 1,
          "wireview_firmware_info labels the version and build string")
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


def exercise_unplug(tmp, dev, daemon, hwmon, sock, port):
    """The command socket and its clients outlive a device disappearance.
    Returns the number of frames written before the device went away."""
    size = 0

    def failures():
        return open(os.path.join(tmp, "daemon.out")).read().count(
            "read failed, reconnecting")

    client = Client(sock)
    try:
        status, _ = client.request(WCMD_GET_DEVICE_INFO)
        check(status == RESP_OK, "long-lived client: RESP_OK before unplug",
              str(status))

        before = failures()
        dev.unplug()
        check(wait_until(lambda: failures() > before, 5),
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
        time.sleep(0.2)     # let the sink drain the last frame
        size = hwmon.size()
        time.sleep(0.3)
        check(hwmon.size() == size,
              "no frames reach hwmon while the device is gone")

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
