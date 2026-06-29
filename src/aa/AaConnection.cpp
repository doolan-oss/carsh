// AaConnection — see AaConnection.h. Faithful port of the reference state machine.
#include "aa/AaConnection.h"

#include <vector>

#include "aa/Pb.h"

namespace carsh::aa {
namespace {

// carsh head-unit identity reported in service discovery.
constexpr char kHuName[] = "carsh";
constexpr char kCarModel[] = "carsh";
constexpr char kCarYear[] = "2026";
constexpr char kCarSerial[] = "CARSH-0001";
constexpr char kHuManufacturer[] = "carsh";
constexpr char kHuModel[] = "RPi4-AA";
constexpr char kSwBuild[] = "1";
constexpr char kSwVersion[] = "1.0.0";

// One audio channel descriptor: channel_id + av_channel{stream_type=AUDIO,
// audio_type, audio_config{rate,bits,channels}}.
void audioChannel(pb::Writer& root, ChannelId id, uint32_t audioType, uint32_t rate,
                  uint32_t channels) {
    root.submessage(1, [&](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(id));
        ch.submessage(3, [&](pb::Writer& av) {
            av.uint32(1, 1);          // stream_type = AUDIO
            av.uint32(2, audioType);  // 1=SPEECH, 2=SYSTEM, 3=MEDIA
            av.submessage(3, [&](pb::Writer& ac) {
                ac.uint32(1, rate);
                ac.uint32(2, 16);
                ac.uint32(3, channels);
            });
        });
    });
}

// Build the ServiceDiscoveryResponse describing carsh's capabilities (audio sink,
// video, input/touch, a few sensors, nav + media-status). Faithful to the
// reference, which mirrors aasdk's ChannelDescriptor layout.
std::vector<uint8_t> buildServiceDiscoveryResponse() {
    std::vector<uint8_t> out;
    pb::Writer w(out);

    // Channel set matches the functional NexusDrive HU exactly: MEDIA_AUDIO, AV_INPUT,
    // SENSOR, INPUT, VIDEO. The previously-advertised SPEECH/SYSTEM audio, NAVIGATION and
    // MEDIA_STATUS channels are omitted — the phone would set them up but they are not
    // part of the minimal set the reference uses to drive video.
    audioChannel(w, ChannelId::MediaAudio, 3, 48000, 2);   // MEDIA 48k stereo

    // AV input (microphone) channel. AVInputChannel: stream_type (field 1) = AUDIO,
    // audio_config = field 2 (not 1) — matches aasdk; a wrong layout makes the phone
    // reject service discovery.
    w.submessage(1, [](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(ChannelId::AvInput));
        ch.submessage(5, [](pb::Writer& av) {
            av.uint32(1, 1);  // stream_type = AUDIO
            av.submessage(2, [](pb::Writer& ac) {  // audio_config
                ac.uint32(1, 16000);
                ac.uint32(2, 16);
                ac.uint32(3, 1);
            });
        });
    });

    // Sensor channel — matches the functional NexusDrive HU: DRIVING_STATUS + NIGHT_DATA
    // only (SensorChannelData has a single field `sensors`, so no trailing fields).
    w.submessage(1, [](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(ChannelId::Sensor));
        ch.submessage(2, [](pb::Writer& sc) {
            for (uint32_t type : {13u /*DRIVING_STATUS*/, 10u /*NIGHT_DATA*/}) {
                sc.submessage(1, [&](pb::Writer& sensor) { sensor.uint32(1, type); });
            }
        });
    });

    // Input channel: supported buttons + touch surface. The touch surface MUST match the
    // advertised video resolution (1280x720) — a mismatch (we used 800x480 while
    // advertising 720p video) is geometrically inconsistent and the phone sets up the
    // video channel but never STARTS the stream. The DHU advertises {1280,720,2,0}.
    w.submessage(1, [](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(ChannelId::Input));
        ch.submessage(4, [](pb::Writer& ic) {
            for (uint32_t key : {0x03u, 0x04u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x54u,
                                 0x55u, 0x57u, 0x58u, 0x7Eu, 0x7Fu, 0x10000u, 0x10002u}) {
                ic.uint32(1, key);
            }
            ic.submessage(2, [](pb::Writer& tc) {
                tc.uint32(1, 1280);  // width  (TouchConfig has only width + height)
                tc.uint32(2, 720);   // height
            });
        });
    });

    // Video channel: 720p @ 60fps. Replicated byte-for-byte from the Google Desktop
    // Head Unit's service-discovery video MediaSinkService (captured 2026-06-28). The
    // previous minimal config (resolution+fps+dpi only, plus a bogus available_type=3
    // and available_while_in_call at field 5) was mis-parsed by the phone: it requested
    // a bogus config_index and set up the channel but never started the stream. The DHU
    // omits available_type for video and sends the full VideoConfig field set.
    w.submessage(1, [](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(ChannelId::Video));
        ch.submessage(3, [](pb::Writer& av) {       // av_channel (AVChannelData)
            av.uint32(1, 3);                        // stream_type = VIDEO (REQUIRED — without
                                                    // it the phone sets up the channel but
                                                    // never knows it's video, so never starts)
            av.submessage(4, [](pb::Writer& vc) {   // video_configs[0] (VideoConfigData)
                vc.uint32(1, 2);      // video_resolution = _720p
                vc.uint32(2, 1);      // video_fps = _30
                vc.uint32(3, 0);      // margin_width
                vc.uint32(4, 0);      // margin_height
                vc.uint32(5, 160);    // dpi
                vc.uint32(6, 0);      // additional_depth
            });
        });
    });

    // Head-unit metadata (required fields).
    w.string(2, kHuName);
    w.string(3, kCarModel);
    w.string(4, kCarYear);
    w.string(5, kCarSerial);
    w.boolean(6, true);  // left-hand drive
    w.string(7, kHuManufacturer);
    w.string(8, kHuModel);
    w.string(9, kSwBuild);
    w.string(10, kSwVersion);
    w.boolean(11, false);
    w.boolean(12, false);
    return out;
}

}  // namespace

AaConnection::AaConnection(AaMessenger& msger, IAaCrypto& crypto, AaConnectionCallbacks cbs)
    : msger_(msger), crypto_(crypto), cbs_(std::move(cbs)) {
    // Decrypt ENCRYPTED frames per-frame inside the messenger (aasdk model) — required
    // for multi-frame (large keyframe) messages; see AaMessenger::setDecryptor.
    msger_.setDecryptor([this](const uint8_t* p, size_t n) -> std::vector<uint8_t> {
        if (!crypto_.handshakeDone()) return {};
        return crypto_.decrypt(p, n);
    });
}

bool AaConnection::channelOpen(ChannelId ch) const {
    const int i = static_cast<int>(ch);
    if (i < 0 || i > kMaxChannel) return false;
    return channelOpen_[static_cast<size_t>(i)];
}

// Version request/handshake/auth go out PLAIN with MessageType::SPECIFIC — matches
// the captured DHU wire format (flags bit 2 clear).
bool AaConnection::sendPlainSpecific(uint16_t msgId, const uint8_t* p, size_t n) {
    if (cbs_.onTxFrame) cbs_.onTxFrame(ChannelId::Control, msgId, false);
    return msger_.sendSpecific(ChannelId::Control, EncryptionType::Plain, msgId, p, n);
}
bool AaConnection::sendPlainControl(uint16_t msgId, const uint8_t* p, size_t n) {
    if (cbs_.onTxFrame) cbs_.onTxFrame(ChannelId::Control, msgId, false);
    return msger_.sendControl(msgId, EncryptionType::Plain, p, n);
}
// Encrypted send on any channel: the 2-byte msg id lives INSIDE the ciphertext, so
// build [msgId][payload], encrypt, and send as a raw encrypted SPECIFIC frame.
bool AaConnection::sendEncrypted(ChannelId ch, uint16_t msgId, const uint8_t* p, size_t n) {
    if (cbs_.onTxFrame) cbs_.onTxFrame(ch, msgId, true);
    std::vector<uint8_t> plain;
    plain.reserve(2 + n);
    plain.push_back(static_cast<uint8_t>((msgId >> 8) & 0xFF));
    plain.push_back(static_cast<uint8_t>(msgId & 0xFF));
    plain.insert(plain.end(), p, p + n);
    std::vector<uint8_t> cipher = crypto_.encrypt(plain.data(), plain.size());
    if (cipher.empty()) return false;
    return msger_.sendRaw(ch, EncryptionType::Encrypted, MessageType::Specific,
                          cipher.data(), cipher.size());
}
bool AaConnection::sendEncryptedControl(uint16_t msgId, const uint8_t* p, size_t n) {
    return sendEncrypted(ChannelId::Control, msgId, p, n);
}
// Like sendEncrypted but with MessageType::Control (the channel-open response is a
// CONTROL-type message carried on the target channel, per aasdk / the reference).
bool AaConnection::sendEncryptedChannelControl(ChannelId ch, uint16_t msgId,
                                               const uint8_t* p, size_t n) {
    if (cbs_.onTxFrame) cbs_.onTxFrame(ch, msgId, true);
    std::vector<uint8_t> plain;
    plain.reserve(2 + n);
    plain.push_back(static_cast<uint8_t>((msgId >> 8) & 0xFF));
    plain.push_back(static_cast<uint8_t>(msgId & 0xFF));
    plain.insert(plain.end(), p, p + n);
    std::vector<uint8_t> cipher = crypto_.encrypt(plain.data(), plain.size());
    if (cipher.empty()) return false;
    return msger_.sendRaw(ch, EncryptionType::Encrypted, MessageType::Control,
                          cipher.data(), cipher.size());
}

void AaConnection::start() {
    // Version exchange uses raw big-endian uint16 pairs, NOT protobuf (per aasdk).
    const uint8_t body[] = {
        static_cast<uint8_t>((kVersionMajor >> 8) & 0xFF), static_cast<uint8_t>(kVersionMajor & 0xFF),
        static_cast<uint8_t>((kVersionMinor >> 8) & 0xFF), static_cast<uint8_t>(kVersionMinor & 0xFF)};
    sendPlainSpecific(static_cast<uint16_t>(ControlMsgId::VersionRequest), body, sizeof(body));
    state_ = AaState::VersionExchange;
}

void AaConnection::handleVersionResponse(const uint8_t* p, size_t n) {
    if (n < 4) return;
    const uint16_t status = (n >= 6) ? static_cast<uint16_t>((p[4] << 8) | p[5]) : 0;
    if (status != 0) {  // version rejected by the phone
        state_ = AaState::Shutdown;
        return;
    }
    state_ = AaState::SslHandshake;
    // Kick off the TLS handshake with an empty step → ClientHello-equivalent.
    std::vector<uint8_t> hs = crypto_.handshakeStep(nullptr, 0);
    if (!hs.empty()) {
        sendPlainSpecific(static_cast<uint16_t>(ControlMsgId::SslHandshake), hs.data(), hs.size());
    }
}

void AaConnection::handleSslHandshake(const uint8_t* p, size_t n) {
    std::vector<uint8_t> resp = crypto_.handshakeStep(p, n);
    if (!resp.empty()) {
        sendPlainSpecific(static_cast<uint16_t>(ControlMsgId::SslHandshake), resp.data(),
                          resp.size());
    }
    if (crypto_.handshakeDone()) {
        state_ = AaState::Auth;
        // AUTH_COMPLETE is sent PLAIN with an AuthCompleteIndication{status=OK}
        // protobuf, immediately after the TLS Finished and BEFORE encrypted
        // traffic begins (sending it encrypted stalls the phone's auth state).
        std::vector<uint8_t> authBuf;
        pb::Writer(authBuf).uint32(1, 0);  // status = OK
        sendPlainSpecific(static_cast<uint16_t>(ControlMsgId::AuthComplete), authBuf.data(),
                          authBuf.size());
        state_ = AaState::ServiceDiscovery;
    }
}

void AaConnection::handleServiceDiscoveryRequest(const uint8_t* /*p*/, size_t /*n*/) {
    std::vector<uint8_t> resp = buildServiceDiscoveryResponse();
    // SPECIFIC, not control (aasdk ControlServiceChannel::sendServiceDiscoveryResponse).
    sendEncrypted(ChannelId::Control,
                  static_cast<uint16_t>(ControlMsgId::ServiceDiscoveryResponse),
                  resp.data(), resp.size());
    state_ = AaState::Running;
}

// CHANNEL_OPEN_REQUEST arrives ON the channel being opened (a CONTROL-type message on
// the target channel), NOT on the control channel — the frame channel is authoritative
// (the payload's channel_id field is only advisory). Reply CHANNEL_OPEN_RESPONSE on
// that same channel. Routing these only on Control (so they were dropped) made the
// phone abort right after service discovery (bench-confirmed; matches the reference).
void AaConnection::handleChannelOpenRequest(ChannelId frameCh, const uint8_t* /*p*/,
                                            size_t /*n*/) {
    const int idx = static_cast<int>(frameCh);
    uint32_t status = 0;  // OK
    if (idx < 0 || idx > kMaxChannel) {
        status = 1;  // FAIL — invalid channel
    } else {
        channelOpen_[static_cast<size_t>(idx)] = true;
        if (cbs_.onChannelOpened) cbs_.onChannelOpened(frameCh);
    }
    std::vector<uint8_t> body;
    pb::Writer(body).uint32(1, status);
    sendEncryptedChannelControl(frameCh,
                                static_cast<uint16_t>(ControlMsgId::ChannelOpenResponse),
                                body.data(), body.size());
    // NOTE: no VideoFocusRequest on video-channel-open. The reference HU (DHU) sends NO
    // video focus at all during setup — with the correct service-discovery video config
    // the phone sets up AND starts video on its own; focus is sent only later (after the
    // stream is live). Prompting here was a workaround for the previously-wrong config.
}

void AaConnection::handlePingRequest(const uint8_t* p, size_t n) {
    pb::Reader r(p, n);
    uint32_t field;
    uint8_t wire;
    int32_t id = 0;
    uint64_t timestamp = 0;
    while (!r.eof()) {
        if (!r.tag(field, wire)) break;
        if (field == 1 && wire == 0) {
            uint64_t v;
            if (r.varint(v)) id = static_cast<int32_t>(v);
        } else if (field == 2 && wire == 0) {
            r.varint(timestamp);
        } else if (!r.skip(wire)) {
            break;
        }
    }
    std::vector<uint8_t> body;
    pb::Writer w(body);
    w.int32(1, id);
    w.tag(0, 2);
    w.varint(timestamp);
    sendPlainSpecific(static_cast<uint16_t>(ControlMsgId::PingResponse), body.data(), body.size());
}

// The phone requests audio focus right after service discovery and BLOCKS waiting
// for AUDIO_FOCUS_RESPONSE — without it, it tears the link down after ~1s (observed
// on the bench). Faithful to the reference (head_unit/aa_channels.c).
void AaConnection::handleAudioFocusRequest(const uint8_t* p, size_t n) {
    // AudioFocusRequest{ focus_type = 1 (AudioFocusType enum) }.
    uint32_t focusType = 0;
    pb::Reader r(p, n);
    uint32_t field;
    uint8_t wire;
    while (!r.eof()) {
        if (!r.tag(field, wire)) break;
        if (field == 1 && wire == 0) {
            r.u32(focusType);
        } else if (!r.skip(wire)) {
            break;
        }
    }
    // Map AudioFocusType (request) -> AudioFocusState (response) — DIFFERENT enums.
    // Request: GAIN=1, GAIN_TRANSIENT=2, GAIN_NAVI=3, RELEASE=4. State: GAIN=1,
    // GAIN_TRANSIENT=2, LOSS=3. Echoing the value would mis-map RELEASE(4) to
    // LOSS_TRANSIENT_CAN_DUCK(4), so map explicitly.
    uint32_t focusState;
    switch (focusType) {
        case 2: focusState = 2; break;  // GAIN_TRANSIENT -> GAIN_TRANSIENT
        case 4: focusState = 3; break;  // RELEASE -> LOSS
        default: focusState = 1; break; // GAIN / GAIN_NAVI / unknown -> GAIN
    }
    std::vector<uint8_t> body;
    pb::Writer(body).uint32(1, focusState);
    sendEncrypted(ChannelId::Control,
                  static_cast<uint16_t>(ControlMsgId::AudioFocusResponse), body.data(),
                  body.size());
}

// Always grant navigation focus. NavFocusResponse{ focus_type = 1 (GAIN) }.
void AaConnection::handleNavFocusRequest(const uint8_t* /*p*/, size_t /*n*/) {
    std::vector<uint8_t> body;
    pb::Writer(body).uint32(1, 1);  // GAIN
    sendEncrypted(ChannelId::Control,
                  static_cast<uint16_t>(ControlMsgId::NavFocusResponse), body.data(),
                  body.size());
}

void AaConnection::handleControl(uint16_t msgId, const uint8_t* p, size_t n) {
    switch (static_cast<ControlMsgId>(msgId)) {
        case ControlMsgId::VersionResponse:
            handleVersionResponse(p, n);
            break;
        case ControlMsgId::SslHandshake:
            handleSslHandshake(p, n);
            break;
        case ControlMsgId::ServiceDiscoveryRequest:
            handleServiceDiscoveryRequest(p, n);
            break;
        // ChannelOpenRequest is routed by id in poll() (it arrives on the target
        // channel, not Control), so it never reaches here.
        case ControlMsgId::PingRequest:
            handlePingRequest(p, n);
            break;
        case ControlMsgId::AudioFocusRequest:
            handleAudioFocusRequest(p, n);
            break;
        case ControlMsgId::NavFocusRequest:
            handleNavFocusRequest(p, n);
            break;
        case ControlMsgId::ShutdownRequest:
            state_ = AaState::Shutdown;
            if (cbs_.onShutdown) cbs_.onShutdown();
            break;
        default:
            break;
    }
}

// --- rung 4c: per-channel dispatch in the Running phase ----------------------

void AaConnection::handleChannelMessage(ChannelId ch, uint16_t msgId, const uint8_t* p,
                                        size_t n) {
    if (isAvChannel(ch)) {
        handleAvChannel(ch, msgId, p, n);
    } else if (ch == ChannelId::Input) {
        handleInputChannel(msgId, p, n);
    } else if (ch == ChannelId::Sensor) {
        handleSensorChannel(msgId, p, n);
    } else if (cbs_.onChannelMessage) {
        // Nav / media-status / bluetooth: not decoded here yet.
        cbs_.onChannelMessage(ch, msgId, p, n);
    }
}

void AaConnection::handleAvChannel(ChannelId ch, uint16_t msgId, const uint8_t* p, size_t n) {
    const size_t i = static_cast<size_t>(ch);
    switch (static_cast<AvMsgId>(msgId)) {
        case AvMsgId::SetupRequest: {
            // AVChannelSetupRequest{ config_index = 1 (uint32) }.
            uint32_t configIndex = 0;
            pb::Reader r(p, n);
            uint32_t field;
            uint8_t wire;
            while (!r.eof()) {
                if (!r.tag(field, wire)) break;
                if (field == 1 && wire == 0) {
                    r.u32(configIndex);
                } else if (!r.skip(wire)) {
                    break;
                }
            }
            sendAvSetupResponse(ch, static_cast<uint8_t>(configIndex));
            if (cbs_.onAvSetup) cbs_.onAvSetup(ch, static_cast<uint8_t>(configIndex));
            // NOTE: do NOT send VideoFocusRequest here. The reference HU (DHU) sends
            // video focus only AFTER the phone has already started the stream — sending
            // it repeatedly during the phone's setup sequence makes the phone bail. The
            // single prompt at channel-open is enough to drive the phone to set up video.
            break;
        }
        case AvMsgId::StartIndication: {
            // AVChannelStartIndication{ session = 1 (int32), config = 2 (uint32) }.
            pb::Reader r(p, n);
            uint32_t field;
            uint8_t wire;
            while (!r.eof()) {
                if (!r.tag(field, wire)) break;
                if (field == 1 && wire == 0) {
                    uint64_t v;
                    if (r.varint(v)) avSession_[i] = static_cast<int32_t>(v);
                } else if (!r.skip(wire)) {
                    break;
                }
            }
            if (cbs_.onAvStart) cbs_.onAvStart(ch, true);
            break;
        }
        case AvMsgId::StopIndication:
            if (cbs_.onAvStart) cbs_.onAvStart(ch, false);
            break;
        case AvMsgId::VideoFocusRequest: {
            // VideoFocusRequest{ disp_channel = 1, focus_mode = 2 (enum) }.
            uint32_t mode = static_cast<uint32_t>(VideoFocusMode::Focused);
            pb::Reader r(p, n);
            uint32_t field;
            uint8_t wire;
            while (!r.eof()) {
                if (!r.tag(field, wire)) break;
                if (field == 2 && wire == 0) {
                    r.u32(mode);
                } else if (!r.skip(wire)) {
                    break;
                }
            }
            // REACTIVE focus: reply with VideoFocusIndication echoing the requested mode
            // (the functional NexusDrive HU does exactly this). This is what unblocks the
            // phone to send the StartIndication and begin streaming.
            sendVideoFocus(static_cast<VideoFocusMode>(mode), /*unrequested=*/false);
            if (cbs_.onVideoFocusRequest)
                cbs_.onVideoFocusRequest(static_cast<VideoFocusMode>(mode));
            break;
        }
        case AvMsgId::MediaWithTimestamp:
        case AvMsgId::MediaIndication: {
            const bool tsPresent =
                static_cast<AvMsgId>(msgId) == AvMsgId::MediaWithTimestamp;
            uint64_t ts = 0;
            const uint8_t* data = p;
            size_t len = n;
            if (tsPresent) {
                if (n < 8) {
                    sendMediaAck(ch);  // malformed — still ack to keep the stream flowing
                    break;
                }
                for (int b = 0; b < 8; ++b) ts = (ts << 8) | p[b];
                data = p + 8;
                len = n - 8;
            }
            // Flow control: ack first so the phone keeps streaming, then deliver.
            sendMediaAck(ch);
            if (cbs_.onAvMedia && len > 0) cbs_.onAvMedia(ch, tsPresent, ts, data, len);
            break;
        }
        default:
            if (cbs_.onChannelMessage) cbs_.onChannelMessage(ch, msgId, p, n);
            break;
    }
}

void AaConnection::handleInputChannel(uint16_t msgId, const uint8_t* p, size_t n) {
    if (static_cast<InputMsgId>(msgId) == InputMsgId::BindingRequest) {
        // BindingResponse{ status = 1 (int32) = 0 (OK) }.
        std::vector<uint8_t> body;
        pb::Writer(body).int32(1, 0);
        // BINDING_RESPONSE is SPECIFIC (aasdk InputServiceChannel::sendBindingResponse).
        sendEncrypted(ChannelId::Input,
                      static_cast<uint16_t>(InputMsgId::BindingResponse), body.data(),
                      body.size());
    } else if (cbs_.onChannelMessage) {
        cbs_.onChannelMessage(ChannelId::Input, msgId, p, n);
    }
}

void AaConnection::handleSensorChannel(uint16_t msgId, const uint8_t* p, size_t n) {
    if (static_cast<SensorMsgId>(msgId) == SensorMsgId::SensorStartRequest) {
        // SensorStartRequestMessage{ sensor_type = 1 (enum), refresh_interval = 2 }.
        uint32_t sensorType = 0;
        pb::Reader r(p, n);
        uint32_t field;
        uint8_t wire;
        while (!r.eof()) {
            if (!r.tag(field, wire)) break;
            if (field == 1 && wire == 0) {
                r.u32(sensorType);
            } else if (!r.skip(wire)) {
                break;
            }
        }
        // SensorStartResponseMessage{ status = 1 (int32) = 0 (OK) }.
        std::vector<uint8_t> resp;
        pb::Writer(resp).int32(1, 0);
        // SENSOR_START_RESPONSE is SPECIFIC (aasdk SensorServiceChannel::sendSensorStartResponse).
        sendEncrypted(ChannelId::Sensor,
                      static_cast<uint16_t>(SensorMsgId::SensorStartResponse), resp.data(),
                      resp.size());
        // Remember this sensor as started so the periodic tick only feeds subscribed
        // sensors, and push one reading immediately so the phone unblocks quickly.
        if (sensorType < sensorStarted_.size()) sensorStarted_[sensorType] = true;
        sendSensorEvent(sensorType);
    } else if (cbs_.onChannelMessage) {
        cbs_.onChannelMessage(ChannelId::Sensor, msgId, p, n);
    }
}

// SensorEventIndication carries one repeated sub-message per sensor, and aasdk's
// field number for each equals the SensorType id (driving_status = 13, night = 10,
// matching buildServiceDiscoveryResponse above), so encode generically by type.
void AaConnection::sendSensorEvent(uint32_t sensorType) {
    std::vector<uint8_t> body;
    pb::Writer w(body);
    if (sensorType == static_cast<uint32_t>(SensorType::DrivingStatus)) {
        // driving_status[13] { status[1] = 0 (UNRESTRICTED) }.
        w.submessage(static_cast<uint32_t>(SensorType::DrivingStatus),
                     [](pb::Writer& s) { s.uint32(1, 0); });
    } else if (sensorType == static_cast<uint32_t>(SensorType::NightData)) {
        // night_mode[10] { is_night[1] = false }.
        w.submessage(static_cast<uint32_t>(SensorType::NightData),
                     [](pb::Writer& s) { s.boolean(1, false); });
    } else {
        return;  // GPS/location etc. need real data — omit (phone tolerates absence).
    }
    sendEncrypted(ChannelId::Sensor,
                  static_cast<uint16_t>(SensorMsgId::SensorEventIndication), body.data(),
                  body.size());
}

void AaConnection::sendSensorTick() {
    if (state_ != AaState::Running || !channelOpen(ChannelId::Sensor)) return;
    // Only emit events for sensors the phone actually started — sending data for an
    // unstarted sensor makes the phone reject the link. The phone blocks on driving
    // status before starting the AV stream, so feed the subscribed ones each tick.
    for (uint32_t t : {static_cast<uint32_t>(SensorType::DrivingStatus),
                       static_cast<uint32_t>(SensorType::NightData)}) {
        if (t < sensorStarted_.size() && sensorStarted_[t]) sendSensorEvent(t);
    }
}

void AaConnection::sendAvSetupResponse(ChannelId ch, uint8_t /*configIndex*/) {
    // AVChannelSetupResponse{ media_status = 1 (OK), max_unacked = 2, configs = 3 }.
    // configs is the index of the config the HU SELECTED from its OWN advertised list —
    // NOT an echo of the phone's requested config_index. The phone asks for its preferred
    // index (e.g. 3) but the HU answers with the config it will actually use; we advertise
    // one config per AV channel, so that's index 0. Echoing the phone's 3 (a config we
    // never advertised) made the phone set up the channel but never START the stream.
    // Captured from the Google DHU: it answers configs=[0], max_unacked=2.
    std::vector<uint8_t> body;
    pb::Writer w(body);
    w.uint32(1, 2);                  // media_status = OK (AVChannelSetupStatus::OK = 2)
    w.uint32(2, 1);                  // max_unacked (matches the functional NexusDrive HU)
    w.uint32(3, 0);                  // configs[0] = the HU's selected (only) config index
    // SETUP_RESPONSE is SPECIFIC, NOT control (aasdk VideoServiceChannel/AudioServiceChannel
    // ::sendAVChannelSetupResponse). Only CHANNEL_OPEN_RESPONSE is control. carsh sending
    // this as control is what made the phone set the channel up but never START streaming.
    sendEncrypted(ch, static_cast<uint16_t>(AvMsgId::SetupResponse), body.data(),
                  body.size());
}

void AaConnection::sendMediaAck(ChannelId ch) {
    // AVMediaAckIndication{ session = 1 (int32), ack_count = 2 (uint32) }.
    std::vector<uint8_t> body;
    pb::Writer w(body);
    w.int32(1, avSession_[static_cast<size_t>(ch)]);
    w.uint32(2, 1);
    sendEncrypted(ch, static_cast<uint16_t>(AvMsgId::MediaAck), body.data(), body.size());
}

bool AaConnection::sendVideoFocus(VideoFocusMode mode, bool unrequested) {
    if (!channelOpen(ChannelId::Video)) return false;
    // VideoFocusIndication{ focus_mode = 1, unrequested = 2 (bool) }. The phone DRIVES
    // video focus: it sends VideoFocusRequest and the HU replies with this Indication,
    // echoing the requested mode (unrequested=false). Matches the functional NexusDrive
    // HU (aasdk). The phone then sends the StartIndication and streams H.264.
    std::vector<uint8_t> body;
    pb::Writer w(body);
    w.uint32(1, static_cast<uint32_t>(mode));
    w.boolean(2, unrequested);
    return sendEncrypted(ChannelId::Video,
                         static_cast<uint16_t>(AvMsgId::VideoFocusIndication), body.data(),
                         body.size());
}

bool AaConnection::sendTouch(TouchAction action, uint32_t x, uint32_t y,
                             uint64_t timestamp) {
    if (!channelOpen(ChannelId::Input)) return false;
    // InputEventIndication{ timestamp = 1 (int64), touch_event = 2 {
    //   touch_location[1] { x[1], y[2], pointer_id[3] }, touch_action[2], action_index[3] } }.
    std::vector<uint8_t> body;
    pb::Writer w(body);
    w.tag(0, 1);
    w.varint(timestamp);
    w.submessage(2, [&](pb::Writer& te) {
        te.submessage(1, [&](pb::Writer& loc) {
            loc.uint32(1, x);
            loc.uint32(2, y);
            loc.uint32(3, 0);  // pointer_id
        });
        te.uint32(2, static_cast<uint32_t>(action));
        te.uint32(3, 0);  // action_index
    });
    return sendEncrypted(ChannelId::Input,
                         static_cast<uint16_t>(InputMsgId::InputEventIndication),
                         body.data(), body.size());
}

bool AaConnection::sendButtonEvent(MediaKey key, bool pressed, uint64_t timestamp) {
    if (!channelOpen(ChannelId::Input)) return false;
    // InputEventIndication{ timestamp = 1 (int64), button_event = 3 {  // ButtonEvents
    //   button_events[1] { scan_code[1], is_pressed[2] } } }.
    // NOTE: button_event is field 3 (not 4) because carsh follows the older aasdk
    // layout WITHOUT disp_channel — the same layout its hardware-validated sendTouch
    // uses (touch_event = field 2). On aasdk master (with disp_channel = 2) these
    // shift to touch_event = 3 / button_event = 4; keep these in lockstep with
    // sendTouch above. [hw] confirm the phone binds + acts on the key on the bench.
    std::vector<uint8_t> body;
    pb::Writer w(body);
    w.tag(0, 1);  // timestamp (varint, field 1)
    w.varint(timestamp);
    w.submessage(3, [&](pb::Writer& be) {            // button_event (ButtonEvents)
        be.submessage(1, [&](pb::Writer& b) {        // button_events[0] (ButtonEvent)
            b.uint32(1, static_cast<uint32_t>(key));  // scan_code
            b.boolean(2, pressed);                    // is_pressed
        });
    });
    return sendEncrypted(ChannelId::Input,
                         static_cast<uint16_t>(InputMsgId::InputEventIndication),
                         body.data(), body.size());
}

bool AaConnection::sendMediaKey(MediaKey key, uint64_t timestamp) {
    // A key press is a down followed by an up (mirrors sendTouch's press/release).
    const bool down = sendButtonEvent(key, true, timestamp);
    const bool up = sendButtonEvent(key, false, timestamp);
    return down && up;
}

bool AaConnection::poll(uint32_t timeoutMs) {
    if (state_ == AaState::Shutdown) return false;

    AaMessage msg;
    if (!msger_.receive(msg, timeoutMs)) return false;

    // Decrypt-then-dispatch. PLAIN messages already carry the message id (the
    // SSL_HANDSHAKE payload is raw TLS, handled as-is). ENCRYPTED messages hold a
    // TLS record whose plaintext is [2B msg id][protobuf].
    uint16_t msgId = msg.messageId;
    std::vector<uint8_t> decrypted;
    const uint8_t* payload = msg.payload.data();
    size_t payloadLen = msg.payload.size();
    const bool encrypted = (msg.encryption == EncryptionType::Encrypted);

    // The messenger decrypts ENCRYPTED frames per-frame and sets msg.decrypted, leaving
    // msg.messageId + msg.payload ready (id stripped). The branch below is a fallback for
    // a messenger with no decryptor wired (shouldn't happen in production).
    if (encrypted && !msg.decrypted) {
        if (!crypto_.handshakeDone()) {
            if (cbs_.onRxFrame) cbs_.onRxFrame(msg.channel, 0xFFF0, true, msg.payload.size());
            return true;
        }
        decrypted = crypto_.decrypt(msg.payload.data(), msg.payload.size());
        if (decrypted.size() < 2) {
            if (cbs_.onRxFrame) cbs_.onRxFrame(msg.channel, 0xFFF1, true, decrypted.size());
            return true;
        }
        msgId = static_cast<uint16_t>((decrypted[0] << 8) | decrypted[1]);
        payload = decrypted.data() + 2;
        payloadLen = decrypted.size() - 2;
    }

    // Diagnostic tap: the fully-resolved frame (real msgId after decrypt, decrypted
    // length) so the device build can log exactly what the phone sends, especially in
    // the Running phase. Sentinels 0xFFF0/0xFFF1 above flag the decrypt-failure paths.
    if (cbs_.onRxFrame) cbs_.onRxFrame(msg.channel, msgId, encrypted, payloadLen);

    // CHANNEL_OPEN_REQUEST arrives on the TARGET channel (not Control), so route it by
    // id regardless of channel; the handler replies on that same channel. Everything
    // else: control ids on Control, channel messages elsewhere.
    if (msgId == static_cast<uint16_t>(ControlMsgId::ChannelOpenRequest)) {
        handleChannelOpenRequest(msg.channel, payload, payloadLen);
    } else if (msg.channel == ChannelId::Control) {
        handleControl(msgId, payload, payloadLen);
    } else {
        handleChannelMessage(msg.channel, msgId, payload, payloadLen);
    }
    return true;
}

}  // namespace carsh::aa
