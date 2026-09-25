#!/usr/bin/env python3
"""carsh OTA dev tool — keygen / pack / push / flash over the USB-C console.

The serial-push dev loop (no SD card swap):

    # once: make a dev signing key + provision its public key into the image
    scripts/carsh_ota.py keygen

    # each iteration: package the freshly built kernel, push it over the
    # USB-C serial console, let the kernel stage it to the inactive A/B slot
    # and reboot through the chainloader
    scripts/carsh_ota.py flash --image device/kernel8-rpi4.img

Wire format: PushProtocol.h framing (magic "CPSH", BEGIN/DATA/END packets with
CRC-32) carrying a .carup package (Package.h container: signed manifest +
image). This script is the host-side mirror of those two headers — change them
together. Device feedback comes back as "ota-push: ..." log lines on the same
serial stream.

Only the Python stdlib + the `openssl` CLI (Ed25519 sign) are required.
"""

import argparse
import hashlib
import json
import os
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
TYPE_BEGIN, TYPE_DATA, TYPE_END = 1, 2, 3
CHUNK = 4096


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


def drain_log(fd, buf: bytearray, quiet: bool):
    """Read whatever the device has logged; return completed lines."""
    try:
        data = os.read(fd, 65536)
    except (BlockingIOError, OSError):
        data = b""
    lines = []
    if data:
        buf.extend(data)
        while b"\n" in buf:
            line, _, rest = bytes(buf).partition(b"\n")
            buf[:] = rest
            text = line.decode(errors="replace").rstrip("\r")
            lines.append(text)
            if not quiet:
                print(f"  | {text}")
    return lines


def write_all(fd, data: bytes):
    off = 0
    while off < len(data):
        try:
            off += os.write(fd, data[off:off + 4096])
        except BlockingIOError:
            time.sleep(0.005)
    termios.tcdrain(fd)


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


def send_carup(fd, carup: bytes, quiet: bool):
    log = bytearray()
    write_all(fd, packet(TYPE_BEGIN, 0,
                         struct.pack("<II", len(carup),
                                     zlib.crc32(carup) & 0xFFFFFFFF)))
    seq = 1
    t0 = time.monotonic()
    for off in range(0, len(carup), CHUNK):
        write_all(fd, packet(TYPE_DATA, seq, carup[off:off + CHUNK]))
        seq += 1
        if seq % 64 == 0:
            drain_log(fd, log, quiet)  # keep the console moving during send
    write_all(fd, packet(TYPE_END, seq, b""))
    dt = time.monotonic() - t0
    rate = len(carup) / dt / 1024 if dt > 0 else 0
    print(f"sent {len(carup)} B in {dt:.1f}s ({rate:.0f} KiB/s), waiting for "
          "the kernel ...")
    return log


def cmd_push(args):
    if args.carup:
        with open(args.carup, "rb") as f:
            carup = f.read()
    else:
        carup = make_package(args)

    fd = open_port(args.port)
    try:
        log = send_carup(fd, carup, args.quiet)
        deadline = time.monotonic() + args.timeout
        ok = rebooting = False
        while time.monotonic() < deadline:
            for line in drain_log(fd, log, args.quiet):
                if "ota-push: apply=ok" in line:
                    ok = True
                if "ota-push:" in line and ("fail" in line or "err" in line):
                    print(f"DEVICE ERROR: {line}", file=sys.stderr)
                    return 1
                if "ota-push: rebooting" in line:
                    rebooting = True
            if rebooting:
                break
            time.sleep(0.05)
        if not ok:
            print("no apply=ok from the device (is the image key provisioned "
                  "and the kernel running?)", file=sys.stderr)
            return 1
    finally:
        os.close(fd)

    print("applied — device is rebooting into the new slot")
    if args.watch:
        return watch_boot(args)
    return 0


def watch_boot(args):
    """Re-open the port as it comes back and echo the boot log."""
    print(f"watching {args.port} for the reboot (^C to stop) ...")
    deadline = time.monotonic() + args.timeout
    log = bytearray()
    while time.monotonic() < deadline:
        try:
            fd = open_port(args.port, wait=deadline - time.monotonic())
        except OSError:
            break
        try:
            while time.monotonic() < deadline:
                for line in drain_log(fd, log, quiet=False):
                    if "entering UI loop" in line:
                        print("new image is up.")
                        return 0
                time.sleep(0.05)
        except OSError:
            pass  # port vanished (reboot) — reopen
        finally:
            try:
                os.close(fd)
            except OSError:
                pass
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

    args = ap.parse_args()
    sys.exit(args.fn(args) or 0)


if __name__ == "__main__":
    main()
