// AaConnection — the Android Auto head-unit connection state machine. Drives the
// bring-up sequence over an AaMessenger + IAaCrypto:
//   1. send VERSION_REQUEST            (plain, control channel)
//   2. recv VERSION_RESPONSE
//   3. SSL_HANDSHAKE exchange          (plain) until the TLS handshake completes
//   4. send AUTH_COMPLETE              (plain)
//   5. recv SERVICE_DISCOVERY_REQUEST → send SERVICE_DISCOVERY_RESPONSE (encrypted)
//   6. recv CHANNEL_OPEN_REQUEST(s)    → send CHANNEL_OPEN_RESPONSE (encrypted)
//   7. Running: decrypt + dispatch channel messages to callbacks
// Ported from the dylant9/head_unit reference (aa_channels). Platform-independent;
// host-tested end-to-end with a scripted phone + FakeAaCrypto (tests/aa).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "aa/AaMessenger.h"
#include "aa/AaTypes.h"
#include "aa/IAaCrypto.h"

namespace carsh::aa {

enum class AaState {
    VersionExchange,
    SslHandshake,
    Auth,
    ServiceDiscovery,
    Running,
    Shutdown,
};

struct AaConnectionCallbacks {
    // A decrypted channel message in the Running phase (video/audio/sensor/input/
    // media/nav). The connection has already stripped framing + decryption + the
    // 2-byte message id; payload is the protobuf (or media) body.
    std::function<void(ChannelId channel, uint16_t msgId, const uint8_t* payload,
                       size_t len)>
        onChannelMessage;
    // A channel transitioned to open after a CHANNEL_OPEN_REQUEST was accepted.
    std::function<void(ChannelId channel)> onChannelOpened;
    // The phone requested shutdown.
    std::function<void()> onShutdown;
};

class AaConnection {
public:
    AaConnection(AaMessenger& msger, IAaCrypto& crypto, AaConnectionCallbacks cbs = {});

    // Begin the handshake by sending VERSION_REQUEST.
    void start();

    // Receive and handle exactly one message. Returns false on transport
    // timeout/error or once the connection has shut down.
    bool poll(uint32_t timeoutMs);

    AaState state() const { return state_; }
    bool channelOpen(ChannelId ch) const;

private:
    bool sendPlainSpecific(uint16_t msgId, const uint8_t* p, size_t n);
    bool sendPlainControl(uint16_t msgId, const uint8_t* p, size_t n);
    bool sendEncryptedControl(uint16_t msgId, const uint8_t* p, size_t n);

    void handleControl(uint16_t msgId, const uint8_t* p, size_t n);
    void handleVersionResponse(const uint8_t* p, size_t n);
    void handleSslHandshake(const uint8_t* p, size_t n);
    void handleServiceDiscoveryRequest(const uint8_t* p, size_t n);
    void handleChannelOpenRequest(const uint8_t* p, size_t n);
    void handlePingRequest(const uint8_t* p, size_t n);

    AaMessenger& msger_;
    IAaCrypto& crypto_;
    AaConnectionCallbacks cbs_;
    AaState state_ = AaState::VersionExchange;
    std::array<bool, static_cast<size_t>(kMaxChannel) + 1> channelOpen_{};
};

}  // namespace carsh::aa
