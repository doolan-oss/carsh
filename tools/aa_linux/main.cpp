// aa_linux — run carsh's own AA protocol core (src/aa/) against the real CA525 dongle
// over libusb on the Linux PC. If this reaches Running and media frames flow past the
// channel-open/AV-setup burst that wedges the Pi, then the dongle + protocol are good
// and the Pi-side failure is Circle's xHCI bulk transport.
//
// Auto-retries the whole session on disconnect so one run shows the failure PATTERN
// (does it consistently reach the same point, or die randomly?).
//
// Usage:  sudo ./aa_linux            (root or a udev rule is needed to claim the USB
//                                      interface). Plug the dongle into the PC and make
//                                      sure your phone is connected to it first.
#include <libusb.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <random>
#include <thread>

#include "AaCryptoMbedTls.h"
#include "LibUsbAaTransport.h"
#include "aa/AaConnection.h"
#include "aa/AaMessenger.h"
#include "aa/AaTypes.h"

using carsh::aa::AaConnection;
using carsh::aa::AaConnectionCallbacks;
using carsh::aa::AaMessenger;
using carsh::aa::AaState;
using carsh::aa::ChannelId;
using carsh::device::AaCryptoMbedTls;

namespace {

std::atomic<bool> g_stop{false};
void onSigint(int) { g_stop = true; }

int hostEntropy(void*, unsigned char* buf, size_t len) {
    static std::random_device rd;
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<unsigned char>(rd() & 0xFF);
    return 0;
}

const char* stateName(AaState s) {
    switch (s) {
        case AaState::VersionExchange: return "VersionExchange";
        case AaState::SslHandshake: return "SslHandshake";
        case AaState::Auth: return "Auth";
        case AaState::ServiceDiscovery: return "ServiceDiscovery";
        case AaState::Running: return "Running";
        case AaState::Shutdown: return "Shutdown";
    }
    return "?";
}

// Drive one AA session to death; returns the deepest state reached + media count.
struct SessionResult {
    AaState deepest;
    unsigned long mediaFrames;
};

SessionResult runSession(carsh::aalinux::LibUsbAaTransport* transport) {
    AaMessenger msger(*transport);
    AaCryptoMbedTls crypto(&hostEntropy, nullptr);
    if (!crypto.init()) {
        printf("error: mbedTLS init failed (err %d)\n", crypto.lastError());
        return {AaState::VersionExchange, 0};
    }

    unsigned long mediaFrames = 0, mediaBytes = 0;
    AaConnectionCallbacks cbs;
    cbs.onRxFrame = [](ChannelId ch, uint16_t id, bool enc, size_t n) {
        printf("rx  ch=%u id=%04x enc=%u len=%zu\n", (unsigned)ch, id, enc ? 1 : 0, n);
    };
    cbs.onTxFrame = [](ChannelId ch, uint16_t id, bool enc) {
        printf("tx  ch=%u id=%04x enc=%u\n", (unsigned)ch, id, enc ? 1 : 0);
    };
    cbs.onChannelOpened = [](ChannelId ch) {
        printf("** channel opened: %u\n", (unsigned)ch);
    };
    cbs.onAvMedia = [&](ChannelId ch, bool, uint64_t, const uint8_t*, size_t len) {
        mediaFrames++;
        mediaBytes += len;
        if (mediaFrames <= 3 || mediaFrames % 30 == 0)
            printf(">> MEDIA ch=%u frame#%lu len=%zu (total %lu KB)\n", (unsigned)ch,
                   mediaFrames, len, mediaBytes / 1024);
    };
    cbs.onShutdown = [] { printf("** phone requested shutdown\n"); };
    bool videoStarted = false;
    cbs.onAvStart = [&](ChannelId ch, bool started) {
        printf("** AV %s on ch=%u  <<<< %s\n", started ? "START" : "STOP", (unsigned)ch,
               started ? "STREAM STARTING" : "");
        if (ch == ChannelId::Video && started) videoStarted = true;
    };
    cbs.onAvSetup = [](ChannelId ch, uint8_t cfg) {
        printf("** AV setup done ch=%u config=%u\n", (unsigned)ch, cfg);
    };

    AaConnection conn(msger, crypto, cbs);
    printf("== starting AA connection (VERSION_REQUEST) ==\n");
    conn.start();

    AaState last = AaState::VersionExchange, deepest = last;
    auto t0 = std::chrono::steady_clock::now();
    auto lastBeat = t0;
    auto lastSensor = t0;
    while (!g_stop && !transport->isGone()) {
        // Drain ALL pending inbound frames each iteration.
        for (int drained = 0; drained < 256 && conn.poll(0); ++drained) {
        }
        // NO periodic sensor tick: the functional reference (NexusDrive/aasdk) sends a
        // sensor event exactly ONCE, reactively, on each SensorStartRequest (carsh's core
        // does this in handleSensorChannel). Spamming sensor events on a 1 Hz tick floods
        // the phone right as it decides to start video — it NAKs bulk-OUT and never starts.
        (void)videoStarted;
        (void)lastSensor;
        AaState st = conn.state();
        if (st != last) {
            printf("== state -> %s ==\n", stateName(st));
            last = st;
            if ((int)st > (int)deepest) deepest = st;
            if (st == AaState::Shutdown) break;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - lastBeat > std::chrono::seconds(2)) {
            lastBeat = now;
            printf("-- alive: state=%s media=%lu (%lu KB) --\n", stateName(st),
                   mediaFrames, mediaBytes / 1024);
        }
        if (now - t0 > std::chrono::seconds(60)) break;  // a healthy session would stream
    }
    if (transport->isGone()) printf("== device disappeared (dongle reset) ==\n");
    return {deepest, mediaFrames};
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, onSigint);

    // Optional arg: the pre-switch USB id to AOA-switch, "vid:pid" in hex. Default is
    // the CA525 dongle. For DIRECT wired Android Auto, pass the phone's own id (from
    // `lsusb` while it's plugged in), e.g.  ./aa_linux 04e8:6860
    uint16_t preVid = 0x2717, prePid = 0xFF40;
    if (argc > 1) {
        unsigned v = 0, p = 0;
        if (sscanf(argv[1], "%x:%x", &v, &p) == 2) {
            preVid = (uint16_t)v;
            prePid = (uint16_t)p;
            printf("== pre-switch target: %04x:%04x (from arg) ==\n", preVid, prePid);
        } else {
            printf("usage: %s [vid:pid]   (hex; default 2717:ff40 = CA525 dongle)\n", argv[0]);
            return 1;
        }
    }

    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        printf("error: libusb_init failed\n");
        return 1;
    }

    int attempts = 0, reachedRunning = 0;
    AaState best = AaState::VersionExchange;
    unsigned long totalMedia = 0;
    while (!g_stop && attempts < 8) {
        ++attempts;
        printf("\n############ ATTEMPT %d ############\n", attempts);
        auto* transport = carsh::aalinux::LibUsbAaTransport::openDongle(ctx, preVid, prePid);
        if (transport == nullptr) {
            // Dongle not in PTP yet (it cycles) — wait a bit and retry.
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        auto res = runSession(transport);
        delete transport;
        if ((int)res.deepest > (int)best) best = res.deepest;
        if (res.deepest == AaState::Running) ++reachedRunning;
        totalMedia += res.mediaFrames;
        printf("---- attempt %d: deepest=%s media=%lu ----\n", attempts,
               stateName(res.deepest), res.mediaFrames);
        if (res.mediaFrames > 0) break;  // success
        std::this_thread::sleep_for(std::chrono::seconds(1));  // let the dongle re-AOA
    }

    printf("\n===== OVERALL =====\n");
    printf("attempts: %d, reached Running: %d, deepest: %s, total media frames: %lu\n",
           attempts, reachedRunning, stateName(best), totalMedia);
    if (totalMedia > 0)
        printf("RESULT: MEDIA STREAMED over libusb => dongle+protocol good; Pi reset is "
               "Circle's xHCI.\n");
    else
        printf("RESULT: never streamed. Same dongle reset as the Pi on a clean USB stack "
               "=> NOT Circle's xHCI; it's the dongle or a protocol/behaviour issue.\n");

    libusb_exit(ctx);
    return 0;
}
