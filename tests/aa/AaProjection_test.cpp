// AaProjection wiring tests (AA_PLAN rung 6): drive a scripted phone to Running,
// then assert the full projection data flow with fakes — video media decodes and
// reaches the showFrame hook, audio media reaches the playAudio hook, channel-open
// flips connected, outbound touch/focus emit the right input/video messages, and a
// phone shutdown tears the projection down. The device kernel supplies the real
// hooks + CircleH264Decoder; this proves the glue host-side.
#include "aa/AaProjection.h"

#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

#include "Test.h"
#include "aa/AaConnection.h"
#include "aa/AaTypes.h"
#include "aa/FakeAaCrypto.h"
#include "aa/FakeAaTransport.h"
#include "aa/FakeH264Decoder.h"
#include "aa/Pb.h"

using namespace carsh;
using carsh::aa::AaConnection;
using carsh::aa::AaMessage;
using carsh::aa::AaMessenger;
using carsh::aa::AaProjection;
using carsh::aa::AaState;
using carsh::aa::AvMsgId;
using carsh::aa::ChannelId;
using carsh::aa::EncryptionType;
using carsh::aa::InputMsgId;
using carsh::aa::MediaKey;
using carsh::aa::MessageType;
using carsh::aa::TouchAction;
using carsh::host::FakeAaCrypto;
using carsh::host::FakeAaTransport;
using carsh::host::FakeH264Decoder;
namespace pb = carsh::aa::pb;

namespace {

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

bool huSent(const FakeAaTransport& hu, ChannelId ch, uint16_t msgId) {
    FakeAaTransport t;
    t.pushRx(hu.sent());
    AaMessenger m(t);
    AaMessage msg;
    while (m.receive(msg, 0)) {
        uint16_t id = msg.messageId;
        if (msg.encryption == EncryptionType::Encrypted && msg.payload.size() >= 2)
            id = static_cast<uint16_t>((msg.payload[0] << 8) | msg.payload[1]);
        if (msg.channel == ch && id == msgId) return true;
    }
    return false;
}

void driveToRunning(AaConnection& conn, FakeAaTransport& hu) {
    conn.start();
    phoneSend(hu, ChannelId::Control, EncryptionType::Plain, 0x0002,
              {0x00, 0x01, 0x00, 0x07, 0x00, 0x00});
    REQUIRE(conn.poll(0));
    phoneSend(hu, ChannelId::Control, EncryptionType::Plain, 0x0003, {0x01});
    REQUIRE(conn.poll(0));
    std::vector<uint8_t> sdReq;
    pb::Writer(sdReq).string(4, "Pixel");
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, 0x0005, sdReq);
    REQUIRE(conn.poll(0));
    REQUIRE(conn.state() == AaState::Running);
}

void openChannel(AaConnection& conn, FakeAaTransport& hu, ChannelId ch) {
    // The phone opens a channel by sending CHANNEL_OPEN_REQUEST on that channel itself.
    std::vector<uint8_t> openReq;
    pb::Writer(openReq).uint32(2, static_cast<uint32_t>(ch));
    phoneSend(hu, ch, EncryptionType::Encrypted, 0x0007, openReq);
    REQUIRE(conn.poll(0));
    REQUIRE(conn.channelOpen(ch));
}

}  // namespace

TEST(AaProjection, EndToEndWiring) {
    bool haveFrame = false;
    int fw = 0, fh = 0;
    AaProjection::Hooks hooks;
    hooks.showFrame = [&](const uint8_t* rgb, int w, int h) {
        haveFrame = (rgb != nullptr && w > 0 && h > 0);
        fw = w;
        fh = h;
    };
    ChannelId audioCh = ChannelId::None;
    std::vector<uint8_t> audioData;
    hooks.playAudio = [&](ChannelId ch, const uint8_t* p, size_t n) {
        audioCh = ch;
        audioData.assign(p, p + n);
    };
    bool connected = false;
    hooks.setConnected = [&](bool v) { connected = v; };

    AaProjection proj(hooks);
    FakeH264Decoder dec(&proj);
    proj.setDecoder(&dec);

    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);
    AaConnection conn(msger, crypto, proj.callbacks());
    proj.setConnection(&conn);

    driveToRunning(conn, hu);
    openChannel(conn, hu, ChannelId::Video);
    CHECK(connected);
    CHECK(proj.connected());
    openChannel(conn, hu, ChannelId::MediaAudio);
    openChannel(conn, hu, ChannelId::Input);

    // Video media -> decoder.feed -> onFrame -> showFrame.
    const std::vector<uint8_t> nal = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
    phoneSend(hu, ChannelId::Video, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::MediaIndication), nal);
    REQUIRE(conn.poll(0));
    CHECK(haveFrame);
    CHECK_EQ(fw, 8);
    CHECK_EQ(fh, 8);
    CHECK_EQ(dec.frames, 1);
    CHECK(dec.lastFed == nal);

    // Audio media -> playAudio hook (not the decoder).
    const std::vector<uint8_t> pcm = {9, 8, 7, 6};
    phoneSend(hu, ChannelId::MediaAudio, EncryptionType::Encrypted,
              static_cast<uint16_t>(AvMsgId::MediaIndication), pcm);
    REQUIRE(conn.poll(0));
    CHECK(audioCh == ChannelId::MediaAudio);
    CHECK(audioData == pcm);
    CHECK_EQ(dec.frames, 1);  // audio did not hit the video decoder

    // Outbound touch -> INPUT_EVENT_INDICATION on the input channel.
    hu.clearSent();
    CHECK(proj.forwardTouch(696, 200, TouchAction::Press, 42));
    CHECK(huSent(hu, ChannelId::Input,
                 static_cast<uint16_t>(InputMsgId::InputEventIndication)));
    // Off-video touches (negative source coords) are dropped.
    CHECK(!proj.forwardTouch(-1, 5, TouchAction::Press, 42));

    // Focus -> VIDEO_FOCUS_REQUEST on the video channel (HU drives projection).
    hu.clearSent();
    CHECK(proj.setFocus(true));
    CHECK(huSent(hu, ChannelId::Video,
                 static_cast<uint16_t>(AvMsgId::VideoFocusIndication)));

    // Phone shutdown -> acknowledged, then projection torn down (placeholder +
    // disconnected) and the connection unbound.
    hu.clearSent();
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, 0x000F, {});
    REQUIRE(conn.poll(0));
    CHECK(huSent(hu, ChannelId::Control, 0x0010));  // SHUTDOWN_RESPONSE
    CHECK(!connected);
    CHECK(!proj.connected());
    CHECK(!haveFrame);
    CHECK(!proj.forwardTouch(10, 10, TouchAction::Press, 43));
}

TEST(AaProjection, OutboundNoOpsWithoutConnection) {
    AaProjection proj(AaProjection::Hooks{});
    // No connection bound yet -> touch/focus are safe no-ops returning false.
    CHECK(!proj.forwardTouch(10, 10, TouchAction::Press, 0));
    CHECK(!proj.setFocus(true));
}

namespace {

// A projection wired to a Running connection with video + input open, recording
// every hook call. Mirrors the kernel wiring (Hooks + callbacks() + setConnection).
struct Rig {
    FakeAaTransport hu;
    AaMessenger msger{hu};
    FakeAaCrypto crypto{2};
    int connectedTrue = 0, connectedFalse = 0, framesShown = 0, framesCleared = 0;
    std::function<void(bool)> onSetConnected;  // extra behaviour (e.g. a tab switch)
    AaProjection proj{hooks()};
    FakeH264Decoder dec{&proj};
    AaConnection conn{msger, crypto, proj.callbacks()};

    AaProjection::Hooks hooks() {
        AaProjection::Hooks h;
        h.showFrame = [this](const uint8_t* rgb, int, int) {
            (rgb != nullptr ? framesShown : framesCleared)++;
        };
        h.setConnected = [this](bool v) {
            (v ? connectedTrue : connectedFalse)++;
            if (onSetConnected) onSetConnected(v);
        };
        return h;
    }
    void up() {
        proj.setDecoder(&dec);
        proj.setConnection(&conn);
        driveToRunning(conn, hu);
        openChannel(conn, hu, ChannelId::Video);
        openChannel(conn, hu, ChannelId::Input);
    }
    void videoFrame() {
        phoneSend(hu, ChannelId::Video, EncryptionType::Encrypted,
                  static_cast<uint16_t>(AvMsgId::MediaIndication), {0, 0, 0, 1, 0x65});
        REQUIRE(conn.poll(0));
    }
};

// Decode one INPUT_EVENT_INDICATION body's touch_event: (touch_action, x, y).
struct Touch {
    uint32_t action = 99, x = 0, y = 0;
};
Touch decodeTouch(const std::vector<uint8_t>& body) {
    Touch t;
    pb::Reader r(body);
    uint32_t f;
    uint8_t w;
    while (!r.eof()) {
        REQUIRE(r.tag(f, w));
        if (f == 3 && w == 2) {  // touch_event
            pb::Reader te(nullptr, 0);
            REQUIRE(r.submessage(te));
            while (!te.eof()) {
                REQUIRE(te.tag(f, w));
                if (f == 1 && w == 2) {  // touch_location
                    pb::Reader loc(nullptr, 0);
                    REQUIRE(te.submessage(loc));
                    while (!loc.eof()) {
                        REQUIRE(loc.tag(f, w));
                        if (f == 1) REQUIRE(loc.u32(t.x));
                        else if (f == 2) REQUIRE(loc.u32(t.y));
                        else REQUIRE(loc.skip(w));
                    }
                } else if (f == 3 && w == 0) {
                    REQUIRE(te.u32(t.action));
                } else {
                    REQUIRE(te.skip(w));
                }
            }
        } else {
            REQUIRE(r.skip(w));
        }
    }
    return t;
}

}  // namespace

// Dongle unplug / re-cycle (the owner calls disconnect()): the projection resets the
// UI exactly once — setConnected(false) + a cleared frame, so the app drops the
// frozen frame and re-arms auto-show — and unbinds, so touch/focus/keys no-op.
TEST(AaProjection, DisconnectResetsOnceAndUnbinds) {
    Rig rig;
    rig.up();
    rig.videoFrame();
    CHECK(rig.proj.connected());
    CHECK_EQ(rig.framesShown, 1);

    rig.proj.disconnect();
    CHECK(!rig.proj.connected());
    CHECK_EQ(rig.connectedFalse, 1);
    CHECK_EQ(rig.framesCleared, 1);
    CHECK(!rig.proj.forwardTouch(10, 10, TouchAction::Press, 1));
    CHECK(!rig.proj.setFocus(true));
    CHECK(!rig.proj.sendMediaKey(carsh::aa::MediaKey::PlayPause, 1));

    // Idempotent: the driver may call it again (and after onShutdown).
    rig.proj.disconnect();
    CHECK_EQ(rig.connectedFalse, 1);
    CHECK_EQ(rig.framesCleared, 1);

    // A new session re-binds and comes back up.
    FakeAaTransport hu2;
    AaMessenger msger2(hu2);
    FakeAaCrypto crypto2(2);
    AaConnection conn2(msger2, crypto2, rig.proj.callbacks());
    rig.proj.setConnection(&conn2);
    driveToRunning(conn2, hu2);
    openChannel(conn2, hu2, ChannelId::Input);
    CHECK(rig.proj.connected());
    CHECK(rig.proj.forwardTouch(10, 10, TouchAction::Press, 2));
}

// A disconnect before any session came up (e.g. unplugged mid-handshake) must not
// fire the reset hooks — the app would otherwise yank the user off their tab.
TEST(AaProjection, DisconnectWithoutSessionFiresNoHooks) {
    Rig rig;
    rig.proj.setConnection(&rig.conn);
    rig.conn.start();
    rig.proj.disconnect();
    CHECK_EQ(rig.connectedFalse, 0);
    CHECK_EQ(rig.framesCleared, 0);
    CHECK(!rig.proj.setFocus(true));
}

// After the phone's SHUTDOWN_REQUEST, a frame that finishes decoding late (AUs still
// queued for the decode core) must not reach the screen — it would re-freeze the AA
// tab and re-trigger the app's auto-show on a dead session.
TEST(AaProjection, LateFrameAfterShutdownIsDropped) {
    Rig rig;
    rig.up();
    rig.videoFrame();
    phoneSend(rig.hu, ChannelId::Control, EncryptionType::Encrypted, 0x000F, {});
    REQUIRE(rig.conn.poll(0));
    CHECK_EQ(rig.framesShown, 1);
    CHECK_EQ(rig.framesCleared, 1);

    uint8_t px[3] = {1, 2, 3};
    carsh::aa::DecodedFrame late;
    late.width = 1;
    late.height = 1;
    late.pixels = px;
    late.pixelsLen = sizeof(px);
    rig.proj.onFrame(late);  // what the decode core would deliver
    CHECK_EQ(rig.framesShown, 1);
    // ...so the owner's later disconnect() has nothing left to clear.
    rig.proj.disconnect();
    CHECK_EQ(rig.connectedFalse, 1);
    CHECK_EQ(rig.framesCleared, 1);
}

// The app's disconnect hook switches tabs, which drops video focus (setFocus(false)).
// That must NOT go out to the phone that just shut down.
TEST(AaProjection, ShutdownSendsNothingAfterTheResponse) {
    Rig rig;
    bool focusSent = true;
    rig.onSetConnected = [&](bool v) {
        if (!v) focusSent = rig.proj.setFocus(false);
    };
    rig.up();
    rig.hu.clearSent();
    phoneSend(rig.hu, ChannelId::Control, EncryptionType::Encrypted, 0x000F, {});
    REQUIRE(rig.conn.poll(0));
    CHECK(!focusSent);
    FakeAaTransport t;
    t.pushRx(rig.hu.sent());
    AaMessenger m(t);
    AaMessage msg;
    int sent = 0;
    while (m.receive(msg, 0)) ++sent;
    CHECK_EQ(sent, 1);  // only the SHUTDOWN_RESPONSE
}

// A swipe streamed as PRESS, DRAG..., RELEASE reaches the phone as those actions in
// order (touch_action = 0, 2, 2, 1), each at its own point.
TEST(AaProjection, DragStreamsEndToEnd) {
    Rig rig;
    rig.up();
    rig.hu.clearSent();
    CHECK(rig.proj.forwardTouch(400, 380, TouchAction::Press, 1000));
    CHECK(rig.proj.forwardTouch(400, 300, TouchAction::Drag, 17000));
    CHECK(rig.proj.forwardTouch(400, 200, TouchAction::Drag, 33000));
    CHECK(rig.proj.forwardTouch(400, 100, TouchAction::Release, 50000));

    std::vector<Touch> got;
    FakeAaTransport t;
    t.pushRx(rig.hu.sent());
    AaMessenger m(t);
    AaMessage msg;
    while (m.receive(msg, 0)) {
        REQUIRE(msg.payload.size() >= 2);
        const uint16_t id = static_cast<uint16_t>((msg.payload[0] << 8) | msg.payload[1]);
        if (msg.channel != ChannelId::Input ||
            id != static_cast<uint16_t>(InputMsgId::InputEventIndication))
            continue;
        got.push_back(decodeTouch(std::vector<uint8_t>(msg.payload.begin() + 2,
                                                       msg.payload.end())));
    }
    REQUIRE(got.size() == 4);
    const uint32_t actions[] = {0, 2, 2, 1};
    const uint32_t ys[] = {380, 300, 200, 100};
    for (size_t i = 0; i < 4; ++i) {
        CHECK_EQ(got[i].action, actions[i]);
        CHECK_EQ(got[i].x, 400u);
        CHECK_EQ(got[i].y, ys[i]);
    }
}

// Review T10: a media key must carry the caller's 64-bit µs stamp. sendMediaKey has no
// default timestamp any more, so a call that forgets it no longer compiles — the
// zero-stamped keys can't come back silently.
namespace {
template <class P, class = void>
struct MediaKeyWithoutStamp : std::false_type {};
template <class P>
struct MediaKeyWithoutStamp<
    P, std::void_t<decltype(std::declval<P&>().sendMediaKey(MediaKey::PlayPause))>>
    : std::true_type {};
}  // namespace
static_assert(!MediaKeyWithoutStamp<AaProjection>::value,
              "AaProjection::sendMediaKey must require a timestamp");

TEST(AaProjection, MediaKeyCarriesTheCallersStamp) {
    Rig rig;
    rig.up();
    rig.hu.clearSent();
    const uint64_t stamp = 0x100000007ULL;  // past 2^32 µs (~71.6 min of uptime)
    REQUIRE(rig.proj.sendMediaKey(MediaKey::Next, stamp));

    std::vector<uint64_t> stamps;
    FakeAaTransport t;
    t.pushRx(rig.hu.sent());
    AaMessenger m(t);
    AaMessage msg;
    while (m.receive(msg, 0)) {
        REQUIRE(msg.payload.size() >= 2);
        const uint16_t id = static_cast<uint16_t>((msg.payload[0] << 8) | msg.payload[1]);
        if (msg.channel != ChannelId::Input ||
            id != static_cast<uint16_t>(InputMsgId::InputEventIndication))
            continue;
        pb::Reader r(msg.payload.data() + 2, msg.payload.size() - 2);
        uint32_t f;
        uint8_t w;
        uint64_t ts = 0;
        while (!r.eof()) {
            REQUIRE(r.tag(f, w));
            if (f == 1 && w == 0) REQUIRE(r.varint(ts));  // InputEventIndication.timestamp
            else REQUIRE(r.skip(w));
        }
        stamps.push_back(ts);
    }
    REQUIRE(stamps.size() == 2);  // button down + up
    CHECK_EQ(stamps[0], stamp);
    CHECK_EQ(stamps[1], stamp);
}
