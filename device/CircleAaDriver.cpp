// CircleAaDriver.cpp — see CircleAaDriver.h.
#include "CircleAaDriver.h"

#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle-mbedtls/threadingmodule.h>  // installs mbedTLS THREADING_ALT mutex glue

#include <cstring>

#include "AaCryptoMbedTls.h"     // the mbedTLS-backed IAaCrypto (rung 4b)
#include "CircleAoaTransport.h"  // CAndroidAutoDevice (the IAaTransport behind uaa1)

namespace carsh::device {
namespace {

const char From[] = "aa";

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

// Why the connection entered AaState::Shutdown (AaConnection::shutdownReason()).
const char* shutdownReasonName(carsh::aa::AaShutdownReason r) {
    using R = carsh::aa::AaShutdownReason;
    switch (r) {
        case R::None: return "none";
        case R::PhoneRequest: return "phone exit (SHUTDOWN_REQUEST)";
        case R::VersionRejected: return "version rejected";
        case R::CryptoFailure: return "TLS failure";
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

CircleAaDriver::~CircleAaDriver() {
    // Run() is returning (the OTA reboot path): free the session quietly — the UI the
    // projection hooks drive is being torn down too — and stop watching the transport
    // so its later removal can't call back into a dead driver.
    endSession(false);
    unwatchTransport();
}

// Circle is destroying the bound transport (its derived part is already gone; don't
// touch pDevice). Task context: UpdatePlugAndPlay, or aoaForceReenumerate from tick().
// Forget it at once and unbind the projection so no UI path (touch, focus, media key)
// can send on it before tick() ends the session; Circle drops the registration.
void CircleAaDriver::transportRemoved(CDevice* /*pDevice*/, void* pContext) {
    auto* self = static_cast<CircleAaDriver*>(pContext);
    self->bound_ = nullptr;
    self->removedHandle_ = nullptr;
    self->transportGone_ = true;
    if (self->projection_ != nullptr) self->projection_->setConnection(nullptr);
}

void CircleAaDriver::unwatchTransport() {
    if (bound_ != nullptr && removedHandle_ != nullptr) {
        bound_->UnregisterRemovedHandler(removedHandle_);
    }
    removedHandle_ = nullptr;
    bound_ = nullptr;
}

void CircleAaDriver::endSession(bool notify) {
    // Contract C2 / review A4: every teardown of a live connection resets the
    // projection — the AA tab drops the frozen frame for the placeholder, the UI
    // returns to Now Playing and the auto-show latch re-arms for the next session.
    // disconnect() unbinds the connection before it runs the hooks (so they can't send
    // on the dying link) and runs while conn_ is still alive. It is idempotent: after
    // a phone SHUTDOWN_REQUEST the projection already ran it from onShutdown.
    if (projection_ != nullptr) {
        if (notify && conn_ != nullptr) {
            projection_->disconnect();
        } else {
            projection_->setConnection(nullptr);
        }
    }
    delete conn_;
    conn_ = nullptr;
    delete msger_;
    msger_ = nullptr;
    delete crypto_;
    crypto_ = nullptr;
    mbed_ = nullptr;  // freed via crypto_ above (same object); just drop the alias
    lastState_ = -1;
}

void CircleAaDriver::bind(CAndroidAutoDevice* aa) {
    bound_ = aa;
    removedHandle_ = aa->RegisterRemovedHandler(&CircleAaDriver::transportRemoved, this);
    faultLogged_ = false;
    sup_.onTransportBound();

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
        cbs.onShutdown = [this] {
            CLogger::Get()->Write(From, LogNotice, "connection shut down: %s",
                                  conn_ != nullptr
                                      ? shutdownReasonName(conn_->shutdownReason())
                                      : "?");
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
    // Immediately before the VERSION_REQUEST goes out (nothing above reads the
    // transport; crypto init and the allocations take time, and anything that arrives
    // until start() is old-session data): on a transport that already carried a
    // session (forceReenumerate's fallback re-binds one Circle could not remove), drop
    // the old session's bytes still in the RX ring.
    const unsigned stale = aa->beginSession();
    if (aa->sessions() > 1) {
        CLogger::Get()->Write(From, LogWarning,
                              "uaa1 re-bound (same transport, session %u; discarded %u "
                              "stale RX bytes) — sending VERSION_REQUEST",
                              aa->sessions(), stale);
    } else {
        CLogger::Get()->Write(From, LogNotice, "uaa1 bound — sending VERSION_REQUEST");
    }
    conn_->start();
    lastState_ = static_cast<int>(conn_->state());
}

// The first fault on the bound link, once. Circle's xHCI path records no USB error
// code or length for a failed transfer, so the transport can only say WHICH pipe
// failed and how; the xHC completion code (4 = USB Transaction Error, 3 = Babble,
// 6 = Stall) is in Circle's own "xhciep: Transfer error N on endpoint M" line just
// before this one (M is the xHCI endpoint ID, 2*epnum + 1 for IN: the bench's
// "endpoint 3" is our bulk-IN 0x81).
void CircleAaDriver::logLinkFault() {
    const auto fault = bound_->linkFault();
    if (fault == carsh::aa::AaLinkFault::OutError) {
        CLogger::Get()->Write(From, LogWarning,
                              "link dead: %s after %u ms (~3000 = xHCI timeout, the "
                              "dongle NAKed) tx=%u rx=%u",
                              carsh::aa::aaLinkFaultName(fault), bound_->lastOutBlockedMs(),
                              bound_->txBytes(), bound_->rxBytes());
    } else {
        CLogger::Get()->Write(From, LogWarning,
                              "link dead: %s — inFlight=%u ring=%u rx=%u tx=%u",
                              carsh::aa::aaLinkFaultName(fault), bound_->inFlight(),
                              bound_->ringUsed(), bound_->rxBytes(), bound_->txBytes());
    }
}

// Every consumed removal gets one line starting "uaa1 gone", so a grep for it finds
// them all. Circle's hub code logs no connect/disconnect, and after an Exit or a fault
// the session is already freed — without this the dongle re-cycling by itself (the
// [hw] premise of the Exit policy) would leave no trace. The removal is noticed only
// when the kernel pumps USB plug-and-play (every 33 UI frames), so the ms figures are
// upper bounds, coarse by about a second.
void CircleAaDriver::logTransportGone(uint64_t nowMs) {
    using Gone = carsh::aa::AaLinkSupervisor::GoneKind;
    const unsigned sinceEnd = static_cast<unsigned>(sup_.msSinceEnd(nowMs));
    switch (sup_.goneKind()) {
        case Gone::LiveSession:
            CLogger::Get()->Write(From, LogNotice,
                                  "uaa1 gone — session ended (unplugged, or the dongle "
                                  "re-cycled mid-session)");
            break;
        case Gone::AfterExit:
            CLogger::Get()->Write(From, LogNotice,
                                  "uaa1 gone ~%u ms after the phone exit — the dongle "
                                  "re-cycled by itself (or was unplugged); nothing forced",
                                  sinceEnd);
            break;
        case Gone::AfterFault:
            CLogger::Get()->Write(From, LogNotice,
                                  "uaa1 gone ~%u ms after the session ended — the dongle "
                                  "re-cycled by itself (or was unplugged); no forced "
                                  "re-enumeration needed",
                                  sinceEnd);
            break;
        case Gone::Forced:
            CLogger::Get()->Write(From, LogNotice,
                                  "uaa1 gone — removed by the forced re-enumeration (%u/%u); "
                                  "waiting for the dongle to come back",
                                  sup_.forcedAttempts(), sup_.config().maxForced);
            break;
        case Gone::AfterGiveUp:
            CLogger::Get()->Write(From, LogNotice,
                                  "uaa1 gone — after giving up: the dongle re-cycled by "
                                  "itself or was unplugged");
            break;
        case Gone::NotBound:
            CLogger::Get()->Write(From, LogWarning,
                                  "uaa1 gone — but no transport was being supervised");
            break;
    }
}

void CircleAaDriver::forceReenumerate() {
    CAndroidAutoDevice* aa = bound_;
    if (aa == nullptr) return;  // already gone — its own re-cycle raced the grace period
    endSession(true);           // defensive: the session already ended at EndSession
    CLogger::Get()->Write(From, LogWarning,
                          "dongle did not re-cycle — forcing USB re-enumeration (%u/%u)",
                          sup_.forcedAttempts(), sup_.config().maxForced);
    // On success aa is deleted inside this call; transportRemoved() has already
    // cleared bound_ and flagged transportGone_ (tick() consumes it next time).
    if (!aoaForceReenumerate(aa)) {
        // Circle couldn't remove it (the hub refused CLEAR_FEATURE(PORT_ENABLE)), so
        // this transport still exists. Stop watching it; the next tick binds the SAME
        // instance again as a new session, and the attempt stays counted in the budget.
        //  * Dead link (the usual case here — including an RX ring that overflowed
        //    while nobody read it during the grace period): that session ends at once
        //    and the removal is retried after the next grace period, at most maxForced
        //    times in a row, then "AA link lost — replug dongle".
        //  * Live link (a version reject or TLS failure left the pipe healthy): a
        //    best-effort retry on the same accessory connection, NOT a clean restart.
        //    bind() discards what the old session left in the RX ring, but a bulk-IN
        //    request already posted can still deliver old-session bytes (they then
        //    fail the new session, most likely as a TLS failure, and the bounded retry
        //    continues), and whether the dongle answers a second VERSION_REQUEST on
        //    one accessory connection is untested. If nothing answers, the session
        //    waits in VersionExchange — as with a dongle that has no phone — until the
        //    dongle re-cycles, the link faults, or it is replugged.
        unwatchTransport();
        sup_.onTransportGone();
    }
}

void CircleAaDriver::tick() {
    const uint64_t nowMs = CTimer::GetClockTicks64() / 1000;

    // 1. The bound transport was destroyed (unplug, the dongle's own re-cycle, or our
    //    forced re-enumeration): the session it carried, if any, is over. Logged every
    //    time, also when no session was live (after an Exit or a fault the session is
    //    already freed): it is the only trace of the dongle re-cycling by itself.
    if (transportGone_) {
        transportGone_ = false;
        logTransportGone(nowMs);  // before onTransportGone(): the phase says what it means
        endSession(true);
        sup_.onTransportGone();
    }

    // 2. Bind a transport that appeared under "uaa1" (the dongle re-cycled / was
    //    re-enumerated). A bound transport whose session ended stays bound, WITHOUT a
    //    new session, until it goes away — its link is dead, or the phone shut down.
    if (bound_ == nullptr) {
        auto* aa = static_cast<CAndroidAutoDevice*>(
            CDeviceNameService::Get()->GetDevice("uaa1", FALSE));
        if (aa != nullptr && aa->isAccessory()) bind(aa);
    }

    // 3. Drain ALL buffered messages this tick (poll(0) is non-blocking — returns
    //    false when no complete frame is buffered, the link is dead, or the
    //    connection has shut down). The phone fires a burst of ~12-20 channel-open +
    //    AV-setup messages in ~20ms right after service discovery and times out if the
    //    head unit doesn't answer them promptly; capping at a few per tick (one tick
    //    ≈ 33ms at 30fps) spread the replies across frames and tripped that timeout.
    //    The cap is a safety bound, not the steady state.
    if (conn_ != nullptr) {
        for (int i = 0; i < 256; ++i) {
            if (!conn_->poll(0)) break;
        }
    }

    // 4. Report the bound link's first fault (latched by the transport — the IN side
    //    from IRQ context, which can't log). Only while a session is live: after it
    //    ended, a still-bound transport may overflow its unread ring — irrelevant.
    const bool linkDead = bound_ != nullptr && bound_->linkDead();
    const carsh::aa::AaLinkFault fault =
        linkDead ? bound_->linkFault() : carsh::aa::AaLinkFault::None;
    if (linkDead && !faultLogged_ && conn_ != nullptr) {
        faultLogged_ = true;
        logLinkFault();
    }

    // 5. Connection state + transport-health telemetry.
    if (conn_ != nullptr) {
        const int st = static_cast<int>(conn_->state());
        const bool changed = (st != lastState_);
        const bool heartbeat = (++ticks_ % 60 == 0);
        if (changed) {
            lastState_ = st;
            const bool down = conn_->state() == carsh::aa::AaState::Shutdown;
            CLogger::Get()->Write(From, LogNotice, "state -> %s%s%s (tx=%u rx=%u bytes)",
                                  stateName(conn_->state()), down ? ": " : "",
                                  down ? shutdownReasonName(conn_->shutdownReason()) : "",
                                  bound_ ? bound_->txBytes() : 0,
                                  bound_ ? bound_->rxBytes() : 0);
        } else if (heartbeat && bound_ != nullptr) {
            // Transport-health heartbeat: tx/rx plus the bulk-pipe state, so a stall
            // is visible even when the AaState doesn't change.
            CLogger::Get()->Write(From, LogNotice,
                                  "alive: state=%s tx=%u rx=%u inFlight=%u ring=%u link=%s",
                                  stateName(conn_->state()), bound_->txBytes(),
                                  bound_->rxBytes(), bound_->inFlight(), bound_->ringUsed(),
                                  bound_->linkDead() ? "dead" : "ok");
        }
        // TLS bring-up telemetry (rung 4b). In the handshake phase, surface whether
        // mbedTLS made local progress — a ClientHello reaching the wire makes tx grow
        // past the version request — and any fatal mbedTLS error. A local crypto
        // failure shows as tx flat + mbedErr nonzero; the phone rejecting/ignoring a
        // well-formed handshake as tx growing, mbedErr 0, rx flat.
        if ((changed || heartbeat) && conn_->state() == carsh::aa::AaState::SslHandshake &&
            mbed_ != nullptr) {
            CLogger::Get()->Write(From, LogNotice, "tls: handshakeDone=%d mbedErr=-0x%04x",
                                  crypto_->handshakeDone() ? 1 : 0,
                                  static_cast<unsigned>(-mbed_->lastError()));
        }
    }

    // 6. Session end + recovery policy (AaLinkSupervisor, host-tested). The shutdown
    //    reason matters: a phone Exit ends the session without forcing anything.
    using Action = carsh::aa::AaLinkSupervisor::Action;
    using Cause = carsh::aa::AaLinkSupervisor::EndCause;
    const carsh::aa::AaShutdownReason shutdown =
        conn_ != nullptr ? conn_->shutdownReason() : carsh::aa::AaShutdownReason::None;
    const bool running = conn_ != nullptr && conn_->state() == carsh::aa::AaState::Running;
    switch (sup_.update(nowMs, linkDead, shutdown, running)) {
        case Action::None:
            break;
        case Action::EndSession:
            if (sup_.endCause() == Cause::PhoneExit) {
                CLogger::Get()->Write(From, LogNotice,
                                      "session ended (%s) — tearing down; no forced "
                                      "re-enumeration (the dongle re-cycles when the phone "
                                      "leaves; replug it if AA does not come back)",
                                      shutdownReasonName(shutdown));
            } else {
                CLogger::Get()->Write(From, LogWarning, "session ended (%s) — tearing down; "
                                      "waiting %u ms for the dongle to re-cycle",
                                      sup_.endCause() == Cause::LinkDead
                                          ? carsh::aa::aaLinkFaultName(fault)
                                          : shutdownReasonName(shutdown),
                                      static_cast<unsigned>(sup_.config().graceMs));
            }
            endSession(true);
            break;
        case Action::ForceReenumerate:
            forceReenumerate();
            break;
        case Action::GiveUp:
            CLogger::Get()->Write(From, LogError,
                                  "AA link lost — replug dongle (%u forced re-enumerations "
                                  "brought no session up)", sup_.forcedAttempts());
            break;
        case Action::DongleLost:
            CLogger::Get()->Write(From, LogError,
                                  "AA link lost — replug dongle (it did not come back after "
                                  "the forced re-enumeration)");
            break;
    }
}

}  // namespace carsh::device
