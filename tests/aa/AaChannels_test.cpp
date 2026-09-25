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
using carsh::aa::MediaKey;
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
    // The phone sends CHANNEL_OPEN_REQUEST on the target channel itself (the frame
    // channel is authoritative); the payload channel_id is advisory.
    std::vector<uint8_t> openReq;
    pb::Writer(openReq).uint32(2, static_cast<uint32_t>(ch));
    phoneSend(hu, ch, EncryptionType::Encrypted, kChannelOpenReq, openReq);
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

// ---- INPUT_EVENT_INDICATION golden bytes -------------------------------------------
// Derived by hand from aasdk's aasdk_proto/InputEventIndicationMessage.proto (and the
// TouchEvent / TouchLocation / ButtonEvents / ButtonEvent protos it imports) — NOT
// produced by carsh's pb::Writer — and cross-checked by parsing them with protobuf
// against both the aasdk (proto3) schema and headunit's proto2 hu.proto, where they
// are IsInitialized() (pointer_id, touch_action, meta, long_press are REQUIRED there):
//   InputEventIndication { timestamp = 1 (uint64), disp_channel = 2 (int32),
//                          touch_event = 3 (TouchEvent), button_event = 4 (ButtonEvents) }
//   TouchEvent    { touch_location = 1 (repeated TouchLocation), action_index = 2,
//                   touch_action = 3 (PRESS = 0, RELEASE = 1, DRAG = 2) }
//   TouchLocation { x = 1, y = 2, pointer_id = 3 }
//   ButtonEvents  { button_events = 1 (repeated ButtonEvent) }
//   ButtonEvent   { scan_code = 1, is_pressed = 2, meta = 3, long_press = 4 }
// Tag bytes: 08 = field 1 varint, 10 = field 2 varint, 18 = field 3 varint,
// 20 = field 4 varint, 0a = field 1 length-delimited, 1a = field 3 LD, 22 = field 4 LD.

namespace {

// touch at (696,200), ts = 12345 (varint b9 60), action byte last.
//   08 b9 60                  timestamp = 12345
//   1a 0e                     touch_event, 14 bytes:
//     0a 08                     touch_location, 8 bytes:
//       08 b8 05                  x = 696
//       10 c8 01                  y = 200
//       18 00                     pointer_id = 0
//     10 00                     action_index = 0
//     18 aa                     touch_action = aa
std::vector<uint8_t> goldenTouch696x200(uint8_t action) {
    return {0x08, 0xb9, 0x60, 0x1a, 0x0e, 0x0a, 0x08, 0x08, 0xb8, 0x05, 0x10,
            0xc8, 0x01, 0x18, 0x00, 0x10, 0x00, 0x18, action};
}

// All INPUT_EVENT_INDICATION bodies the HU sent, in order.
std::vector<std::vector<uint8_t>> inputEvents(const FakeAaTransport& hu) {
    std::vector<std::vector<uint8_t>> out;
    for (const auto& m : parseHuSends(hu)) {
        if (m.channel == ChannelId::Input &&
            m.messageId == static_cast<uint16_t>(InputMsgId::InputEventIndication))
            out.push_back(m.payload);
    }
    return out;
}

}  // namespace

TEST(AaChannels, InputBindingAndOutboundTouch) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    AaConnection conn(msger, crypto, {});

    // Touch before the input channel is open must no-op (false) and send nothing.
    CHECK(!conn.sendTouch(TouchAction::Press, 1, 1, 1));

    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::Input);

    // BINDING_REQUEST -> BINDING_RESPONSE.
    phoneSend(hu, ChannelId::Input, EncryptionType::Encrypted,
              static_cast<uint16_t>(InputMsgId::BindingRequest), {0x08, 0x03});
    REQUIRE(conn.poll(0));
    CHECK(huSent(hu, ChannelId::Input, static_cast<uint16_t>(InputMsgId::BindingResponse)));

    // PRESS / RELEASE: touch_event in field 3, touch_action in TouchEvent field 3 —
    // byte-exact against the aasdk-derived golden.
    hu.clearSent();
    REQUIRE(conn.sendTouch(TouchAction::Press, 696, 200, 12345));
    REQUIRE(conn.sendTouch(TouchAction::Release, 696, 200, 12345));
    auto ev = inputEvents(hu);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0] == goldenTouch696x200(0x00));  // PRESS
    CHECK(ev[1] == goldenTouch696x200(0x01));  // RELEASE
}

// DRAG at (400,240) with a 64-bit µs timestamp above 2^32 (0x0123456789ab): it must
// go out whole — no 32-bit truncation — and carry touch_action = DRAG (2).
TEST(AaChannels, TouchDragWith64BitTimestamp) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    AaConnection conn(msger, crypto, {});
    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::Input);

    hu.clearSent();
    REQUIRE(conn.sendTouch(TouchAction::Drag, 400, 240, 0x0123456789abull));
    const std::vector<uint8_t> golden = {
        0x08, 0xab, 0x93, 0x9e, 0xab, 0xb4, 0x24,  // timestamp = 0x0123456789ab
        0x1a, 0x0e,                                // touch_event, 14 bytes
        0x0a, 0x08,                                //   touch_location, 8 bytes
        0x08, 0x90, 0x03,                          //     x = 400
        0x10, 0xf0, 0x01,                          //     y = 240
        0x18, 0x00,                                //     pointer_id = 0
        0x10, 0x00,                                //   action_index = 0
        0x18, 0x02,                                //   touch_action = DRAG
    };
    auto ev = inputEvents(hu);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == golden);
}

TEST(AaChannels, MediaKeySendsPressThenReleaseButtonEvent) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    AaConnection conn(msger, crypto, {});

    // Media key before the input channel is open must no-op (false).
    CHECK(!conn.sendMediaKey(MediaKey::Next, 1));

    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::Input);

    hu.clearSent();
    REQUIRE(conn.sendMediaKey(MediaKey::PlayPause, 7));

    // A media key = two INPUT_EVENT_INDICATION frames, button down then up, with
    // button_event in field 4 (NOT 3, which is touch_event on the phone):
    //   08 07                 timestamp = 7
    //   22 0a                 button_event (ButtonEvents), 10 bytes:
    //     0a 08                 button_events[0] (ButtonEvent), 8 bytes:
    //       08 55                 scan_code = 0x55 KEYCODE_MEDIA_PLAY_PAUSE
    //       10 01 / 10 00         is_pressed = true / false
    //       18 00                 meta = 0
    //       20 00                 long_press = false
    const std::vector<uint8_t> down = {0x08, 0x07, 0x22, 0x0a, 0x0a, 0x08, 0x08,
                                       0x55, 0x10, 0x01, 0x18, 0x00, 0x20, 0x00};
    const std::vector<uint8_t> up = {0x08, 0x07, 0x22, 0x0a, 0x0a, 0x08, 0x08,
                                     0x55, 0x10, 0x00, 0x18, 0x00, 0x20, 0x00};
    auto ev = inputEvents(hu);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0] == down);
    CHECK(ev[1] == up);
}

// The advertised AV-input (microphone) channel must answer the phone's
// SETUP_REQUEST and AV_INPUT_OPEN_REQUEST (aasdk AVInputServiceChannel), or a voice
// session hangs waiting on the open. Both replies are encrypted SPECIFIC on channel 7.
TEST(AaChannels, AvInputSetupAndOpenAnswered) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    std::vector<int> micEvents;  // 1 = open, 0 = close
    carsh::aa::AaConnectionCallbacks cbs;
    cbs.onMicRequest = [&](bool open) { micEvents.push_back(open ? 1 : 0); };
    AaConnection conn(msger, crypto, cbs);
    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::AvInput);

    // SETUP_REQUEST{config_index = 0} -> AVChannelSetupResponse{media_status = OK(2),
    // max_unacked = 1, configs = [0]} = 08 02 10 01 18 00 (aasdk
    // AVChannelSetupResponseMessage.proto; OK = 2 per AVChannelSetupStatusEnum).
    hu.clearSent();
    std::vector<uint8_t> setup;
    pb::Writer(setup).uint32(1, 0);
    phoneSend(hu, ChannelId::AvInput, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::SetupRequest), setup);
    REQUIRE(conn.poll(0));
    const std::vector<uint8_t> setupGolden = {0x08, 0x02, 0x10, 0x01, 0x18, 0x00};
    CHECK(lastHuBody(hu, ChannelId::AvInput,
                     static_cast<uint16_t>(AvMsgId::SetupResponse)) == setupGolden);

    // AV_INPUT_OPEN_REQUEST{open = true} -> AVInputOpenResponse{session = 0,
    // value = 0 (OK)} = 08 00 10 00 (both fields required in aasdk's proto2 message).
    hu.clearSent();
    std::vector<uint8_t> openReq;
    pb::Writer(openReq).boolean(1, true);
    phoneSend(hu, ChannelId::AvInput, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::AvInputOpenRequest), openReq);
    REQUIRE(conn.poll(0));
    const std::vector<uint8_t> openGolden = {0x08, 0x00, 0x10, 0x00};
    CHECK(lastHuBody(hu, ChannelId::AvInput,
                     static_cast<uint16_t>(AvMsgId::AvInputOpenResponse)) == openGolden);
    bool specificEncrypted = false;
    for (const auto& m : parseHuSends(hu)) {
        if (m.channel == ChannelId::AvInput &&
            m.messageId == static_cast<uint16_t>(AvMsgId::AvInputOpenResponse))
            specificEncrypted = m.encryption == EncryptionType::Encrypted &&
                                m.msgType == MessageType::Specific;
    }
    CHECK(specificEncrypted);

    // ...and the close is answered too.
    hu.clearSent();
    std::vector<uint8_t> closeReq;
    pb::Writer(closeReq).boolean(1, false);
    phoneSend(hu, ChannelId::AvInput, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::AvInputOpenRequest), closeReq);
    REQUIRE(conn.poll(0));
    CHECK(huSent(hu, ChannelId::AvInput, static_cast<uint16_t>(AvMsgId::AvInputOpenResponse)));
    REQUIRE(micEvents.size() == 2);
    CHECK_EQ(micEvents[0], 1);
    CHECK_EQ(micEvents[1], 0);
}

// PingRequest/PingResponse are { int64 timestamp = 1 }: the HU must echo the phone's
// timestamp unchanged — including values above 2^31 and negative int64s (10-byte
// varints) — with no extra fields.
TEST(AaChannels, PingEchoesInt64Timestamp) {
    using carsh::aa::ControlMsgId;
    const uint16_t kPingReq = static_cast<uint16_t>(ControlMsgId::PingRequest);
    const uint16_t kPingResp = static_cast<uint16_t>(ControlMsgId::PingResponse);

    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    AaConnection conn(msger, crypto, {});
    driveToRunning(conn, hu);

    // timestamp = 5,000,000,000 -> 08 80 e4 97 d0 12 (it used to come back as
    // int32 705,032,704 plus a bogus field 2).
    const std::vector<uint8_t> big = {0x08, 0x80, 0xe4, 0x97, 0xd0, 0x12};
    hu.clearSent();
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, kPingReq, big);
    REQUIRE(conn.poll(0));
    CHECK(lastHuBody(hu, ChannelId::Control, kPingResp) == big);

    // timestamp = -1 (int64) -> 08 ff ff ff ff ff ff ff ff ff 01. A trailing unknown
    // request field (aap_protobuf's bool bug_report = 2) is not echoed.
    const std::vector<uint8_t> neg = {0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff, 0x01};
    std::vector<uint8_t> negReq = neg;
    negReq.push_back(0x10);  // bug_report = false
    negReq.push_back(0x00);
    hu.clearSent();
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, kPingReq, negReq);
    REQUIRE(conn.poll(0));
    CHECK(lastHuBody(hu, ChannelId::Control, kPingResp) == neg);
}

// SHUTDOWN_REQUEST: the HU acknowledges with an (empty) encrypted SPECIFIC
// SHUTDOWN_RESPONSE on the control channel FIRST, then goes to Shutdown for good —
// onShutdown fires once, poll() stops, and app-driven sends (touch, keys, focus) no
// longer reach the dead session.
TEST(AaChannels, ShutdownRequestAcknowledgedThenShutdown) {
    using carsh::aa::AaShutdownReason;
    using carsh::aa::ControlMsgId;
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    int shutdowns = 0;
    carsh::aa::AaConnectionCallbacks cbs;
    cbs.onShutdown = [&] { ++shutdowns; };
    AaConnection conn(msger, crypto, cbs);
    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::Input);
    openChannel(conn, hu, ChannelId::Video);

    hu.clearSent();
    std::vector<uint8_t> req;
    pb::Writer(req).uint32(1, 1);  // reason = QUIT
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted,
              static_cast<uint16_t>(ControlMsgId::ShutdownRequest), req);
    REQUIRE(conn.poll(0));
    CHECK(conn.state() == AaState::Shutdown);
    CHECK(conn.shutdownReason() == AaShutdownReason::PhoneRequest);
    CHECK_EQ(shutdowns, 1);

    const auto sent = parseHuSends(hu);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].channel == ChannelId::Control);
    CHECK_EQ(int(sent[0].messageId), int(ControlMsgId::ShutdownResponse));
    CHECK(sent[0].encryption == EncryptionType::Encrypted);
    CHECK(sent[0].msgType == MessageType::Specific);
    CHECK(sent[0].payload.empty());  // ShutdownResponse has no fields

    // Terminal: nothing further is received or sent.
    hu.clearSent();
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted,
              static_cast<uint16_t>(ControlMsgId::ShutdownRequest), req);
    CHECK(!conn.poll(0));
    CHECK(!conn.sendTouch(TouchAction::Press, 10, 10, 1));
    CHECK(!conn.sendMediaKey(MediaKey::PlayPause, 1));
    CHECK(!conn.sendVideoFocus(VideoFocusMode::Unfocused));
    CHECK(!conn.channelOpen(ChannelId::Input));
    CHECK(hu.sent().empty());
    CHECK_EQ(shutdowns, 1);
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

    // HU-driven focus change -> VIDEO_FOCUS_INDICATION (0x8008) on the video channel.
    hu.clearSent();
    REQUIRE(conn.sendVideoFocus(VideoFocusMode::Focused));
    CHECK(huSent(hu, ChannelId::Video,
                 static_cast<uint16_t>(AvMsgId::VideoFocusIndication)));
}

// The phone sends AUDIO_FOCUS_REQUEST (and NAV_FOCUS_REQUEST) right after service
// discovery and blocks on the response; missing it tears the link down ~1s into
// Running (seen on the bench). Verify the HU grants both, with the request->state
// enum remap for audio focus.
TEST(AaChannels, AudioAndNavFocusGranted) {
    using carsh::aa::ControlMsgId;
    const uint16_t kAudioReq = static_cast<uint16_t>(ControlMsgId::AudioFocusRequest);
    const uint16_t kAudioResp = static_cast<uint16_t>(ControlMsgId::AudioFocusResponse);
    const uint16_t kNavReq = static_cast<uint16_t>(ControlMsgId::NavFocusRequest);
    const uint16_t kNavResp = static_cast<uint16_t>(ControlMsgId::NavFocusResponse);

    auto field1 = [](const std::vector<uint8_t>& body) -> uint32_t {
        pb::Reader r(body.data(), body.size());
        uint32_t field;
        uint8_t wire;
        uint32_t v = 0xFFFFFFFFu;
        while (!r.eof()) {
            if (!r.tag(field, wire)) break;
            if (field == 1 && wire == 0)
                r.u32(v);
            else if (!r.skip(wire))
                break;
        }
        return v;
    };

    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    AaConnection conn(msger, crypto, {});
    driveToRunning(conn, hu);

    // AUDIO_FOCUS_REQUEST{focus_type=1 GAIN} -> AUDIO_FOCUS_RESPONSE{state=1 GAIN}.
    hu.clearSent();
    std::vector<uint8_t> gain;
    pb::Writer(gain).uint32(1, 1);
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, kAudioReq, gain);
    REQUIRE(conn.poll(0));
    REQUIRE(huSent(hu, ChannelId::Control, kAudioResp));
    CHECK_EQ(static_cast<int>(field1(lastHuBody(hu, ChannelId::Control, kAudioResp))), 1);

    // RELEASE(4) must remap to LOSS(3), not echo to 4 (LOSS_TRANSIENT_CAN_DUCK).
    hu.clearSent();
    std::vector<uint8_t> release;
    pb::Writer(release).uint32(1, 4);
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, kAudioReq, release);
    REQUIRE(conn.poll(0));
    CHECK_EQ(static_cast<int>(field1(lastHuBody(hu, ChannelId::Control, kAudioResp))), 3);

    // NAV_FOCUS_REQUEST -> NAV_FOCUS_RESPONSE{focus_type=1 GAIN}.
    hu.clearSent();
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, kNavReq, {});
    REQUIRE(conn.poll(0));
    REQUIRE(huSent(hu, ChannelId::Control, kNavResp));
    CHECK_EQ(static_cast<int>(field1(lastHuBody(hu, ChannelId::Control, kNavResp))), 1);
}
