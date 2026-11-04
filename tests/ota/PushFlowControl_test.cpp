// Serial OTA push flow control (review finding O2): the receiver's cumulative
// acks + QUERY re-ack, the rx-overrun failure path, the device drain helper
// (drainPushSource, the body of CircleUsbLog's task), and an end-to-end model
// that runs the real PushReceiver behind a faithful model of Circle Step51's
// CDC gadget RX ring — no back-pressure, drops on overflow, drained only while
// the UI loop yields — to show a flow-controlled sender can't overrun it while
// the old unpaced stream does.
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "Test.h"
#include "ota/PushProtocol.h"

using namespace carsh::ota;

namespace {

std::vector<uint8_t> blobOf(std::size_t n) {
    std::vector<uint8_t> b(n);
    uint32_t x = 0x9E3779B9u;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        b[i] = uint8_t(x);
    }
    return b;
}

std::string hex(const std::vector<uint8_t>& v) {
    std::string s;
    char b[3];
    for (uint8_t c : v) {
        std::snprintf(b, sizeof b, "%02x", c);
        s += b;
    }
    return s;
}

void feed(PushReceiver& rx, const std::vector<uint8_t>& w) {
    rx.feed(w.data(), w.size());
}

// The sender side of a transfer as individual packets: [BEGIN, DATA..., END].
std::vector<std::vector<uint8_t>> packetsOf(const std::vector<uint8_t>& blob,
                                            std::size_t chunk = push::kMaxChunk) {
    std::vector<std::vector<uint8_t>> out;
    out.push_back(push::encodeBegin(uint32_t(blob.size()),
                                    crc32Of(blob.data(), blob.size())));
    uint16_t seq = 1;
    for (std::size_t off = 0; off < blob.size(); off += chunk, ++seq) {
        const std::size_t n = blob.size() - off < chunk ? blob.size() - off : chunk;
        out.push_back(push::encodePacket(push::kTypeData, seq, blob.data() + off, n));
    }
    out.push_back(push::encodePacket(push::kTypeEnd, seq, nullptr, 0));
    return out;
}

// Circle Step51 CUSBCDCGadgetEndpoint, OUT direction (usbcdcgadgetendpoint.cpp):
// an 8192+1 byte ring filled per USB packet from the dwc2 IRQ; a packet larger
// than the free space is truncated and the status set to -1, which the next
// Read() returns once. The endpoint is re-armed regardless: no NAK, no
// back-pressure — the host never learns bytes were lost.
struct GadgetRxModel {
    static constexpr std::size_t kQueueSize = 8192 + 1;
    std::vector<uint8_t> q = std::vector<uint8_t>(kQueueSize);
    std::size_t in = 0, out = 0;
    int status = 0;
    unsigned overruns = 0;
    unsigned zeroLengthEnqueues = 0;  // stock Circle: assert (nCount > 0) here
    std::size_t maxFill = 0;

    std::size_t freeBytes() const {
        return out <= in ? kQueueSize + out - in - 1 : out - in - 1;
    }
    std::size_t avail() const { return in < out ? kQueueSize + in - out : in - out; }

    void onTransferComplete(const uint8_t* p, std::size_t n) {
        const std::size_t f = freeBytes();
        if (n > f) {
            n = f;
            status = -1;  // RX overrun
            ++overruns;
        }
        if (n == 0) {
            ++zeroLengthEnqueues;
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            q[in] = p[i];
            if (++in == kQueueSize) in = 0;
        }
        if (avail() > maxFill) maxFill = avail();
    }
    void hostWrite(const std::vector<uint8_t>& d) {  // HighSpeed: 512 B packets
        for (std::size_t off = 0; off < d.size(); off += 512)
            onTransferComplete(d.data() + off, d.size() - off < 512 ? d.size() - off : 512);
    }
    int read(uint8_t* buf, std::size_t cap) {
        if (status) {
            const int s = status;
            status = 0;
            return s;
        }
        std::size_t a = avail();
        if (a == 0) return 0;
        if (a > cap) a = cap;
        for (std::size_t i = 0; i < a; ++i) {
            buf[i] = q[out];
            if (++out == kQueueSize) out = 0;
        }
        return int(a);
    }
};

// The kernel around the gadget: CircleUsbLog drains (drainPushSource) only
// while the UI task yields — the last (periodMs - renderMs) of each loop — and
// OtaPushService::poll runs once per loop. Time advances in 1 ms ticks.
struct KernelModel {
    GadgetRxModel gadget;
    PushReceiver rx;
    unsigned periodMs = 74, renderMs = 44;  // bench numbers with AA running
    unsigned nowMs = 0;
    std::deque<uint16_t> ackLines;           // "ota-push: ack <seq>" to the host
    std::vector<PushReceiver::Error> failures;
    std::vector<std::vector<uint8_t>> completed;
    unsigned overrunLogs = 0;

    void tick() {
        const unsigned inLoop = nowMs % periodMs;
        if (inLoop == 0) {  // UI loop: poll
            if (rx.state() == PushReceiver::State::Failed) {
                failures.push_back(rx.error());
                rx.reset();
            } else if (rx.state() == PushReceiver::State::Complete) {
                completed.push_back(rx.takeBlob());
            }
        }
        if (inLoop >= renderMs) {  // yielding: the CircleUsbLog task runs
            const PushDrainResult r = drainPushSource(
                &rx, [this](uint8_t* b, std::size_t cap) { return gadget.read(b, cap); });
            if (r.overrun) ++overrunLogs;
            if (r.ackReady) ackLines.push_back(r.ackSeq);
        }
        ++nowMs;
    }
};

// C++ mirror of carsh_ota.py's PushSender window logic (the Python sender has
// its own simulation in tests/ota/carsh_ota_test.py): keep at most
// kMaxInFlight bytes beyond the last cumulative ack.
bool flowControlledPush(KernelModel& k, const std::vector<uint8_t>& blob,
                        unsigned maxMs) {
    const auto pkts = packetsOf(blob);
    const uint16_t endSeq = uint16_t(pkts.size() - 1);
    k.gadget.hostWrite(pkts[0]);  // BEGIN
    bool begun = false;
    std::size_t next = 1;
    std::deque<std::pair<uint16_t, std::size_t>> inflight;
    std::size_t inflightBytes = 0;
    uint16_t acked = 0;
    for (unsigned t = 0; t < maxMs; ++t) {
        k.tick();
        while (!k.ackLines.empty()) {
            const uint16_t a = k.ackLines.front();
            k.ackLines.pop_front();
            if (!begun) {
                begun = a == 0;
                continue;
            }
            if (a > acked && a < next) {
                acked = a;
                while (!inflight.empty() && inflight.front().first <= a) {
                    inflightBytes -= inflight.front().second;
                    inflight.pop_front();
                }
            }
        }
        if (begun && acked == endSeq) return true;
        while (begun && next < pkts.size() &&
               (inflight.empty() || inflightBytes + pkts[next].size() <= push::kMaxInFlight)) {
            k.gadget.hostWrite(pkts[next]);
            inflight.emplace_back(uint16_t(next), pkts[next].size());
            inflightBytes += pkts[next].size();
            ++next;
        }
    }
    return false;
}

}  // namespace

TEST(PushFlowControl, WireFormatPinnedForTheHostScript) {
    // Same literals as tests/ota/carsh_ota_test.py (the Python encoder).
    CHECK_EQ(hex(push::encodeQuery()), std::string("435053480400000000dd51a233"));
    CHECK_EQ(hex(push::encodeBegin(0x1234, 0xDEADBEEF)),
             std::string("43505348010000080034120000efbeadde90b0ed0b"));
    CHECK_EQ(std::string(push::kAckLogFormat), std::string("ota-push: ack %u"));
    CHECK(push::kMaxPacket <= push::kMaxInFlight);
    CHECK(push::kMaxInFlight < push::kDeviceRxQueue);
    CHECK_EQ(push::kDeviceRxQueue, GadgetRxModel::kQueueSize - 1);
}

TEST(PushFlowControl, AcksAreCumulativeAndTakenOnce) {
    const auto blob = blobOf(10000);
    const auto pkts = packetsOf(blob, 4096);  // BEGIN, DATA 1..3, END 4
    PushReceiver rx;
    uint16_t seq = 99;
    CHECK(!rx.takeAck(seq));  // nothing yet

    feed(rx, pkts[0]);
    REQUIRE(rx.takeAck(seq));
    CHECK_EQ(seq, uint16_t(0));  // BEGIN accepted = ack 0
    CHECK(!rx.takeAck(seq));     // once

    feed(rx, pkts[1]);
    feed(rx, pkts[2]);  // two packets in one drain pass -> one cumulative ack
    REQUIRE(rx.takeAck(seq));
    CHECK_EQ(seq, uint16_t(2));

    // Half a packet consumed: not acked until it completes.
    rx.feed(pkts[3].data(), 100);
    CHECK(!rx.takeAck(seq));
    rx.feed(pkts[3].data() + 100, pkts[3].size() - 100);
    feed(rx, pkts[4]);
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    REQUIRE(rx.takeAck(seq));
    CHECK_EQ(seq, uint16_t(4));  // END acked with its own seq
    CHECK(rx.takeBlob() == blob);
}

TEST(PushFlowControl, QueryReAcksCurrentPosition) {
    const auto blob = blobOf(9000);
    const auto pkts = packetsOf(blob, 4096);
    const auto query = push::encodeQuery();
    PushReceiver rx;
    uint16_t seq = 0;

    feed(rx, query);  // Idle: nothing to ack, and nothing breaks
    CHECK(!rx.takeAck(seq));
    CHECK(rx.state() == PushReceiver::State::Idle);

    feed(rx, pkts[0]);
    CHECK(rx.takeAck(seq));  // "ack 0" ... lost on the way to the host
    feed(rx, query);
    REQUIRE(rx.takeAck(seq));
    CHECK_EQ(seq, uint16_t(0));

    feed(rx, pkts[1]);
    CHECK(rx.takeAck(seq));  // "ack 1" lost too
    feed(rx, query);
    REQUIRE(rx.takeAck(seq));
    CHECK_EQ(seq, uint16_t(1));
    CHECK(rx.state() == PushReceiver::State::Receiving);  // QUERY isn't DATA

    for (std::size_t i = 2; i < pkts.size(); ++i) feed(rx, pkts[i]);
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(rx.takeBlob() == blob);
}

TEST(PushFlowControl, BeginResendIsIdempotentBeforeData) {
    const auto blob = blobOf(5000);
    const auto pkts = packetsOf(blob, 4096);
    PushReceiver rx;
    uint16_t seq = 7;
    feed(rx, pkts[0]);
    feed(rx, pkts[0]);  // host re-sent BEGIN (its ack went missing)
    REQUIRE(rx.takeAck(seq));
    CHECK_EQ(seq, uint16_t(0));
    for (std::size_t i = 1; i < pkts.size(); ++i) feed(rx, pkts[i]);
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(rx.takeBlob() == blob);
}

TEST(PushFlowControl, NoAckAfterAFailure) {
    const auto blob = blobOf(9000);
    const auto pkts = packetsOf(blob, 4096);
    PushReceiver rx;
    uint16_t seq = 0;
    feed(rx, pkts[0]);
    feed(rx, pkts[1]);
    feed(rx, pkts[3]);  // DATA 3 without 2: fails in the same pass as ack 1
    CHECK(rx.state() == PushReceiver::State::Failed);
    CHECK(rx.error() == PushReceiver::Error::BadSeq);
    CHECK(!rx.takeAck(seq));  // the failure line replaces the ack
    feed(rx, push::encodeQuery());
    CHECK(!rx.takeAck(seq));
}

TEST(PushFlowControl, OverrunFailsTransferInProgress) {
    const auto blob = blobOf(9000);
    const auto pkts = packetsOf(blob, 4096);
    PushReceiver rx;
    feed(rx, pkts[0]);
    feed(rx, pkts[1]);
    CHECK(rx.notifyRxOverrun());
    CHECK(rx.state() == PushReceiver::State::Failed);
    CHECK(rx.error() == PushReceiver::Error::RxOverrun);
    CHECK_EQ(std::string(PushReceiver::errorStr(rx.error())), std::string("rx-overrun"));
    CHECK_EQ(rx.rxOverruns(), 1u);
    uint16_t seq = 0;
    CHECK(!rx.takeAck(seq));
    // The host restarts from BEGIN after the kernel logged + reset it.
    rx.reset();
    for (const auto& p : pkts) feed(rx, p);
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(rx.takeBlob() == blob);
}

TEST(PushFlowControl, OverrunWhileIdleDropsPartialPacket) {
    const auto blob = blobOf(3000);
    const auto pkts = packetsOf(blob);
    PushReceiver rx;
    rx.feed(pkts[0].data(), 10);  // a BEGIN cut short by the loss
    CHECK(!rx.notifyRxOverrun()); // no transfer to abort
    CHECK(rx.state() == PushReceiver::State::Idle);
    for (const auto& p : pkts) feed(rx, p);  // the host's BEGIN re-send + data
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(rx.takeBlob() == blob);
}

TEST(PushFlowControl, OverrunAfterCompleteKeepsTheBlob) {
    const auto blob = blobOf(3000);
    PushReceiver rx;
    feed(rx, push::encodeTransfer(blob));
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(!rx.notifyRxOverrun());
    CHECK(rx.state() == PushReceiver::State::Complete);
    CHECK(rx.takeBlob() == blob);
}

TEST(PushFlowControl, DrainReportsOverrunAndKeepsDraining) {
    const auto blob = blobOf(300);  // packets fit one 512-byte read
    const auto pkts = packetsOf(blob);
    // Reader script: BEGIN, then -1 (overrun), then DATA, then empty.
    std::vector<std::vector<uint8_t>> reads = {pkts[0], {}, pkts[1]};
    std::size_t i = 0;
    auto reader = [&](uint8_t* b, std::size_t cap) -> int {
        if (i >= reads.size()) return 0;
        const auto& r = reads[i++];
        if (r.empty()) return -1;
        REQUIRE(r.size() <= cap);
        std::copy(r.begin(), r.end(), b);
        return int(r.size());
    };
    PushReceiver rx;
    const PushDrainResult res = drainPushSource(&rx, reader);
    CHECK(res.overrun);
    CHECK(res.abortedTransfer);
    CHECK(!res.ackReady);  // failed: no ack, the poll logs the failure
    CHECK_EQ(res.bytes, pkts[0].size() + pkts[1].size());  // kept reading
    CHECK_EQ(i, reads.size());
    CHECK(rx.error() == PushReceiver::Error::RxOverrun);
}

TEST(PushFlowControl, DrainAcksAndBoundsReads) {
    const auto blob = blobOf(9000);
    const auto wire = push::encodeTransfer(blob, 4096);
    std::size_t off = 0;
    unsigned calls = 0;
    auto reader = [&](uint8_t* b, std::size_t cap) -> int {
        ++calls;
        const std::size_t n = wire.size() - off < cap ? wire.size() - off : cap;
        std::copy(wire.begin() + long(off), wire.begin() + long(off + n), b);
        off += n;
        return int(n);
    };
    PushReceiver rx;
    PushDrainResult r = drainPushSource(&rx, reader, 4);  // 4 x 512 B max
    CHECK_EQ(calls, 4u);
    CHECK_EQ(r.bytes, std::size_t(2048));
    CHECK(r.ackReady);
    CHECK_EQ(r.ackSeq, uint16_t(0));  // BEGIN only so far
    while (rx.state() == PushReceiver::State::Receiving) r = drainPushSource(&rx, reader);
    REQUIRE(rx.state() == PushReceiver::State::Complete);
    CHECK(r.ackReady);
    CHECK_EQ(r.ackSeq, uint16_t(4));

    // No receiver: bytes are drained and dropped, never acked.
    off = 0;
    const PushDrainResult d = drainPushSource(nullptr, reader);
    CHECK_EQ(d.bytes, wire.size());
    CHECK(!d.ackReady);
}

TEST(PushFlowControl, FlowControlledPushNeverOverrunsTheGadget) {
    const auto blob = blobOf(1024 * 1024 + 77);
    KernelModel k;  // 44 ms render out of every 74 ms: no draining
    REQUIRE(flowControlledPush(k, blob, 60000));
    for (int t = 0; t < 200; ++t) k.tick();  // the UI loop picks it up
    CHECK_EQ(k.gadget.overruns, 0u);
    CHECK_EQ(k.overrunLogs, 0u);
    CHECK(k.gadget.maxFill <= push::kMaxInFlight);
    CHECK(k.failures.empty());
    REQUIRE(k.completed.size() == 1u);
    CHECK(k.completed[0] == blob);
    // ~1 MiB inside the 60 s budget with a quarter-second render stall too:
    KernelModel slow;
    slow.periodMs = 250;
    slow.renderMs = 240;
    CHECK(flowControlledPush(slow, blobOf(128 * 1024), 60000));
    CHECK_EQ(slow.gadget.overruns, 0u);
}

TEST(PushFlowControl, UnpacedStreamOverrunsAndNowFailsAsRxOverrun) {
    // The pre-fix sender (BEGIN acked, then every DATA back to back) against
    // the same kernel: the ring overflows during the first render, and the
    // receiver now fails with rx-overrun at once instead of bad-seq later.
    const auto blob = blobOf(256 * 1024);
    const auto pkts = packetsOf(blob);
    KernelModel k;
    k.gadget.hostWrite(pkts[0]);
    while (k.ackLines.empty()) k.tick();
    k.nowMs = 0;  // start of a loop: render, no draining for 44 ms
    for (std::size_t i = 1; i < pkts.size(); ++i) k.gadget.hostWrite(pkts[i]);
    for (int t = 0; t < 2000; ++t) k.tick();
    CHECK(k.gadget.overruns > 0);
    // Stock Circle asserts on these (a full ring enqueues 0 bytes) — see
    // scripts/patches/usbcdcgadget-rx-overrun.patch.
    CHECK(k.gadget.zeroLengthEnqueues > 0);
    CHECK(k.overrunLogs > 0);
    REQUIRE(!k.failures.empty());
    CHECK(k.failures[0] == PushReceiver::Error::RxOverrun);
    CHECK(k.completed.empty());
}
