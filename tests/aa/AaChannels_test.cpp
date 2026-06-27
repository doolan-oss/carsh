// Per-channel handler tests (AA_PLAN rung 4c): drive the connection into the
// Running phase, open the AV / input / sensor channels, then exercise the
// structured dispatch — AV setup/start/media/stop with flow-control acks, input
// binding + outbound touch, sensor start, and video focus. Same scripted-phone +
// identity-crypto discipline as AaConnection_test, so the protobuf the HU emits is
// decoded back and asserted. The bytes-on-the-wire acceptance by a real phone is
// [hw]; this proves the framing, routing, and message shapes host-side.
#include "aa/AaConnection.h"

#include <vector>

#include "Test.h"
#include "aa/AaTypes.h"
#include "aa/FakeAaCrypto.h"
#include "aa/FakeAaTransport.h"
#include "aa/Pb.h"

using namespace carsh;
using carsh::aa::AaConnection;
using carsh::aa::AaMessage;
using carsh::aa::AaMessenger;
using carsh::aa::AaState;
using carsh::aa::AvMsgId;
using carsh::aa::ChannelId;
using carsh::aa::EncryptionType;
using carsh::aa::InputMsgId;
using carsh::aa::MessageType;
using carsh::aa::SensorMsgId;
using carsh::aa::TouchAction;
using carsh::aa::VideoFocusMode;
using carsh::host::FakeAaCrypto;
using carsh::host::FakeAaTransport;
namespace pb = carsh::aa::pb;

namespace {

// Frame a phone->HU message into the head unit's transport. Encrypted messages use
// identity crypto: the wire payload is [2B msgId][body].
void phoneSend(FakeAaTransport& hu, ChannelId ch, EncryptionType enc, uint16_t msgId,
               const std::vector<uint8_t>& body) {
    FakeAaTransport t;
    AaMessenger m(t);
    if (enc == EncryptionType::Encrypted) {
        std::vector<uint8_t> plain;
        plain.push_back(static_cast<uint8_t>(msgId >> 8));
        plain.push_back(static_cast<uint8_t>(msgId & 0xFF));
        plain.insert(plain.end(), body.begin(), body.end());
        m.sendRaw(ch, EncryptionType::Encrypted, MessageType::Specific, plain.data(),
                  plain.size());
    } else {
        m.send(ch, EncryptionType::Plain, MessageType::Specific, msgId, body.data(),
               body.size());
    }
    hu.pushRx(t.sent());
}

// All HU sends so far, decoded into messages with the (identity-)encrypted msg id
// exposed and the 2-byte id stripped from the payload.
std::vector<AaMessage> parseHuSends(const FakeAaTransport& hu) {
    FakeAaTransport t;
    t.pushRx(hu.sent());
    AaMessenger m(t);
    std::vector<AaMessage> out;
    AaMessage msg;
    while (m.receive(msg, 0)) {
        if (msg.encryption == EncryptionType::Encrypted && msg.payload.size() >= 2) {
            msg.messageId = static_cast<uint16_t>((msg.payload[0] << 8) | msg.payload[1]);
            msg.payload.erase(msg.payload.begin(), msg.payload.begin() + 2);
        }
        out.push_back(msg);
    }
    return out;
}

// The body of the last HU-sent message on (ch,msgId), or empty if none.
std::vector<uint8_t> lastHuBody(const FakeAaTransport& hu, ChannelId ch, uint16_t msgId) {
    std::vector<uint8_t> body;
    for (const auto& m : parseHuSends(hu)) {
        if (m.channel == ch && m.messageId == msgId) body = m.payload;
    }
    return body;
}

bool huSent(const FakeAaTransport& hu, ChannelId ch, uint16_t msgId) {
    for (const auto& m : parseHuSends(hu)) {
        if (m.channel == ch && m.messageId == msgId) return true;
    }
    return false;
}

constexpr uint16_t kVersionResponse = 0x0002;
constexpr uint16_t kSslHandshake = 0x0003;
constexpr uint16_t kSvcDiscoveryReq = 0x0005;
constexpr uint16_t kChannelOpenReq = 0x0007;

// Drive version -> handshake -> auth -> service discovery so state == Running.
void driveToRunning(AaConnection& conn, FakeAaTransport& hu) {
    conn.start();
    phoneSend(hu, ChannelId::Control, EncryptionType::Plain, kVersionResponse,
              {0x00, 0x01, 0x00, 0x07, 0x00, 0x00});
    REQUIRE(conn.poll(0));
    phoneSend(hu, ChannelId::Control, EncryptionType::Plain, kSslHandshake, {0x01});
    REQUIRE(conn.poll(0));
    std::vector<uint8_t> sdReq;
    pb::Writer(sdReq).string(4, "Pixel");
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, kSvcDiscoveryReq, sdReq);
    REQUIRE(conn.poll(0));
    REQUIRE(conn.state() == AaState::Running);
}

void openChannel(AaConnection& conn, FakeAaTransport& hu, ChannelId ch) {
    std::vector<uint8_t> openReq;
    pb::Writer(openReq).uint32(2, static_cast<uint32_t>(ch));
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, kChannelOpenReq, openReq);
    REQUIRE(conn.poll(0));
    REQUIRE(conn.channelOpen(ch));
}

}  // namespace

TEST(AaChannels, VideoSetupStartMediaStop) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    bool setupSeen = false, startSeen = false, stopSeen = false;
    std::vector<uint8_t> mediaSeen;
    bool mediaTs = false;
    uint64_t mediaPts = 0;
    carsh::aa::AaConnectionCallbacks cbs;
    cbs.onAvSetup = [&](ChannelId c, uint8_t) {
        if (c == ChannelId::Video) setupSeen = true;
    };
    cbs.onAvStart = [&](ChannelId c, bool started) {
        if (c == ChannelId::Video) (started ? startSeen : stopSeen) = true;
    };
    cbs.onAvMedia = [&](ChannelId c, bool ts, uint64_t pts, const uint8_t* d, size_t n) {
        if (c == ChannelId::Video) {
            mediaTs = ts;
            mediaPts = pts;
            mediaSeen.assign(d, d + n);
        }
    };
    AaConnection conn(msger, crypto, cbs);
    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::Video);
    hu.clearSent();

    // SETUP_REQUEST{config_index=0} -> HU replies SETUP_RESPONSE + onAvSetup.
    std::vector<uint8_t> setup;
    pb::Writer(setup).uint32(1, 0);
    phoneSend(hu, ChannelId::Video, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::SetupRequest), setup);
    REQUIRE(conn.poll(0));
    CHECK(setupSeen);
    CHECK(huSent(hu, ChannelId::Video, static_cast<uint16_t>(AvMsgId::SetupResponse)));

    // START_INDICATION{session=7} -> onAvStart(started).
    std::vector<uint8_t> start;
    pb::Writer(start).int32(1, 7);
    phoneSend(hu, ChannelId::Video, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::StartIndication), start);
    REQUIRE(conn.poll(0));
    CHECK(startSeen);

    // MEDIA_WITH_TIMESTAMP: 8-byte PTS then a fake NAL. HU must ack + deliver the
    // stripped bitstream, and the ack must echo the session id from START.
    hu.clearSent();
    const uint64_t pts = 0x0102030405060708ull;
    std::vector<uint8_t> media;
    for (int i = 7; i >= 0; --i) media.push_back(static_cast<uint8_t>((pts >> (i * 8)) & 0xFF));
    const uint8_t nal[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42};  // SPS start
    media.insert(media.end(), nal, nal + sizeof(nal));
    phoneSend(hu, ChannelId::Video, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::MediaWithTimestamp), media);
    REQUIRE(conn.poll(0));
    CHECK(mediaTs);
    CHECK_EQ(mediaPts, pts);
    REQUIRE(mediaSeen.size() == sizeof(nal));
    CHECK(mediaSeen[4] == 0x67);
    // Ack emitted, carrying session=7.
    std::vector<uint8_t> ack = lastHuBody(hu, ChannelId::Video,
                                          static_cast<uint16_t>(AvMsgId::MediaAck));
    REQUIRE(!ack.empty());
    {
        pb::Reader r(ack);
        uint32_t field;
        uint8_t wire;
        int32_t session = -1;
        while (!r.eof()) {
            REQUIRE(r.tag(field, wire));
            if (field == 1) {
                uint64_t v;
                REQUIRE(r.varint(v));
                session = static_cast<int32_t>(v);
            } else {
                REQUIRE(r.skip(wire));
            }
        }
        CHECK_EQ(session, 7);
    }

    // STOP_INDICATION -> onAvStart(false).
    phoneSend(hu, ChannelId::Video, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::StopIndication), {});
    REQUIRE(conn.poll(0));
    CHECK(stopSeen);
}

TEST(AaChannels, MediaIndicationNoTimestamp) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    bool ts = true;
    std::vector<uint8_t> data;
    carsh::aa::AaConnectionCallbacks cbs;
    cbs.onAvMedia = [&](ChannelId c, bool t, uint64_t, const uint8_t* d, size_t n) {
        if (c == ChannelId::MediaAudio) {
            ts = t;
            data.assign(d, d + n);
        }
    };
    AaConnection conn(msger, crypto, cbs);
    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::MediaAudio);

    const std::vector<uint8_t> pcm = {1, 2, 3, 4, 5, 6};
    phoneSend(hu, ChannelId::MediaAudio, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::MediaIndication), pcm);
    REQUIRE(conn.poll(0));
    CHECK(!ts);
    CHECK(data == pcm);
    CHECK(huSent(hu, ChannelId::MediaAudio, static_cast<uint16_t>(AvMsgId::MediaAck)));
}

TEST(AaChannels, InputBindingAndOutboundTouch) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    AaConnection conn(msger, crypto, {});
    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::Input);

    // BINDING_REQUEST -> BINDING_RESPONSE.
    phoneSend(hu, ChannelId::Input, EncryptionType::Encrypted,
              static_cast<uint16_t>(InputMsgId::BindingRequest), {0x08, 0x03});
    REQUIRE(conn.poll(0));
    CHECK(huSent(hu, ChannelId::Input, static_cast<uint16_t>(InputMsgId::BindingResponse)));

    // Outbound touch -> INPUT_EVENT_INDICATION whose touch_location decodes to x/y.
    hu.clearSent();
    REQUIRE(conn.sendTouch(TouchAction::Press, 696, 200, 12345));
    std::vector<uint8_t> body =
        lastHuBody(hu, ChannelId::Input,
                   static_cast<uint16_t>(InputMsgId::InputEventIndication));
    REQUIRE(!body.empty());
    // Walk to touch_event[2] -> touch_location[1] -> {x[1], y[2]}.
    pb::Reader r(body);
    uint32_t field;
    uint8_t wire;
    uint32_t gotX = 0, gotY = 0;
    bool foundLoc = false;
    while (!r.eof()) {
        REQUIRE(r.tag(field, wire));
        if (field == 2 && wire == 2) {  // touch_event
            pb::Reader te(nullptr, 0);
            REQUIRE(r.submessage(te));
            uint32_t f2;
            uint8_t w2;
            while (!te.eof()) {
                REQUIRE(te.tag(f2, w2));
                if (f2 == 1 && w2 == 2) {  // touch_location
                    pb::Reader loc(nullptr, 0);
                    REQUIRE(te.submessage(loc));
                    uint32_t f3;
                    uint8_t w3;
                    while (!loc.eof()) {
                        REQUIRE(loc.tag(f3, w3));
                        if (f3 == 1) {
                            REQUIRE(loc.u32(gotX));
                        } else if (f3 == 2) {
                            REQUIRE(loc.u32(gotY));
                        } else {
                            REQUIRE(loc.skip(w3));
                        }
                    }
                    foundLoc = true;
                } else {
                    REQUIRE(te.skip(w2));
                }
            }
        } else {
            REQUIRE(r.skip(wire));
        }
    }
    CHECK(foundLoc);
    CHECK_EQ(gotX, 696u);
    CHECK_EQ(gotY, 200u);

    // Touch before the input channel is open must no-op (false).
    AaConnection fresh(msger, crypto, {});
    CHECK(!fresh.sendTouch(TouchAction::Press, 1, 1, 1));
}

TEST(AaChannels, SensorStartGetsResponseAndEvent) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    AaConnection conn(msger, crypto, {});
    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::Sensor);
    hu.clearSent();

    // SENSOR_START_REQUEST{sensor_type=13 DRIVING_STATUS} -> response + an event.
    std::vector<uint8_t> req;
    pb::Writer(req).uint32(1, 13);
    phoneSend(hu, ChannelId::Sensor, EncryptionType::Encrypted,
              static_cast<uint16_t>(SensorMsgId::SensorStartRequest), req);
    REQUIRE(conn.poll(0));
    CHECK(huSent(hu, ChannelId::Sensor,
                 static_cast<uint16_t>(SensorMsgId::SensorStartResponse)));
    std::vector<uint8_t> evt = lastHuBody(
        hu, ChannelId::Sensor, static_cast<uint16_t>(SensorMsgId::SensorEventIndication));
    REQUIRE(!evt.empty());
    // The event carries a driving_status sub-message at field 13.
    pb::Reader r(evt);
    uint32_t field;
    uint8_t wire;
    bool hasDriving = false;
    while (!r.eof()) {
        REQUIRE(r.tag(field, wire));
        if (field == 13 && wire == 2) {
            hasDriving = true;
            pb::Reader sub(nullptr, 0);
            REQUIRE(r.submessage(sub));
        } else {
            REQUIRE(r.skip(wire));
        }
    }
    CHECK(hasDriving);
}

TEST(AaChannels, VideoFocusRequestAndOutboundIndication) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    VideoFocusMode requested = VideoFocusMode::Unfocused;
    bool sawRequest = false;
    carsh::aa::AaConnectionCallbacks cbs;
    cbs.onVideoFocusRequest = [&](VideoFocusMode m) {
        sawRequest = true;
        requested = m;
    };
    AaConnection conn(msger, crypto, cbs);
    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::Video);

    // Phone VIDEO_FOCUS_REQUEST{focus_mode=1 FOCUSED}.
    std::vector<uint8_t> req;
    pb::Writer(req).uint32(2, static_cast<uint32_t>(VideoFocusMode::Focused));
    phoneSend(hu, ChannelId::Video, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::VideoFocusRequest), req);
    REQUIRE(conn.poll(0));
    CHECK(sawRequest);
    CHECK(requested == VideoFocusMode::Focused);

    // HU grants focus -> VIDEO_FOCUS_INDICATION on the video channel.
    hu.clearSent();
    REQUIRE(conn.sendVideoFocus(VideoFocusMode::Focused));
    CHECK(huSent(hu, ChannelId::Video,
                 static_cast<uint16_t>(AvMsgId::VideoFocusIndication)));
}
