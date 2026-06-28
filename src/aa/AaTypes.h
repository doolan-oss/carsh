// Android Auto protocol constants (channel ids, frame flags, control message ids)
// and the wire-format sizing limits. Platform-independent; ported from the
// dylant9/head_unit reference (aa_types.h), itself derived from aasdk's reverse
// engineering. See AA_PLAN.md.
#pragma once

#include <cstddef>
#include <cstdint>

namespace carsh::aa {

// Channel ids (aasdk ChannelId enum). The control channel (0) carries version
// exchange, the SSL handshake, and service discovery; the rest are opened after.
enum class ChannelId : uint8_t {
    Control = 0,
    Input = 1,
    Sensor = 2,
    Video = 3,
    MediaAudio = 4,
    SpeechAudio = 5,
    SystemAudio = 6,
    AvInput = 7,
    Bluetooth = 8,
    Navigation = 9,
    MediaStatus = 10,
    None = 255,
};
// Highest valid channel id — sizes the per-channel reassembly table.
inline constexpr int kMaxChannel = static_cast<int>(ChannelId::MediaStatus);

// Frame-header flag byte (byte 1) fields, per aasdk FrameType.hpp:
//   bits 1:0 = frame type, bit 2 = message type, bit 3 = encryption.
enum class FrameType : uint8_t {
    Middle = 0x00,
    First = 0x01,
    Last = 0x02,
    Bulk = 0x03,  // First | Last: a complete single-frame message
};
enum class MessageType : uint8_t {
    Specific = 0x00,
    Control = 0x04,
};
enum class EncryptionType : uint8_t {
    Plain = 0x00,
    Encrypted = 0x08,
};

// Control-channel message ids (first 2 bytes of a plain control message payload).
enum class ControlMsgId : uint16_t {
    VersionRequest = 0x0001,
    VersionResponse = 0x0002,
    SslHandshake = 0x0003,
    AuthComplete = 0x0004,
    ServiceDiscoveryRequest = 0x0005,
    ServiceDiscoveryResponse = 0x0006,
    ChannelOpenRequest = 0x0007,
    ChannelOpenResponse = 0x0008,
    PingRequest = 0x000B,
    PingResponse = 0x000C,
    NavFocusRequest = 0x000D,
    NavFocusResponse = 0x000E,
    ShutdownRequest = 0x000F,
    ShutdownResponse = 0x0010,
    VoiceSessionRequest = 0x0011,
    AudioFocusRequest = 0x0012,
    AudioFocusResponse = 0x0013,
};

// AV channel (video / media-audio / speech / system) message ids — the 2-byte id
// prefixing an encrypted AV channel payload (aasdk AVChannelMessageIdsEnum). Media
// data uses the low ids; the 0x80xx ids are the per-channel setup/flow handshake.
enum class AvMsgId : uint16_t {
    MediaWithTimestamp = 0x0000,  // 8-byte PTS then Annex-B / PCM
    MediaIndication = 0x0001,     // raw Annex-B / PCM, no timestamp
    // AVChannelMessageIdsEnum (aasdk) — exact values matter; carsh had these shifted.
    SetupRequest = 0x8000,        // phone -> HU: AVChannelSetupRequest{config_index}
    StartIndication = 0x8001,     // phone -> HU: stream starting {session, config}
    StopIndication = 0x8002,      // phone -> HU: stream stopping
    SetupResponse = 0x8003,       // HU -> phone: AVChannelSetupResponse
    MediaAck = 0x8004,            // HU -> phone: AVMediaAckIndication (flow control)
    AvInputOpenRequest = 0x8005,  // phone -> HU
    AvInputOpenResponse = 0x8006,  // HU -> phone
    VideoFocusRequest = 0x8007,   // phone -> HU: requests video focus (drives AV_START)
    VideoFocusIndication = 0x8008,  // HU -> phone: focus state (reply to the request)
};

// Input channel message ids (aasdk InputChannelMessageIdsEnum). The HU sends touch
// as INPUT_EVENT_INDICATION; the phone binds keycodes via BINDING_REQUEST.
enum class InputMsgId : uint16_t {
    InputEventIndication = 0x8001,  // HU -> phone: touch/key event
    BindingRequest = 0x8002,        // phone -> HU: bind keycodes
    BindingResponse = 0x8003,       // HU -> phone: binding status
};

// Sensor channel message ids (aasdk SensorChannelMessageIdsEnum). The phone starts
// a sensor; the HU streams SENSOR_EVENT_INDICATION (driving status, night mode, …).
enum class SensorMsgId : uint16_t {
    SensorStartRequest = 0x8001,    // phone -> HU: start a sensor
    SensorStartResponse = 0x8002,   // HU -> phone: start status
    SensorEventIndication = 0x8003,  // HU -> phone: sensor data
};

// Sensor type ids (aasdk SensorType) the HU advertises + reports.
enum class SensorType : uint32_t {
    DrivingStatus = 13,
    NightData = 10,
    Location = 21,
};

// Video focus modes (aasdk VideoFocusModeEnum) for VIDEO_FOCUS_INDICATION.
enum class VideoFocusMode : uint32_t {
    Focused = 1,
    Unfocused = 2,
};

// Touch action codes (aasdk TouchActionEnum) for INPUT_EVENT_INDICATION.
enum class TouchAction : uint32_t {
    Press = 0,
    Release = 1,
    Drag = 2,
};

// Protocol version the head unit advertises (matches the DHU: 1.7).
inline constexpr uint16_t kVersionMajor = 1;
inline constexpr uint16_t kVersionMinor = 7;

// Wire-format sizing. A USB bulk transfer caps a single frame; a reassembled
// message can span many frames up to kMaxMessageSize.
inline constexpr size_t kUsbTransferSize = 16384;
// Big enough for a large H.264 video keyframe (reassembled, still-encrypted blob).
// Dropping an over-cap message would desync the TLS stream, so keep generous headroom.
inline constexpr size_t kMaxMessageSize = 1024 * 1024;

// Assemble the frame-header flag byte from its three fields.
inline constexpr uint8_t frameFlags(FrameType ft, MessageType mt, EncryptionType enc) {
    return static_cast<uint8_t>(static_cast<uint8_t>(ft) | static_cast<uint8_t>(mt) |
                                static_cast<uint8_t>(enc));
}

}  // namespace carsh::aa
