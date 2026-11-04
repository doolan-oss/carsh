#!/usr/bin/env python3
"""Host tests for scripts/carsh_ota.py's flow-controlled push sender.

Run directly (python3 tests/ota/carsh_ota_test.py) or via carsh_tests, which
runs this file from tests/ota/PushSenderScript_test.cpp.

The device is simulated on a fake clock: Circle Step51's CDC gadget RX ring
(DEVICE_RX_QUEUE usable bytes; a USB packet that doesn't fit is truncated and an
overrun flagged, as in CUSBCDCGadgetEndpoint::OnTransferComplete, and nothing
ever pushes back on the host) drained by a PushReceiver model that only runs
while the kernel's UI loop yields (a render stretch with no draining, then a
sleep window). The receiver model mirrors src/ota/PushProtocol.h just enough to
parse packets, ack them and report failures the way the kernel logs them.
"""

import importlib.util
import os
import select
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib

sys.dont_write_bytecode = True  # don't leave __pycache__ in scripts/

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "..", "scripts", "carsh_ota.py")
_spec = importlib.util.spec_from_file_location("carsh_ota", SCRIPT)
ota = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ota)


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


class SimDevice:
    """Kernel side of the push on a fake clock (see the module docstring)."""

    USB_PACKET = 512  # HighSpeed bulk OUT max packet

    def __init__(self, clock, period=0.074, render=0.044, drain_step=0.001,
                 drop_ack=None, stall_at=None, junk_at=None):
        self.clock = clock
        self.period = period          # UI loop period
        self.render = render          # part of it with no yield (no draining)
        self.drain_step = drain_step  # CircleUsbLog task rerun interval
        self.drop_ack = drop_ack      # fn(seq, nth) -> True drops that ack line
        self.stall_at = stall_at      # time after which the kernel never drains
        self.junk_at = junk_at        # time at which 9 KiB of junk arrives
        self.ring = bytearray()
        self.overrun_flag = False
        self.overruns = 0
        self.max_fill = 0
        self.bytes_in = 0
        self.lines = []
        self.next_task = 0.0
        self.next_poll = 0.0
        self.acks = 0
        # receiver model
        self.acc = bytearray()
        self.state = "idle"
        self.err = ""
        self.blob = bytearray()
        self.total = 0
        self.crc = 0
        self.next_seq = 0
        self.ack_pending = None
        self.completed = []
        self.failures = []

    # --- host -> device (USB OUT, never NAKed) -----------------------------
    def host_write(self, data: bytes):
        self.bytes_in += len(data)
        for off in range(0, len(data), self.USB_PACKET):
            pkt = data[off:off + self.USB_PACKET]
            free = ota.DEVICE_RX_QUEUE - len(self.ring)
            if len(pkt) > free:
                pkt = pkt[:free]
                self.overrun_flag = True
                self.overruns += 1
            self.ring += pkt
            self.max_fill = max(self.max_fill, len(self.ring))

    # --- time ----------------------------------------------------------------
    def advance(self, dt: float):
        end = self.clock.t + dt
        while True:
            nxt = min(self.next_task, self.next_poll)
            if nxt > end:
                break
            self.clock.t = nxt
            if self.junk_at is not None and self.clock.t >= self.junk_at:
                self.junk_at = None
                self.host_write(b"x" * (ota.DEVICE_RX_QUEUE + 1024))  # a stray writer
            if self.next_poll <= self.next_task:
                self._poll()
                self.next_poll += self.period
            else:
                in_loop = self.clock.t % self.period
                stalled = self.stall_at is not None and self.clock.t >= self.stall_at
                if in_loop >= self.render and not stalled:
                    self._drain()
                self.next_task += self.drain_step
        self.clock.t = end

    def _poll(self):  # OtaPushService::poll on the UI loop
        if self.state == "failed":
            self.failures.append(self.err)
            self.lines.append(f"ota: ota-push: recv failed err={self.err} "
                              "(resend to retry)")
            self._reset()
        elif self.state == "complete":
            self.completed.append(bytes(self.blob))
            self.lines.append(f"ota: ota-push: recv ok ({len(self.blob)} bytes)"
                              " — verifying")
            self.lines.append("ota: ota-push: apply=ok slot=B v1 tries=3")
            self.lines.append("ota: ota-push: rebooting")
            self._reset()

    def _drain(self):  # CircleUsbLog: drainPushSource + ack log
        for _ in range(32):
            if self.overrun_flag:  # Read() returns -1 once
                self.overrun_flag = False
                self.lines.append("usblog: CDC RX overrun: host->device bytes "
                                  "dropped")
                if self.state == "receiving":
                    self._fail("rx-overrun")
                elif self.state == "idle":
                    self.acc.clear()
                continue
            if not self.ring:
                break
            chunk = bytes(self.ring[:512])
            del self.ring[:512]
            if self.state in ("idle", "receiving"):
                self.acc += chunk
                self._parse()
        if self.ack_pending is not None:
            seq, self.ack_pending = self.ack_pending, None
            self.acks += 1
            if not (self.drop_ack and self.drop_ack(seq, self.acks)):
                self.lines.append(f"ota: ota-push: ack {seq}")

    # --- receiver model (PushProtocol.h) --------------------------------------
    def _reset(self):
        self.state, self.err = "idle", ""
        self.acc.clear()
        self.blob = bytearray()
        self.ack_pending = None

    def _fail(self, err):
        self.state, self.err = "failed", err
        self.acc.clear()
        self.ack_pending = None

    def _parse(self):
        while self.state in ("idle", "receiving"):
            i = self.acc.find(ota.MAGIC)
            if i < 0:
                del self.acc[:max(0, len(self.acc) - 3)]
                return
            del self.acc[:i]
            if len(self.acc) < ota.HEADER_LEN:
                return
            ptype, seq, n = struct.unpack_from("<BHH", self.acc, 4)
            if n > ota.CHUNK:
                del self.acc[:1]
                continue
            plen = ota.HEADER_LEN + n + 4
            if len(self.acc) < plen:
                return
            body = bytes(self.acc[4:ota.HEADER_LEN + n])
            (want,) = struct.unpack_from("<I", self.acc, plen - 4)
            if zlib.crc32(body) & 0xFFFFFFFF != want:
                del self.acc[:1]
                continue
            payload = body[5:]
            del self.acc[:plen]
            self._packet(ptype, seq, payload)

    def _packet(self, ptype, seq, p):
        if ptype == ota.TYPE_BEGIN:
            self.total, self.crc = struct.unpack("<II", p)
            self.blob = bytearray()
            self.next_seq = 1
            self.state = "receiving"
            self.ack_pending = 0
            return
        if self.state != "receiving":
            return
        if ptype == ota.TYPE_QUERY:
            self.ack_pending = self.next_seq - 1
        elif ptype == ota.TYPE_DATA:
            if seq != self.next_seq:
                return self._fail("bad-seq")
            self.next_seq += 1
            self.blob += p
            self.ack_pending = seq
        elif ptype == ota.TYPE_END:
            if seq != self.next_seq:
                return self._fail("bad-seq")
            if len(self.blob) != self.total:
                return self._fail("size-mismatch")
            if zlib.crc32(self.blob) & 0xFFFFFFFF != self.crc:
                return self._fail("blob-crc-mismatch")
            self.state = "complete"
            self.acc.clear()
            self.ack_pending = seq


class SimLink:
    """carsh_ota.DeviceLink stand-in: writes go into the SimDevice, waiting for
    a line advances the fake clock (in 1 ms steps) until the device logs one."""

    RTT = 0.0005  # host-side turnaround per write

    def __init__(self, dev):
        self.dev = dev
        self.written = bytearray()

    def write(self, data, timeout=None):
        self.written += data
        self.dev.host_write(bytes(data))
        self.dev.advance(self.RTT)

    def next_line(self, timeout):
        waited = 0.0
        while not self.dev.lines and waited < timeout:
            self.dev.advance(0.001)
            waited += 0.001
        return self.dev.lines.pop(0) if self.dev.lines else None


def pattern(n):
    return bytes((i * 131 + (i >> 7)) & 0xFF for i in range(n))


def make(dev_kwargs=None, sender_kwargs=None):
    clock = FakeClock()
    dev = SimDevice(clock, **(dev_kwargs or {}))
    link = SimLink(dev)
    sender = ota.PushSender(link, clock=clock, progress=lambda s: None,
                            **(sender_kwargs or {}))
    return clock, dev, link, sender


class WireFormat(unittest.TestCase):
    def test_packets_match_the_cpp_encoder(self):
        # Same literals as tests/ota/PushReceiver_test.cpp (encodeQuery /
        # encodeBegin), so the two sides can't drift apart silently.
        self.assertEqual(ota.packet(ota.TYPE_QUERY, 0, b"").hex(),
                         "435053480400000000dd51a233")
        self.assertEqual(
            ota.packet(ota.TYPE_BEGIN, 0,
                       struct.pack("<II", 0x1234, 0xDEADBEEF)).hex(),
            "43505348010000080034120000efbeadde90b0ed0b")

    def test_flow_control_constants(self):
        self.assertEqual(ota.DEVICE_RX_QUEUE, 8192)
        self.assertEqual(ota.MAX_IN_FLIGHT, 6144)
        self.assertLessEqual(ota.HEADER_LEN + ota.CHUNK + 4, ota.MAX_IN_FLIGHT)
        self.assertTrue(ota.ACK_RE.search("12:00:01.00 ota: ota-push: ack 42"))

    def test_data_packets_seq_and_end(self):
        pkts = ota.data_packets(b"a" * 9000)
        self.assertEqual([s for s, _ in pkts], [1, 2, 3, 4])
        self.assertEqual(pkts[-1][1][4], ota.TYPE_END)


class FlowControl(unittest.TestCase):
    def test_multi_mb_push_survives_long_renders(self):
        blob = pattern(2 * 1024 * 1024 + 123)
        clock, dev, link, sender = make()
        sender.send(blob)
        dev.advance(0.2)  # let the UI loop poll the completed receive
        self.assertEqual(dev.overruns, 0)
        self.assertEqual(dev.completed, [blob])
        self.assertLessEqual(dev.max_fill, ota.MAX_IN_FLIGHT)
        self.assertEqual(sender.queries, 0)

    def test_even_longer_renders_only_slow_it_down(self):
        blob = pattern(300 * 1024)
        clock, dev, link, sender = make({"period": 0.25, "render": 0.24})
        sender.send(blob)
        dev.advance(0.5)
        self.assertEqual(dev.overruns, 0)
        self.assertEqual(dev.completed, [blob])

    def test_unpaced_stream_overruns_the_gadget_ring(self):
        # The pre-fix sender: BEGIN, then all DATA back to back. The model must
        # show the O2 overrun, or the tests above prove nothing — and the
        # kernel must now name it (rx-overrun), not fail later with bad-seq.
        blob = pattern(200 * 1024)
        clock, dev, link, sender = make()
        link.write(ota.packet(ota.TYPE_BEGIN, 0, struct.pack(
            "<II", len(blob), zlib.crc32(blob) & 0xFFFFFFFF)))
        while link.next_line(1.0) is None:  # wait for "ack 0"
            pass
        for _, pkt in ota.data_packets(blob):
            link.write(pkt)
        dev.advance(1.0)
        self.assertGreater(dev.overruns, 0)
        self.assertEqual(dev.completed, [])
        self.assertTrue(any("CDC RX overrun" in l for l in dev.lines))
        self.assertTrue(any("recv failed err=rx-overrun" in l for l in dev.lines))

    def test_lost_ack_lines_recovered_by_query(self):
        blob = pattern(120 * 1024)
        # drop every 7th ack line (IN queue full, noisy port, ...)
        clock, dev, link, sender = make({"drop_ack": lambda seq, n: n % 7 == 0})
        sender.send(blob)
        dev.advance(0.2)
        self.assertGreater(sender.queries, 0)
        self.assertEqual(dev.overruns, 0)
        self.assertEqual(dev.completed, [blob])

    def test_lost_begin_ack_resends_begin(self):
        blob = pattern(10 * 1024)
        clock, dev, link, sender = make({"drop_ack": lambda seq, n: n == 1})
        sender.send(blob)
        dev.advance(0.2)
        self.assertEqual(sender.begin_resends, 1)
        self.assertEqual(dev.completed, [blob])

    def test_lost_end_ack_accepts_recv_ok(self):
        blob = pattern(9000)  # DATA 1..3, END 4
        clock, dev, link, sender = make({"drop_ack": lambda seq, n: seq == 4})
        sender.send(blob)
        self.assertEqual(dev.completed, [blob])

    def test_stalled_kernel_times_out_without_overrunning(self):
        blob = pattern(1024 * 1024)
        clock, dev, link, sender = make({"stall_at": 0.5})
        with self.assertRaises(ota.PushError) as cm:
            sender.send(blob)
        self.assertTrue(cm.exception.retryable)
        self.assertIn("stopped acknowledging", str(cm.exception))
        # the stall is detected after ACK_TIMEOUT, with only rate-limited
        # QUERYs on top of the window — never enough to overrun the ring
        self.assertEqual(dev.overruns, 0)
        self.assertLessEqual(sender.queries, ota.ACK_TIMEOUT / ota.ACK_POLL + 1)
        self.assertLessEqual(dev.max_fill, ota.MAX_IN_FLIGHT + 16 * 13)

    def test_no_kernel_fails_begin(self):
        clock, dev, link, sender = make({"stall_at": 0.0})
        with self.assertRaises(ota.PushError) as cm:
            sender.send(pattern(100))
        self.assertIn("BEGIN", str(cm.exception))
        self.assertLessEqual(len(link.written),
                             (ota.ACK_TIMEOUT / ota.ACK_POLL + 2) * 21)

    def test_device_failure_restarts_from_begin(self):
        blob = pattern(1024 * 1024)
        # 9 KiB of junk lands mid-transfer: the ring overruns, the kernel fails
        # the receive with rx-overrun, and the sender restarts from BEGIN.
        clock, dev, link, sender = make({"junk_at": 0.3})
        ok = ota.push_with_retries(sender, link, blob, retries=2)
        dev.advance(0.2)
        self.assertTrue(ok)
        self.assertGreater(dev.overruns, 0)
        self.assertEqual(dev.failures, ["rx-overrun"])
        self.assertEqual(dev.completed, [blob])

    def test_retries_exhausted(self):
        clock, dev, link, sender = make({"stall_at": 0.0})
        self.assertFalse(ota.push_with_retries(sender, link, pattern(100),
                                               retries=1))


class FakeKernelOnPty(threading.Thread):
    """Real-time device end of a pty: parses the push with SimDevice's receiver
    model, answers with the kernel's log lines (acks right away, as the
    CircleUsbLog task would while the UI loop sleeps), then applies."""

    def __init__(self, master, fail_first=False):
        super().__init__(daemon=True)
        self.master = master
        self.dev = SimDevice(FakeClock())
        self.fail_first = fail_first
        self.stop = False
        self.data_packets = 0

    def run(self):
        dev = self.dev
        while not self.stop:
            r, _, _ = select.select([self.master], [], [], 0.02)
            if r:
                try:
                    data = os.read(self.master, 65536)
                except OSError:
                    data = b""
                if dev.state in ("idle", "receiving"):
                    dev.acc += data
                    before = dev.next_seq
                    dev._parse()
                    self.data_packets += max(0, dev.next_seq - before)
                    if (self.fail_first and dev.state == "receiving"
                            and dev.next_seq > 3):
                        self.fail_first = False
                        dev._fail("rx-overrun")
            if dev.ack_pending is not None:
                dev.lines.append(f"ota: ota-push: ack {dev.ack_pending}")
                dev.ack_pending = None
            if dev.state in ("failed", "complete"):
                dev._poll()
            while dev.lines:
                os.write(self.master, (dev.lines.pop(0) + "\r\n").encode())


@unittest.skipUnless(os.path.isdir("/proc/self/fd"), "needs Linux ptys + /proc")
class CliOverPty(unittest.TestCase):
    def run_push(self, fail_first=False):
        master, slave = os.openpty()
        path = os.ttyname(slave)
        os.close(slave)  # the CLI opens it by name; nothing else holds it
        kernel = FakeKernelOnPty(master, fail_first)
        kernel.start()
        blob = pattern(100 * 1024 + 5)
        with tempfile.TemporaryDirectory() as tmp:
            carup = os.path.join(tmp, "u.carup")
            log = os.path.join(tmp, "console.log")
            with open(carup, "wb") as f:
                f.write(blob)
            try:
                res = subprocess.run(
                    [sys.executable, SCRIPT, "push", "--carup", carup,
                     "--port", path, "--timeout", "10", "--log", log],
                    capture_output=True, text=True, timeout=60)
            finally:
                kernel.stop = True
                kernel.join()
                os.close(master)
            with open(log, "rb") as f:
                console = f.read().decode()
        return res, kernel, blob, console

    def test_push_end_to_end(self):
        res, kernel, blob, console = self.run_push()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual(kernel.dev.completed, [blob])
        self.assertIn("applied", res.stdout)
        self.assertNotIn("ota-push: ack", res.stdout)  # acks aren't echoed
        self.assertIn("ota-push: ack 1\r\n", console)  # ... but are logged

    def test_push_restarts_after_device_failure(self):
        res, kernel, blob, console = self.run_push(fail_first=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("restarting the transfer from BEGIN", res.stderr)
        self.assertEqual(kernel.dev.failures, ["rx-overrun"])
        self.assertEqual(kernel.dev.completed, [blob])


class PortUsers(unittest.TestCase):
    @unittest.skipUnless(os.path.isdir("/proc/self/fd"), "needs Linux /proc")
    def test_finds_another_reader(self):
        master, slave = os.openpty()
        path = os.ttyname(slave)
        try:
            child = subprocess.Popen(
                [sys.executable, "-c",
                 "import os,sys,time\n"
                 "fd=os.open(sys.argv[1],os.O_RDONLY|os.O_NOCTTY)\n"
                 "print('ok',flush=True)\n"
                 "time.sleep(30)\n", path],
                stdout=subprocess.PIPE)
            try:
                self.assertEqual(child.stdout.readline().strip(), b"ok")
                pids = [pid for pid, _ in ota.port_users(path)]
                self.assertIn(child.pid, pids)
                self.assertNotIn(os.getpid(), pids)  # our own fd doesn't count
            finally:
                child.kill()
                child.wait()
                child.stdout.close()
        finally:
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    unittest.main(verbosity=1)
