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
    // A decrypted channel message in the Running phase that the structured handlers
    // below did not consume (e.g. nav, media-status). The connection has already
    // stripped framing + decryption + the 2-byte message id; payload is the body.
    std::function<void(ChannelId channel, uint16_t msgId, const uint8_t* payload,
                       size_t len)>
        onChannelMessage;
    // A channel transitioned to open after a CHANNEL_OPEN_REQUEST was accepted.
    std::function<void(ChannelId channel)> onChannelOpened;
    // The phone requested shutdown.
    std::function<void()> onShutdown;

    // --- rung 4c: structured per-channel routing -----------------------------
    // AV media payload (video channel = H.264 Annex-B; audio channels = PCM). The
    // 2-byte msg id and any leading 8-byte timestamp are already stripped, so
    // data/len is the bare bitstream; tsPresent/timestamp carry the PTS when the
    // media message included one (MEDIA_WITH_TIMESTAMP). The connection has already
    // sent the flow-control ack for this message.
    std::function<void(ChannelId channel, bool tsPresent, uint64_t timestamp,
                       const uint8_t* data, size_t len)>
        onAvMedia;
    // An AV channel finished setup — the HU has replied SETUP_RESPONSE for the given
    // config index. Fired for video + each audio channel.
    std::function<void(ChannelId channel, uint8_t configIndex)> onAvSetup;
    // An AV channel stream started (true, START_INDICATION) or stopped (false,
    // STOP_INDICATION).
    std::function<void(ChannelId channel, bool started)> onAvStart;
    // The phone asked for a video focus change (VIDEO_FOCUS_REQUEST{mode}); the app
    // typically mirrors it back with sendVideoFocus once the tab state agrees.
    std::function<void(VideoFocusMode mode)> onVideoFocusRequest;

    // Diagnostic tap: fired for EVERY frame the messenger receives, BEFORE decrypt or
    // dispatch (raw on-wire view). channel/encrypted/rawLen describe the frame as it
    // arrived (wireMsgId is meaningful only for plaintext frames; for encrypted ones
    // the real id lives inside the ciphertext). Used by the device build to observe
    // what the phone sends in the Running phase over the serial console; unused/no-op
    // in host tests. Keep it cheap — it runs in the receive path.
    std::function<void(ChannelId channel, uint16_t wireMsgId, bool encrypted,
                       size_t rawLen)>
        onRxFrame;

    // Diagnostic tap: fired for every frame the HU SENDS, with the resolved channel +
    // msgId + encrypted flag (before framing/encryption). Symmetric to onRxFrame so the
    // device build can confirm on the serial console that it answers the phone's burst.
    std::function<void(ChannelId channel, uint16_t msgId, bool encrypted)> onTxFrame;
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

    // --- rung 4c: outbound channel messages the app drives (rung 6 wiring) ----
    // Grant/revoke video focus to the phone (VIDEO_FOCUS_INDICATION on the video
    // channel) — call when the AA tab gains/loses focus. False until video is open.
    bool sendVideoFocus(VideoFocusMode mode, bool unrequested = false);
    // Forward a touch to the phone (INPUT_EVENT_INDICATION on the input channel).
    // x/y are AA source-frame coords (0..799 / 0..479). False until input is open.
    bool sendTouch(TouchAction action, uint32_t x, uint32_t y, uint64_t timestamp);
    // Send a media transport key (play/pause/next/prev) to the phone as a press +
    // release ButtonEvent on the input channel — lets the HU drive AA media playback
    // from its own Now Playing UI. False until the input channel is open.
    bool sendMediaKey(MediaKey key, uint64_t timestamp);

    // Push the periodic sensor readings the phone needs to enable projection: a
    // DRIVING_STATUS (unrestricted/parked) + NIGHT_MODE event. The phone WAITS on
    // driving status before it will start the AV stream, so the driver must call this
    // on a ~1 Hz tick once Running + the sensor channel is open (matches the reference;
    // a no-op otherwise).
    void sendSensorTick();

private:
    bool sendPlainSpecific(uint16_t msgId, const uint8_t* p, size_t n);
    bool sendPlainControl(uint16_t msgId, const uint8_t* p, size_t n);
    bool sendEncrypted(ChannelId ch, uint16_t msgId, const uint8_t* p, size_t n);
    bool sendEncryptedControl(uint16_t msgId, const uint8_t* p, size_t n);
    // Encrypted CONTROL-type message on an arbitrary channel (e.g. the channel-open
    // response, which the phone expects back on the channel being opened).
    bool sendEncryptedChannelControl(ChannelId ch, uint16_t msgId, const uint8_t* p,
                                     size_t n);

    void handleControl(uint16_t msgId, const uint8_t* p, size_t n);
    void handleVersionResponse(const uint8_t* p, size_t n);
    void handleSslHandshake(const uint8_t* p, size_t n);
    void handleServiceDiscoveryRequest(const uint8_t* p, size_t n);
    // frameCh is the channel the request ARRIVED on — authoritative (the phone sends
    // CHANNEL_OPEN_REQUEST on the target channel, not Control).
    void handleChannelOpenRequest(ChannelId frameCh, const uint8_t* p, size_t n);
    void handlePingRequest(const uint8_t* p, size_t n);
    void handleAudioFocusRequest(const uint8_t* p, size_t n);
    void handleNavFocusRequest(const uint8_t* p, size_t n);

    // Non-control channel dispatch (Running phase).
    void handleChannelMessage(ChannelId ch, uint16_t msgId, const uint8_t* p, size_t n);
    void handleAvChannel(ChannelId ch, uint16_t msgId, const uint8_t* p, size_t n);
    void handleInputChannel(uint16_t msgId, const uint8_t* p, size_t n);
    void handleSensorChannel(uint16_t msgId, const uint8_t* p, size_t n);
    // One ButtonEvent (down or up) for a media key on the input channel.
    bool sendButtonEvent(MediaKey key, bool pressed, uint64_t timestamp);
    void sendAvSetupResponse(ChannelId ch, uint8_t configIndex);
    void sendMediaAck(ChannelId ch);
    void sendSensorEvent(uint32_t sensorType);

    static bool isAvChannel(ChannelId ch) {
        return ch == ChannelId::Video || ch == ChannelId::MediaAudio ||
               ch == ChannelId::SpeechAudio || ch == ChannelId::SystemAudio;
    }

    AaMessenger& msger_;
    IAaCrypto& crypto_;
    AaConnectionCallbacks cbs_;
    AaState state_ = AaState::VersionExchange;
    std::array<bool, static_cast<size_t>(kMaxChannel) + 1> channelOpen_{};
    // Per-AV-channel session id from START_INDICATION, echoed in the media ack.
    std::array<int32_t, static_cast<size_t>(kMaxChannel) + 1> avSession_{};
    // Which sensor types the phone has actually started (SENSOR_START_REQUEST), so the
    // periodic tick only emits events the phone subscribed to — sending unsolicited
    // sensor data (e.g. night mode for a sensor it never started) makes it bail.
    std::array<bool, 32> sensorStarted_{};
};

}  // namespace carsh::aa
