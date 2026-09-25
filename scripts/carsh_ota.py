#!/usr/bin/env python3
"""carsh OTA dev tool — keygen / pack / push / flash over the USB-C console.

The serial-push dev loop (no SD card swap):

    # once: make a dev signing key + provision its public key into the image
    scripts/carsh_ota.py keygen

    # each iteration: package the freshly built kernel, push it over the
    # USB-C serial console, let the kernel stage it to the inactive A/B slot
    # and reboot through the chainloader
    scripts/carsh_ota.py flash --image device/kernel8-rpi4.img

Wire format: PushProtocol.h framing (magic "CPSH", BEGIN/DATA/END/QUERY
packets with CRC-32) carrying a .carup package (Package.h container: signed
manifest + image). This script is the host-side mirror of those two headers —
change them together. Device feedback comes back as "ota-push: ..." log lines on
the same serial stream.

Flow control (PushProtocol.h): the kernel's CDC gadget buffers host->device
bytes in an 8 KiB ring with no back-pressure and drops whatever doesn't fit, so
this script never has more than MAX_IN_FLIGHT bytes on the wire beyond the
device's last "ota-push: ack <seq>" line, nudges a silent device with QUERY, and
restarts from BEGIN (--retries) when the device reports a failed receive.

The acks arrive as log lines, so nothing else may read the port during a push
(e.g. scripts/bench_capture.py — two readers split the stream and the acks go
missing); the script refuses to start if another process has the port open.
Use --log FILE to keep a raw capture of the console while pushing.

Only the Python stdlib + the `openssl` CLI (Ed25519 sign) are required.
"""

import argparse
import collections
import hashlib
import json
import os
import re
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_KEY = os.path.join(REPO, "ota_dev_key.pem")  # gitignored
DEFAULT_PORT = "/dev/ttyACM0"
KEY_HEADER = os.path.join(REPO, "device", "OtaPushKey.h")

MAGIC = b"CPSH"
TYPE_BEGIN, TYPE_DATA, TYPE_END, TYPE_QUERY = 1, 2, 3, 4
CHUNK = 4096                       # PushProtocol.h kMaxChunk
HEADER_LEN = 4 + 1 + 2 + 2         # kHeaderLen (magic..len); + 4-byte CRC trailer

# Flow control — mirrors PushProtocol.h (kDeviceRxQueue, kMaxInFlight,
# kAckPollMs, kAckTimeoutMs, kAckLogFormat); change them together.
DEVICE_RX_QUEUE = 8192             # Circle's CDC gadget RX ring (usable bytes)
MAX_IN_FLIGHT = 6144               # un-acked BEGIN/DATA/END bytes on the wire
ACK_POLL = 1.0                     # s without an ack before a QUERY / BEGIN re-send
ACK_TIMEOUT = 15.0                 # s without ack progress: abandon the attempt
ACK_RE = re.compile(r"ota-push: ack (\d+)")


# --- packet / package builders (mirror PushProtocol.h / Package.h) -----------

def packet(ptype: int, seq: int, payload: bytes) -> bytes:
    body = struct.pack("<BHH", ptype, seq, len(payload)) + payload
    return MAGIC + body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def build_carup(image: bytes, key_pem: str, version: str, build_id: str,
                min_prev: str = "", slot_hint: int = 0xFF) -> bytes:
    manifest = json.dumps({
        "version": version,
        "buildId": build_id,
        "minPrevVersion": min_prev,
        "slotImageSHA256": hashlib.sha256(image).hexdigest(),
    }).encode()
    # openssl 3.x pkeyutl -rawin needs a seekable input, so sign via a temp file.
    with tempfile.NamedTemporaryFile() as tf:
        tf.write(manifest)
        tf.flush()
        sig = subprocess.run(
            ["openssl", "pkeyutl", "-sign", "-inkey", key_pem, "-rawin",
             "-in", tf.name], capture_output=True, check=True).stdout
    if len(sig) != 64:
        sys.exit(f"error: openssl produced a {len(sig)}-byte signature "
                 "(need Ed25519/64 — is the key an ed25519 PEM?)")
    return (b"CARUP1" + bytes([slot_hint]) +
            struct.pack("<I", len(manifest)) + manifest + sig +
            struct.pack("<I", len(image)) + image)


def data_packets(carup: bytes, chunk: int = CHUNK):
    """[(seq, packet)] for the DATA packets + END, seq 1..n+1."""
    pkts = []
    seq = 1
    for off in range(0, len(carup), chunk):
        pkts.append((seq, packet(TYPE_DATA, seq, carup[off:off + chunk])))
        seq += 1
    pkts.append((seq, packet(TYPE_END, seq, b"")))
    return pkts


# --- serial helpers (raw termios; CDC ACM ignores baud) -----------------------

def open_port(port: str, wait: float = 0.0):
    """Open the ACM port raw. With wait>0, retry until it (re)appears."""
    deadline = time.monotonic() + wait
    while True:
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.2)
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[3] = 0        # iflag, oflag, lflag: fully raw
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def port_users(port: str):
    """Other processes holding `port` open, as [(pid, cmdline)] (Linux /proc;
    empty where that isn't available)."""
    try:
        target = os.path.realpath(port)
        pids = [p for p in os.listdir("/proc") if p.isdigit()]
    except OSError:
        return []
    users = []
    for pid in pids:
        if int(pid) == os.getpid():
            continue
        fddir = f"/proc/{pid}/fd"
        try:
            fds = os.listdir(fddir)
        except OSError:
            continue  # gone, or not ours to inspect
        for fd in fds:
            try:
                if os.readlink(os.path.join(fddir, fd)) == target:
                    break
            except OSError:
                continue
        else:
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                cmd = f.read().replace(b"\0", b" ").decode(errors="replace").strip()
        except OSError:
            cmd = "?"
        users.append((int(pid), cmd))
    return users


class PushError(Exception):
    """A failed push attempt. retryable: restarting from BEGIN may succeed."""

    def __init__(self, msg: str, retryable: bool = True):
        super().__init__(msg)
        self.retryable = retryable


def device_error(line: str) -> bool:
    return "ota-push:" in line and ("fail" in line or "err=" in line)


class DeviceLink:
    """The serial port as the push sees it: raw writes with a deadline, and the
    device log as a queue of lines (echoed unless quiet; ack lines are protocol
    and never echoed). tee: optional binary file getting the raw console."""

    def __init__(self, fd, quiet: bool = False, tee=None):
        self.fd = fd
        self.quiet = quiet
        self.tee = tee
        self._buf = bytearray()
        self._lines = collections.deque()

    def write(self, data: bytes, timeout: float = ACK_TIMEOUT):
        view = memoryview(data)
        off = 0
        deadline = time.monotonic() + timeout
        while off < len(data):
            try:
                off += os.write(self.fd, view[off:])
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise PushError("device stopped accepting data "
                                    "(USB OUT stalled — is the kernel alive?)")
                select.select([], [self.fd], [], 0.05)
            except OSError as e:
                raise PushError(f"serial write failed: {e}", retryable=False)

    def next_line(self, timeout: float):
        """The next complete device line, or None after `timeout` seconds."""
        if not self._lines:
            self._read(timeout)
        return self._lines.popleft() if self._lines else None

    def _read(self, timeout: float):
        try:
            r, _, _ = select.select([self.fd], [], [], max(0.0, timeout))
            if not r:
                return
            data = os.read(self.fd, 65536)
        except BlockingIOError:
            return
        except OSError as e:
            raise PushError(f"serial read failed: {e}", retryable=False)
        if not data:
            return
        if self.tee:
            self.tee.write(data)
            self.tee.flush()
        self._buf.extend(data)
        while b"\n" in self._buf:
            line, _, rest = bytes(self._buf).partition(b"\n")
            self._buf[:] = rest
            text = line.decode(errors="replace").rstrip("\r")
            self._lines.append(text)
            if not self.quiet and not ACK_RE.search(text):
                print(f"  | {text}")


class PushSender:
    """Flow-controlled BEGIN/DATA/END sender (PushProtocol.h). `link` needs
    write(bytes, timeout) and next_line(timeout); `clock` is injectable so the
    host tests (tests/ota/carsh_ota_test.py) can simulate the device."""

    def __init__(self, link, clock=time.monotonic, window: int = MAX_IN_FLIGHT,
                 ack_poll: float = ACK_POLL, ack_timeout: float = ACK_TIMEOUT,
                 chunk: int = CHUNK, progress=print):
        if HEADER_LEN + chunk + 4 > window:
            raise ValueError("a DATA packet must fit in the window")
        self.link = link
        self.clock = clock
        self.window = window
        self.ack_poll = ack_poll
        self.ack_timeout = ack_timeout
        self.chunk = chunk
        self.progress = progress
        self.queries = 0        # QUERY packets sent (stats / tests)
        self.begin_resends = 0

    def _lines(self, timeout: float):
        """Device lines until `timeout` passes without one; raises on failures.
        Lazy: lines after a `break` stay queued in the link (e.g. the kernel's
        apply=ok right behind the final ack is the caller's to read)."""
        line = self.link.next_line(timeout)
        while line is not None:
            if device_error(line):
                raise PushError(f"device: {line.strip()}",
                                retryable="recv failed" in line)
            yield line
            line = self.link.next_line(0)

    def send(self, carup: bytes):
        """Push one .carup: returns once the device has acked END (or logged
        "recv ok"); raises PushError otherwise. Lines after that stay queued
        in the link for the caller's apply wait."""
        pkts = data_packets(carup, self.chunk)
        end_seq = pkts[-1][0]
        if end_seq > 0xFFFF:
            raise PushError(f"package too large ({len(carup)} B) for 16-bit seq",
                            retryable=False)
        begin = packet(TYPE_BEGIN, 0, struct.pack(
            "<II", len(carup), zlib.crc32(carup) & 0xFFFFFFFF))
        query = packet(TYPE_QUERY, 0, b"")

        # BEGIN: idempotent before any DATA, so just re-send it until acked.
        self.link.write(begin)
        t0 = t_sent = self.clock()
        acked = False
        while not acked:
            for line in self._lines(0.05):
                m = ACK_RE.search(line)
                if m and int(m.group(1)) == 0:
                    acked = True
                    break
            now = self.clock()
            if acked:
                break
            if now - t0 > self.ack_timeout:
                raise PushError("no ack for BEGIN (is a push-capable kernel "
                                "running, and nothing else reading the port?)")
            if now - t_sent >= self.ack_poll:
                self.link.write(begin)
                self.begin_resends += 1
                t_sent = now

        # DATA + END, never more than `window` bytes beyond the last ack.
        acked_seq = 0
        next_i = 0
        inflight = collections.deque()  # (seq, nbytes) sent, not yet acked
        inflight_bytes = 0
        t_progress = t_query = t_start = self.clock()
        last_pct = -1
        while acked_seq < end_seq:
            while next_i < len(pkts):
                seq, pkt = pkts[next_i]
                if inflight and inflight_bytes + len(pkt) > self.window:
                    break
                self.link.write(pkt)
                inflight.append((seq, len(pkt)))
                inflight_bytes += len(pkt)
                next_i += 1
            sent_seq = pkts[next_i - 1][0] if next_i else 0
            for line in self._lines(0.05):
                if "ota-push: recv ok" in line:
                    a = end_seq  # END's ack line was lost; the device has it all
                else:
                    m = ACK_RE.search(line)
                    a = int(m.group(1)) if m else -1
                if acked_seq < a <= sent_seq:  # stale/foreign acks ignored
                    acked_seq = a
                    while inflight and inflight[0][0] <= a:
                        inflight_bytes -= inflight.popleft()[1]
                    t_progress = t_query = self.clock()
                    if acked_seq >= end_seq:
                        break  # leave what follows (apply=ok ...) queued
            now = self.clock()
            if acked_seq >= end_seq:
                break
            if now - t_progress > self.ack_timeout:
                raise PushError(f"device stopped acknowledging after seq "
                                f"{acked_seq}/{end_seq} (stalled kernel?)")
            if now - t_query >= self.ack_poll:
                self.link.write(query)  # "re-send your ack" (13 bytes)
                self.queries += 1
                t_query = now
            pct = acked_seq * 10 // end_seq
            if pct != last_pct:
                last_pct = pct
                self.progress(f"  push: {min(acked_seq * self.chunk, len(carup))}"
                              f"/{len(carup)} B acked")
        dt = self.clock() - t_start
        rate = len(carup) / dt / 1024 if dt > 0 else 0
        self.progress(f"sent {len(carup)} B in {dt:.1f}s ({rate:.0f} KiB/s), "
                      "waiting for the kernel ...")


def push_with_retries(sender: PushSender, link, carup: bytes, retries: int,
                      settle: float = 1.0) -> bool:
    """sender.send() with restarts from BEGIN; False when out of attempts."""
    for attempt in range(retries + 1):
        try:
            sender.send(carup)
            return True
        except PushError as e:
            print(f"push attempt {attempt + 1} failed: {e}", file=sys.stderr)
            if not e.retryable or attempt == retries:
                return False
            print("restarting the transfer from BEGIN ...", file=sys.stderr)
            # Let the kernel log/reset the failed receive before the new BEGIN;
            # anything it says about the old attempt is dropped here.
            deadline = sender.clock() + settle
            while sender.clock() < deadline:
                link.next_line(0.05)
    return False


# --- commands -----------------------------------------------------------------

def cmd_keygen(args):
    if os.path.exists(args.key) and not args.force:
        sys.exit(f"{args.key} already exists (use --force to overwrite)")
    subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519",
                    "-out", args.key], check=True)
    os.chmod(args.key, 0o600)
    der = subprocess.run(["openssl", "pkey", "-in", args.key, "-pubout",
                          "-outform", "DER"], capture_output=True,
                         check=True).stdout
    pub = der[-32:]  # SPKI: 12-byte header + raw 32-byte key
    rows = [", ".join(f"0x{b:02X}" for b in pub[i:i + 8]) for i in (0, 8, 16, 24)]
    body = ",\n    ".join(rows)
    with open(KEY_HEADER, "w") as f:
        f.write(
            "// OtaPushKey.h — Ed25519 public key the kernel accepts for serial OTA\n"
            "// pushes. GENERATED by scripts/carsh_ota.py keygen (the private key\n"
            "// stays on the dev host, gitignored). All-zero = pushes disabled.\n"
            "#pragma once\n\n"
            "#include \"ota/Crypto.h\"\n\n"
            "namespace carsh::device {\n\n"
            "inline constexpr carsh::ota::Ed25519PublicKey kOtaPushPubKey = {{\n"
            f"    {body},\n"
            "}};\n\n"
            "}  // namespace carsh::device\n")
    print(f"key:    {args.key}  (private — keep out of git)")
    print(f"header: {KEY_HEADER}  (rebuild the device image to provision)")


def make_package(args) -> bytes:
    with open(args.image, "rb") as f:
        image = f.read()
    build_id = args.build_id
    if not build_id:
        rev = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True)
        build_id = (rev.stdout.strip() or "dev") + time.strftime("-%m%d%H%M")
    if not os.path.exists(args.key):
        sys.exit(f"signing key {args.key} not found — run: carsh_ota.py keygen")
    carup = build_carup(image, args.key, args.version, build_id,
                        args.min_prev, args.slot_hint)
    print(f"packed {os.path.basename(args.image)}: image {len(image)} B, "
          f"carup {len(carup)} B, version {args.version}, build {build_id}")
    return carup


def cmd_pack(args):
    carup = make_package(args)
    with open(args.out, "wb") as f:
        f.write(carup)
    print(f"wrote {args.out}")


def check_port_free(args) -> bool:
    users = port_users(args.port)
    if not users or args.shared_port:
        return True
    print(f"error: {args.port} is also open in:", file=sys.stderr)
    for pid, cmd in users:
        print(f"    pid {pid}: {cmd}", file=sys.stderr)
    print("Two readers split the device's output between them, so this script "
          "would miss the\ndevice's flow-control acks. Stop that process for the "
          "push (--log FILE keeps a raw\ncapture meanwhile), or pass "
          "--shared-port to try anyway.", file=sys.stderr)
    return False


def open_tee(args):
    return open(args.log, "ab", buffering=0) if args.log else None


def cmd_push(args):
    if args.carup:
        with open(args.carup, "rb") as f:
            carup = f.read()
    else:
        carup = make_package(args)
    if not check_port_free(args):
        return 1

    fd = open_port(args.port)
    tee = None
    try:
        tee = open_tee(args)
        link = DeviceLink(fd, args.quiet, tee)
        sender = PushSender(link)
        if not push_with_retries(sender, link, carup, args.retries):
            return 1
        deadline = time.monotonic() + args.timeout
        ok = False
        try:
            while time.monotonic() < deadline:
                line = link.next_line(0.1)
                if line is None:
                    continue
                if "ota-push: apply=ok" in line:
                    ok = True
                if device_error(line):
                    print(f"DEVICE ERROR: {line}", file=sys.stderr)
                    return 1
                if "ota-push: rebooting" in line:
                    break
        except PushError:
            if not ok:
                raise
            # the port dropped right after apply=ok: that's the reboot
        if not ok:
            print("no apply=ok from the device (is the image key provisioned "
                  "and the kernel running?)", file=sys.stderr)
            return 1
    except PushError as e:
        print(f"push failed: {e}", file=sys.stderr)
        return 1
    finally:
        os.close(fd)
        if tee:
            tee.close()

    print("applied — device is rebooting into the new slot")
    if args.watch:
        return watch_boot(args)
    return 0


def watch_boot(args):
    """Re-open the port as it comes back and echo the boot log."""
    print(f"watching {args.port} for the reboot (^C to stop) ...")
    deadline = time.monotonic() + args.timeout
    tee = open_tee(args)
    try:
        while time.monotonic() < deadline:
            try:
                fd = open_port(args.port, wait=deadline - time.monotonic())
            except OSError:
                break
            link = DeviceLink(fd, quiet=False, tee=tee)
            try:
                while time.monotonic() < deadline:
                    line = link.next_line(0.1)
                    if line is not None and "entering UI loop" in line:
                        print("new image is up.")
                        return 0
            except PushError:
                pass  # port vanished (reboot) — reopen
            finally:
                try:
                    os.close(fd)
                except OSError:
                    pass
    finally:
        if tee:
            tee.close()
    print("timed out waiting for the new image (power-cycle + serial log to "
          "debug; A/B rollback protects the card)", file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    kg = sub.add_parser("keygen", help="create the dev Ed25519 key + header")
    kg.add_argument("--key", default=DEFAULT_KEY)
    kg.add_argument("--force", action="store_true")
    kg.set_defaults(fn=cmd_keygen)

    def pkg_args(p):
        p.add_argument("--image", default=os.path.join(REPO, "device",
                                                       "kernel8-rpi4.img"))
        p.add_argument("--key", default=DEFAULT_KEY)
        p.add_argument("--version", default="0.0.1",
                       help="manifest version (same-version repush is allowed; "
                            "only downgrades are blocked)")
        p.add_argument("--build-id", default="")
        p.add_argument("--min-prev", default="")
        p.add_argument("--slot-hint", type=lambda s: int(s, 0), default=0xFF)

    pk = sub.add_parser("pack", help="build a signed .carup")
    pkg_args(pk)
    pk.add_argument("--out", default="update.carup")
    pk.set_defaults(fn=cmd_pack)

    ps = sub.add_parser("push", help="send a .carup to the running kernel")
    pkg_args(ps)
    ps.add_argument("--carup", help="pre-built package (else packs --image)")
    ps.add_argument("--port", default=DEFAULT_PORT)
    ps.add_argument("--timeout", type=float, default=60.0)
    ps.add_argument("--quiet", action="store_true",
                    help="don't echo device log lines during the push")
    ps.add_argument("--watch", action="store_true",
                    help="after reboot, follow the console until the new "
                         "image reaches the UI loop")
    ps.set_defaults(fn=cmd_push)

    fl = sub.add_parser("flash", help="pack + push + watch (the dev loop)")
    pkg_args(fl)
    fl.add_argument("--port", default=DEFAULT_PORT)
    fl.add_argument("--timeout", type=float, default=120.0)
    fl.add_argument("--quiet", action="store_true")
    fl.set_defaults(fn=cmd_push, carup=None, watch=True)

    for p in (ps, fl):
        p.add_argument("--retries", type=int, default=2,
                       help="restart the transfer from BEGIN this many times "
                            "after a failed receive or an ack timeout")
        p.add_argument("--log", metavar="FILE",
                       help="append the raw console (incl. acks) to FILE, like "
                            "scripts/bench_capture.py")
        p.add_argument("--shared-port", action="store_true",
                       help="push even if another process has the port open "
                            "(it will steal ack lines; expect stalls)")

    args = ap.parse_args()
    sys.exit(args.fn(args) or 0)


if __name__ == "__main__":
    main()
