// CircleAaDriver.cpp — see CircleAaDriver.h.
#include "CircleAaDriver.h"

#include <circle/devicenameservice.h>
#include <circle/logger.h>

#include <cstring>
#include <vector>

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
    lastState_ = -1;
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
            auto* crypto = new AaCryptoMbedTls(&CircleAaDriver::rngEntropy, &rng_);
            if (!crypto->init()) {
                CLogger::Get()->Write(From, LogError,
                                      "crypto: mbedTLS init failed (err %d) — TLS disabled",
                                      crypto->lastError());
            }
            crypto_ = crypto;
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
            conn_ = new carsh::aa::AaConnection(*msger_, *crypto_, cbs);
            if (projection_ != nullptr) projection_->setConnection(conn_);
            CLogger::Get()->Write(From, LogNotice, "uaa1 bound — sending VERSION_REQUEST");
            conn_->start();
            lastState_ = static_cast<int>(conn_->state());
        }
    }

    if (conn_ == nullptr) return;

    // Drain up to a few messages per tick (poll(0) is non-blocking — returns false
    // when no complete frame is buffered).
    for (int i = 0; i < 8; ++i) {
        if (!conn_->poll(0)) break;
    }

    const int st = static_cast<int>(conn_->state());
    if (st != lastState_) {
        lastState_ = st;
        CLogger::Get()->Write(From, LogNotice, "state -> %s (rx=%u bytes)",
                              stateName(conn_->state()), aa->rxBytes());
    } else if (++ticks_ % 60 == 0) {
        CLogger::Get()->Write(From, LogNotice, "alive: state=%s rx=%u bytes",
                              stateName(conn_->state()), aa->rxBytes());
    }
}

}  // namespace carsh::device
