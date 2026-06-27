// CircleAaDriver.cpp — see CircleAaDriver.h.
#include "CircleAaDriver.h"

#include <circle/devicenameservice.h>
#include <circle/logger.h>

#include <vector>

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

// Logging stub crypto. The AA version exchange is plaintext, so the connection
// reaches SslHandshake without real TLS; we log the handshake kick (and any bytes
// the phone sends) and return empty, so the connection then waits. mbedTLS is next.
class LoggingCrypto : public carsh::aa::IAaCrypto {
public:
    std::vector<uint8_t> handshakeStep(const uint8_t* in, size_t len) override {
        if (in == nullptr || len == 0)
            CLogger::Get()->Write(From, LogNotice,
                                  "crypto: handshake kick reached (TLS not wired — stop here)");
        else
            CLogger::Get()->Write(From, LogNotice, "crypto: phone sent %u handshake bytes",
                                  (unsigned)len);
        return {};
    }
    bool handshakeDone() const override { return false; }
    std::vector<uint8_t> encrypt(const uint8_t*, size_t) override { return {}; }
    std::vector<uint8_t> decrypt(const uint8_t*, size_t) override { return {}; }
};

}  // namespace

CircleAaDriver::~CircleAaDriver() { teardown(); }

void CircleAaDriver::teardown() {
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
            crypto_ = new LoggingCrypto();
            msger_ = new carsh::aa::AaMessenger(*aa);
            carsh::aa::AaConnectionCallbacks cbs;
            cbs.onChannelOpened = [](carsh::aa::ChannelId c) {
                CLogger::Get()->Write(From, LogNotice, "channel opened: %u", (unsigned)c);
            };
            cbs.onChannelMessage = [](carsh::aa::ChannelId c, uint16_t id, const uint8_t*,
                                      size_t n) {
                CLogger::Get()->Write(From, LogNotice, "channel msg ch=%u id=%04x len=%u",
                                      (unsigned)c, (unsigned)id, (unsigned)n);
            };
            cbs.onShutdown = [] {
                CLogger::Get()->Write(From, LogNotice, "phone requested shutdown");
            };
            conn_ = new carsh::aa::AaConnection(*msger_, *crypto_, cbs);
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
