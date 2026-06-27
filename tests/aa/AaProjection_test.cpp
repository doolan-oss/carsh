// AaProjection wiring tests (AA_PLAN rung 6): drive a scripted phone to Running,
// then assert the full projection data flow with fakes — video media decodes and
// reaches the showFrame hook, audio media reaches the playAudio hook, channel-open
// flips connected, outbound touch/focus emit the right input/video messages, and a
// phone shutdown tears the projection down. The device kernel supplies the real
// hooks + CircleH264Decoder; this proves the glue host-side.
#include "aa/AaProjection.h"

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
    std::vector<uint8_t> openReq;
    pb::Writer(openReq).uint32(2, static_cast<uint32_t>(ch));
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, 0x0007, openReq);
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

    // Focus -> VIDEO_FOCUS_INDICATION on the video channel.
    hu.clearSent();
    CHECK(proj.setFocus(true));
    CHECK(huSent(hu, ChannelId::Video,
                 static_cast<uint16_t>(AvMsgId::VideoFocusIndication)));

    // Phone shutdown -> projection torn down (placeholder + disconnected).
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, 0x000F, {});
    REQUIRE(conn.poll(0));
    CHECK(!connected);
    CHECK(!haveFrame);
}

TEST(AaProjection, OutboundNoOpsWithoutConnection) {
    AaProjection proj(AaProjection::Hooks{});
    // No connection bound yet -> touch/focus are safe no-ops returning false.
    CHECK(!proj.forwardTouch(10, 10, TouchAction::Press, 0));
    CHECK(!proj.setFocus(true));
}
