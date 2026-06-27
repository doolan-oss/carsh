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

    audioChannel(w, ChannelId::MediaAudio, 3, 48000, 2);   // MEDIA 48k stereo
    audioChannel(w, ChannelId::SpeechAudio, 1, 16000, 1);  // SPEECH 16k mono
    audioChannel(w, ChannelId::SystemAudio, 2, 16000, 1);  // SYSTEM 16k mono

    // AV input (microphone) channel.
    w.submessage(1, [](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(ChannelId::AvInput));
        ch.submessage(5, [](pb::Writer& av) {
            av.submessage(1, [](pb::Writer& ac) {
                ac.uint32(1, 16000);
                ac.uint32(2, 16);
                ac.uint32(3, 1);
            });
        });
    });

    // Sensor channel: driving status, night data, GPS.
    w.submessage(1, [](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(ChannelId::Sensor));
        ch.submessage(2, [](pb::Writer& sc) {
            for (uint32_t type : {13u /*DRIVING_STATUS*/, 10u /*NIGHT_DATA*/, 21u /*GPS*/}) {
                sc.submessage(1, [&](pb::Writer& sensor) { sensor.uint32(1, type); });
            }
        });
    });

    // Input channel: supported buttons + 800x480 touch surface.
    w.submessage(1, [](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(ChannelId::Input));
        ch.submessage(4, [](pb::Writer& ic) {
            for (uint32_t key : {0x03u, 0x04u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x54u,
                                 0x55u, 0x57u, 0x58u, 0x7Eu, 0x7Fu, 0x10000u, 0x10002u}) {
                ic.uint32(1, key);
            }
            ic.submessage(2, [](pb::Writer& tc) {
                tc.uint32(1, 800);
                tc.uint32(2, 480);
            });
        });
    });

    // Navigation channel (status images).
    w.submessage(1, [](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(ChannelId::Navigation));
        ch.submessage(8, [](pb::Writer& nc) {
            nc.uint32(1, 1000);
            nc.uint32(2, 1);
            nc.submessage(3, [](pb::Writer& io) {
                io.uint32(1, 128);
                io.uint32(2, 128);
                io.uint32(3, 160);
            });
        });
    });

    // Media-status channel (presence only).
    w.submessage(1, [](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(ChannelId::MediaStatus));
        ch.submessage(9, [](pb::Writer&) {});
    });

    // Video channel: 480p @ 30fps.
    w.submessage(1, [](pb::Writer& ch) {
        ch.uint32(1, static_cast<uint32_t>(ChannelId::Video));
        ch.submessage(3, [](pb::Writer& av) {
            av.uint32(1, 3);  // stream_type = VIDEO
            av.submessage(4, [](pb::Writer& vc) {
                vc.uint32(1, 1);    // resolution = 480p
                vc.uint32(2, 2);    // fps = 30
                vc.uint32(3, 0);    // margin width
                vc.uint32(4, 0);    // margin height
                vc.uint32(5, 140);  // dpi
            });
            av.boolean(5, true);  // available while in call
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
    : msger_(msger), crypto_(crypto), cbs_(std::move(cbs)) {}

bool AaConnection::channelOpen(ChannelId ch) const {
    const int i = static_cast<int>(ch);
    if (i < 0 || i > kMaxChannel) return false;
    return channelOpen_[static_cast<size_t>(i)];
}

// Version request/handshake/auth go out PLAIN with MessageType::SPECIFIC — matches
// the captured DHU wire format (flags bit 2 clear).
bool AaConnection::sendPlainSpecific(uint16_t msgId, const uint8_t* p, size_t n) {
    return msger_.sendSpecific(ChannelId::Control, EncryptionType::Plain, msgId, p, n);
}
bool AaConnection::sendPlainControl(uint16_t msgId, const uint8_t* p, size_t n) {
    return msger_.sendControl(msgId, EncryptionType::Plain, p, n);
}
// Encrypted control: the 2-byte msg id lives INSIDE the ciphertext, so build
// [msgId][payload], encrypt, and send as a raw encrypted SPECIFIC frame.
bool AaConnection::sendEncryptedControl(uint16_t msgId, const uint8_t* p, size_t n) {
    std::vector<uint8_t> plain;
    plain.reserve(2 + n);
    plain.push_back(static_cast<uint8_t>((msgId >> 8) & 0xFF));
    plain.push_back(static_cast<uint8_t>(msgId & 0xFF));
    plain.insert(plain.end(), p, p + n);
    std::vector<uint8_t> cipher = crypto_.encrypt(plain.data(), plain.size());
    if (cipher.empty()) return false;
    return msger_.sendRaw(ChannelId::Control, EncryptionType::Encrypted,
                          MessageType::Specific, cipher.data(), cipher.size());
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
    sendEncryptedControl(static_cast<uint16_t>(ControlMsgId::ServiceDiscoveryResponse),
                         resp.data(), resp.size());
    state_ = AaState::Running;
}

void AaConnection::handleChannelOpenRequest(const uint8_t* p, size_t n) {
    pb::Reader r(p, n);
    uint32_t field;
    uint8_t wire;
    uint32_t channelId = 255;
    while (!r.eof()) {
        if (!r.tag(field, wire)) break;
        if (field == 2 && wire == 0) {
            if (!r.u32(channelId)) return;
        } else if (!r.skip(wire)) {
            return;
        }
    }
    uint32_t status = 0;  // OK
    if (channelId > static_cast<uint32_t>(kMaxChannel)) {
        status = 1;  // FAIL — invalid channel
    } else {
        channelOpen_[channelId] = true;
        if (cbs_.onChannelOpened) cbs_.onChannelOpened(static_cast<ChannelId>(channelId));
    }
    std::vector<uint8_t> body;
    pb::Writer(body).uint32(1, status);
    sendEncryptedControl(static_cast<uint16_t>(ControlMsgId::ChannelOpenResponse), body.data(),
                         body.size());
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
    sendPlainControl(static_cast<uint16_t>(ControlMsgId::PingResponse), body.data(), body.size());
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
        case ControlMsgId::ChannelOpenRequest:
            handleChannelOpenRequest(p, n);
            break;
        case ControlMsgId::PingRequest:
            handlePingRequest(p, n);
            break;
        case ControlMsgId::ShutdownRequest:
            state_ = AaState::Shutdown;
            if (cbs_.onShutdown) cbs_.onShutdown();
            break;
        default:
            break;
    }
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

    if (msg.encryption == EncryptionType::Encrypted) {
        if (!crypto_.handshakeDone()) return true;  // can't decrypt yet — ignore
        decrypted = crypto_.decrypt(msg.payload.data(), msg.payload.size());
        if (decrypted.size() < 2) return true;
        msgId = static_cast<uint16_t>((decrypted[0] << 8) | decrypted[1]);
        payload = decrypted.data() + 2;
        payloadLen = decrypted.size() - 2;
    }

    if (msg.channel == ChannelId::Control) {
        handleControl(msgId, payload, payloadLen);
    } else if (cbs_.onChannelMessage) {
        cbs_.onChannelMessage(msg.channel, msgId, payload, payloadLen);
    }
    return true;
}

}  // namespace carsh::aa
