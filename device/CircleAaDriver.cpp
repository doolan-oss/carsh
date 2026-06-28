// CircleAaDriver.cpp — see CircleAaDriver.h.
#include "CircleAaDriver.h"

#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle-mbedtls/threadingmodule.h>  // installs mbedTLS THREADING_ALT mutex glue

#include <cstring>
#include <vector>

#include "AaCryptoMbedTls.h"     // the mbedTLS-backed IAaCrypto (rung 4b)
#include "CircleAoaTransport.h"  // CAndroidAutoDevice (the IAaTransport behind uaa1)

namespace carsh::device {
namespace {

const char From[] = "aa";

// Decode a Circle TUSBError (usb.h enum order) for the serial log, so a bulk-reset
// shows as e.g. "Babble" instead of a bare "2". Babble/over-run on bulk-IN is the
// prime suspect for the non-deterministic AA video stall.
const char* usbErrName(unsigned e) {
    static const char* const kNames[] = {
        "Stall", "Transaction", "Babble", "FrameOverrun", "DataToggle",
        "HostBus", "Split", "Timeout", "Aborted", "Unknown",
    };
    return e < (sizeof(kNames) / sizeof(kNames[0])) ? kNames[e] : "?";
}

// Bounded re-post attempts after the bulk-IN pipe goes fully dead, so a wedged
// endpoint doesn't churn forever — if these don't revive it, the dongle re-cycle
// (rebind) is the only recovery and the log says so.
constexpr unsigned kMaxInRestarts = 5;

const char* stateName(carsh::aa::AaState s) {
    using S = carsh::aa::AaState;
    switch (s) {
        case S::VersionExchange: return "VersionExchange";
        case S::SslHandshake: return "SslHandshake";
        case S::Auth: return "Auth";
        case S::ServiceDiscovery: return "ServiceDiscovery";
        case S::Running: return "Running";
        case S::Shutdown: return "Shutdown";
    }
    return "?";
}

}  // namespace

int CircleAaDriver::rngEntropy(void* ctx, unsigned char* buf, size_t len) {
    auto* rng = static_cast<CBcmRandomNumberGenerator*>(ctx);
    size_t i = 0;
    while (i < len) {
        const u32 r = rng->GetNumber();
        size_t take = len - i;
        if (take > sizeof(r)) take = sizeof(r);
        std::memcpy(buf + i, &r, take);
        i += take;
    }
    return 0;
}

CircleAaDriver::~CircleAaDriver() { teardown(); }

void CircleAaDriver::teardown() {
    // Drop the projection's dangling connection pointer before freeing it (the
    // dongle re-cycles, so this runs on every rebind).
    if (projection_ != nullptr) projection_->setConnection(nullptr);
    delete conn_;
    conn_ = nullptr;
    delete msger_;
    msger_ = nullptr;
    delete crypto_;
    crypto_ = nullptr;
    mbed_ = nullptr;  // freed via crypto_ above (same object); just drop the alias
    lastState_ = -1;
    lastInErrSeq_ = 0;
    lastOutErrSeq_ = 0;
    inRestarts_ = 0;  // re-post budget is per-connection; the dongle re-cycle resets it
}

void CircleAaDriver::tick() {
    auto* aa = static_cast<CAndroidAutoDevice*>(
        CDeviceNameService::Get()->GetDevice("uaa1", FALSE));

    // (Re)bind when the transport instance changes — the dongle re-cycles, so "uaa1"
    // is deleted and re-created. We compare pointers only (never deref a stale one).
    if (static_cast<void*>(aa) != boundDev_) {
        teardown();
        boundDev_ = aa;
        if (aa != nullptr && aa->isAccessory()) {
            // circle-mbedtls is built MBEDTLS_THREADING_C + MBEDTLS_THREADING_ALT, so
            // mbedTLS's mutex callbacks MUST be installed (mbedtls_threading_set_alt)
            // before any mbedTLS context is created — otherwise the default ALT
            // mutex_lock is a fail-stub returning MBEDTLS_ERR_THREADING_BAD_INPUT_DATA
            // (-0x001c) on the first lock, aborting the handshake before the
            // ClientHello (observed on the bench: tx stuck at the VERSION_REQUEST).
            // CTLSSimpleSupport would normally do this; carsh drives raw mbedTLS, so
            // install it here. The function-static runs once, on the first bind (before
            // the first crypto context) and persists across dongle reconnect cycles.
            static CircleMbedTLS::CThreadingModule s_mbedThreading;
            (void)s_mbedThreading;
            auto* crypto = new AaCryptoMbedTls(&CircleAaDriver::rngEntropy, &rng_);
            if (!crypto->init()) {
                CLogger::Get()->Write(From, LogError,
                                      "crypto: mbedTLS init failed (err %d) — TLS disabled",
                                      crypto->lastError());
            }
            crypto_ = crypto;
            mbed_ = crypto;  // typed alias for TLS telemetry below
            msger_ = new carsh::aa::AaMessenger(*aa);
            // Route channel traffic into the projection (video->decoder->screen,
            // audio->ring, focus/touch outbound) when one is wired (rung 6); else a
            // logging fallback so bring-up is still observable on the serial console.
            carsh::aa::AaConnectionCallbacks cbs;
            if (projection_ != nullptr) {
                cbs = projection_->callbacks();
            } else {
                cbs.onChannelOpened = [](carsh::aa::ChannelId c) {
                    CLogger::Get()->Write(From, LogNotice, "channel opened: %u", (unsigned)c);
                };
                cbs.onChannelMessage = [](carsh::aa::ChannelId c, uint16_t id,
                                          const uint8_t*, size_t n) {
                    CLogger::Get()->Write(From, LogNotice,
                                          "channel msg ch=%u id=%04x len=%u", (unsigned)c,
                                          (unsigned)id, (unsigned)n);
                };
                cbs.onShutdown = [] {
                    CLogger::Get()->Write(From, LogNotice, "phone requested shutdown");
                };
            }
            // Diagnostic: log every received frame (esp. in the Running phase) on the
            // serial console regardless of projection wiring, so we can see what the
            // phone sends after service discovery.
            cbs.onRxFrame = [](carsh::aa::ChannelId ch, uint16_t id, bool enc, size_t n) {
                CLogger::Get()->Write(From, LogNotice, "rx frame ch=%u id=%04x enc=%u len=%u",
                                      (unsigned)ch, (unsigned)id, enc ? 1u : 0u, (unsigned)n);
            };
            cbs.onTxFrame = [](carsh::aa::ChannelId ch, uint16_t id, bool enc) {
                CLogger::Get()->Write(From, LogNotice, "tx frame ch=%u id=%04x enc=%u",
                                      (unsigned)ch, (unsigned)id, enc ? 1u : 0u);
            };
            conn_ = new carsh::aa::AaConnection(*msger_, *crypto_, cbs);
            if (projection_ != nullptr) projection_->setConnection(conn_);
            CLogger::Get()->Write(From, LogNotice, "uaa1 bound — sending VERSION_REQUEST");
            conn_->start();
            lastState_ = static_cast<int>(conn_->state());
        }
    }

    if (conn_ == nullptr) return;

    // Drain ALL buffered messages this tick (poll(0) is non-blocking — returns false
    // when no complete frame is buffered). The phone fires a burst of ~12-20 channel-
    // open + AV-setup messages in ~20ms right after service discovery and times out if
    // the head unit doesn't answer them promptly; capping at a few per tick (one tick
    // ≈ 33ms at 30fps) spread the replies across frames and tripped that timeout. The
    // cap is a safety bound, not the steady state.
    for (int i = 0; i < 256; ++i) {
        if (!conn_->poll(0)) break;
    }

    // Surface any bulk-IN transfer error the transport stashed from IRQ context (it
    // can't log there). The decoded TUSBError + the result length pin down the failure
    // mode (Babble / length > 16384 = the dongle over-ran the IN buffer). After an
    // error that URB stopped; once ALL bulk-IN URBs have stopped (inFlight==0) the head
    // unit is deaf, so attempt a bounded, logged re-post — race-free precisely because
    // nothing is in flight.
    if (aa->inErrSeq() != lastInErrSeq_) {
        lastInErrSeq_ = aa->inErrSeq();
        CLogger::Get()->Write(From, LogWarning,
                              "bulk-IN error: %s (usbErr=%u) resultLen=%u inFlight=%u "
                              "ring=%u rx=%u",
                              usbErrName(aa->lastInUsbErr()), aa->lastInUsbErr(),
                              aa->lastInResultLen(), aa->inFlight(), aa->ringUsed(),
                              aa->rxBytes());
        if (aa->inFlight() == 0) {
            if (inRestarts_ < kMaxInRestarts) {
                ++inRestarts_;
                const bool ok = aa->restartInPolling();
                CLogger::Get()->Write(From, ok ? LogNotice : LogError,
                                      "bulk-IN fully stalled — re-post #%u %s",
                                      inRestarts_, ok ? "submitted" : "FAILED");
            } else {
                CLogger::Get()->Write(From, LogError,
                                      "bulk-IN dead after %u re-posts — awaiting dongle "
                                      "re-cycle", inRestarts_);
            }
        }
    }

    // Surface any bulk-OUT failure. write() runs in task context, so the transport
    // captured the real TUSBError from its own URB (the IN path can only stash from
    // IRQ). A stall/timeout here means the phone stopped draining our OUT endpoint.
    if (aa->outErrSeq() != lastOutErrSeq_) {
        lastOutErrSeq_ = aa->outErrSeq();
        CLogger::Get()->Write(From, LogWarning, "bulk-OUT error: %s (usbErr=%u) tx=%u",
                              usbErrName(aa->lastOutUsbErr()), aa->lastOutUsbErr(),
                              aa->txBytes());
    }

    const int st = static_cast<int>(conn_->state());
    const bool changed = (st != lastState_);
    const bool heartbeat = (++ticks_ % 60 == 0);
    if (changed) {
        lastState_ = st;
        CLogger::Get()->Write(From, LogNotice, "state -> %s (tx=%u rx=%u bytes)",
                              stateName(conn_->state()), aa->txBytes(), aa->rxBytes());
    } else if (heartbeat) {
        // Transport-health heartbeat: tx/rx plus the bulk-pipe state, so a stall is
        // visible even when the AaState doesn't change (inFlight dropping to 0, the ring
        // backing up, or the error counters ticking up are the tells).
        CLogger::Get()->Write(From, LogNotice,
                              "alive: state=%s tx=%u rx=%u inFlight=%u ring=%u "
                              "inErr=%u outErr=%u",
                              stateName(conn_->state()), aa->txBytes(), aa->rxBytes(),
                              aa->inFlight(), aa->ringUsed(), aa->inErrSeq(),
                              aa->outErrSeq());
    }
    // TLS bring-up telemetry (rung 4b). In the handshake phase, surface whether
    // mbedTLS made local progress — a ClientHello reaching the wire makes tx grow
    // past the version request — and any fatal mbedTLS error. If the pinned cipher
    // suite is absent from circle-mbedtls (risk #4), the ClientHello is never
    // produced: tx stays flat and mbedErr is nonzero. That distinguishes a local
    // crypto failure from the phone rejecting/ignoring a well-formed handshake (tx
    // grows, mbedErr stays 0, rx stays flat = phone silent).
    if ((changed || heartbeat) && conn_->state() == carsh::aa::AaState::SslHandshake &&
        mbed_ != nullptr) {
        CLogger::Get()->Write(From, LogNotice, "tls: handshakeDone=%d mbedErr=-0x%04x",
                              crypto_->handshakeDone() ? 1 : 0,
                              static_cast<unsigned>(-mbed_->lastError()));
    }
}

}  // namespace carsh::device
