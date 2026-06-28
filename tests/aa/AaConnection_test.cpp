// AaConnection bring-up tests: drive the full head-unit handshake end to end with
// a scripted phone and FakeAaCrypto (identity TLS). Proves the state machine
// walks version exchange -> SSL handshake -> auth -> service discovery -> channel
// opens and emits the right control messages at each step — all host-side, the
// central claim of AA_PLAN Phase 1. The framing underneath is the same code
// validated byte-exact against the real DHU capture (AaMessenger_test).
#include "aa/AaConnection.h"

#include <vector>

#include "Test.h"
#include "aa/FakeAaCrypto.h"
#include "aa/FakeAaTransport.h"
#include "aa/Pb.h"

using namespace carsh;
using carsh::aa::AaConnection;
using carsh::aa::AaMessage;
using carsh::aa::AaMessenger;
using carsh::aa::AaState;
using carsh::aa::ChannelId;
using carsh::aa::EncryptionType;
using carsh::aa::MessageType;
using carsh::host::FakeAaCrypto;
using carsh::host::FakeAaTransport;
namespace pb = carsh::aa::pb;

namespace {

// Frame a phone->HU message and deliver it into the head unit's transport.
// Encrypted messages use identity crypto: the wire payload is [2B msgId][body].
void phoneSend(FakeAaTransport& hu, ChannelId ch, EncryptionType enc, MessageType mt,
               uint16_t msgId, const std::vector<uint8_t>& body) {
    FakeAaTransport t;
    AaMessenger m(t);
    if (enc == EncryptionType::Encrypted) {
        std::vector<uint8_t> plain;
        plain.push_back(static_cast<uint8_t>(msgId >> 8));
        plain.push_back(static_cast<uint8_t>(msgId & 0xFF));
        plain.insert(plain.end(), body.begin(), body.end());
        m.sendRaw(ch, EncryptionType::Encrypted, mt, plain.data(), plain.size());
    } else {
        m.send(ch, EncryptionType::Plain, mt, msgId, body.data(), body.size());
    }
    hu.pushRx(t.sent());
}

// Parse everything the head unit has sent so far into messages, exposing the
// encrypted message id (identity crypto makes the plaintext readable).
std::vector<AaMessage> parseHuSends(const FakeAaTransport& hu) {
    FakeAaTransport t;
    t.pushRx(hu.sent());
    AaMessenger m(t);
    std::vector<AaMessage> out;
    AaMessage msg;
    while (m.receive(msg, 0)) {
        if (msg.encryption == EncryptionType::Encrypted && msg.payload.size() >= 2) {
            msg.messageId = static_cast<uint16_t>((msg.payload[0] << 8) | msg.payload[1]);
        }
        out.push_back(msg);
    }
    return out;
}

bool huSent(const FakeAaTransport& hu, ChannelId ch, uint16_t msgId, EncryptionType enc) {
    for (const auto& m : parseHuSends(hu)) {
        if (m.channel == ch && m.messageId == msgId && m.encryption == enc) return true;
    }
    return false;
}

constexpr uint16_t kVersionResponse = 0x0002;
constexpr uint16_t kSslHandshake = 0x0003;
constexpr uint16_t kAuthComplete = 0x0004;
constexpr uint16_t kSvcDiscoveryReq = 0x0005;
constexpr uint16_t kSvcDiscoveryResp = 0x0006;
constexpr uint16_t kChannelOpenReq = 0x0007;
constexpr uint16_t kChannelOpenResp = 0x0008;

}  // namespace

TEST(AaConnection, FullBringUpHandshake) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(2);  // handshake completes after the kick + one phone msg
    bool videoOpened = false;
    carsh::aa::AaConnectionCallbacks cbs;
    cbs.onChannelOpened = [&](ChannelId c) {
        if (c == ChannelId::Video) videoOpened = true;
    };
    AaConnection conn(msger, crypto, cbs);

    // 1. start() emits VERSION_REQUEST (plain, control).
    conn.start();
    CHECK(conn.state() == AaState::VersionExchange);
    CHECK(huSent(hu, ChannelId::Control, 0x0001, EncryptionType::Plain));

    // 2. Phone's VERSION_RESPONSE (major/minor/status=ok) -> SSL handshake kicked.
    phoneSend(hu, ChannelId::Control, EncryptionType::Plain, MessageType::Specific,
              kVersionResponse, {0x00, 0x01, 0x00, 0x07, 0x00, 0x00});
    REQUIRE(conn.poll(100));
    CHECK(conn.state() == AaState::SslHandshake);
    CHECK(huSent(hu, ChannelId::Control, kSslHandshake, EncryptionType::Plain));

    // 3. Phone's SSL_HANDSHAKE completes the (fake) TLS handshake -> AUTH_COMPLETE
    //    sent plain, and the connection advances to await service discovery.
    phoneSend(hu, ChannelId::Control, EncryptionType::Plain, MessageType::Specific,
              kSslHandshake, {0x01, 0x02, 0x03});
    REQUIRE(conn.poll(100));
    CHECK(conn.state() == AaState::ServiceDiscovery);
    CHECK(huSent(hu, ChannelId::Control, kAuthComplete, EncryptionType::Plain));

    // 4. Phone's encrypted SERVICE_DISCOVERY_REQUEST -> encrypted response, Running.
    std::vector<uint8_t> sdReq;
    pb::Writer(sdReq).string(4, "Pixel");  // device_name
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, MessageType::Specific,
              kSvcDiscoveryReq, sdReq);
    REQUIRE(conn.poll(100));
    CHECK(conn.state() == AaState::Running);
    CHECK(huSent(hu, ChannelId::Control, kSvcDiscoveryResp, EncryptionType::Encrypted));

    // 5. Phone opens the video channel -> accepted + encrypted CHANNEL_OPEN_RESPONSE.
    // The open arrives ON the target channel (Video) as a CONTROL-type message, and
    // the response goes back on that same channel.
    std::vector<uint8_t> openReq;
    pb::Writer(openReq).uint32(2, static_cast<uint32_t>(ChannelId::Video));  // advisory
    phoneSend(hu, ChannelId::Video, EncryptionType::Encrypted, MessageType::Control,
              kChannelOpenReq, openReq);
    REQUIRE(conn.poll(100));
    CHECK(conn.channelOpen(ChannelId::Video));
    CHECK(videoOpened);
    CHECK(huSent(hu, ChannelId::Video, kChannelOpenResp, EncryptionType::Encrypted));
}

// The service-discovery response the HU emits must be well-formed protobuf the
// phone can parse — decode it back and confirm it carries channel descriptors and
// the head-unit name.
TEST(AaConnection, ServiceDiscoveryResponseIsValidProtobuf) {
    FakeAaTransport hu;
    AaMessenger msger(hu);
    FakeAaCrypto crypto(1);  // handshake done after the kick
    AaConnection conn(msger, crypto, {});
    conn.start();
    phoneSend(hu, ChannelId::Control, EncryptionType::Plain, MessageType::Specific,
              kVersionResponse, {0x00, 0x01, 0x00, 0x07, 0x00, 0x00});
    REQUIRE(conn.poll(100));  // version -> handshake kick (and crypto already done)
    phoneSend(hu, ChannelId::Control, EncryptionType::Plain, MessageType::Specific,
              kSslHandshake, {0x01});
    REQUIRE(conn.poll(100));  // -> service discovery
    std::vector<uint8_t> sdReq;
    pb::Writer(sdReq).string(4, "Pixel");
    phoneSend(hu, ChannelId::Control, EncryptionType::Encrypted, MessageType::Specific,
              kSvcDiscoveryReq, sdReq);
    REQUIRE(conn.poll(100));

    // Find the SERVICE_DISCOVERY_RESPONSE the HU sent and decode it.
    std::vector<uint8_t> resp;
    for (const auto& m : parseHuSends(hu)) {
        if (m.channel == ChannelId::Control && m.messageId == kSvcDiscoveryResp) {
            resp.assign(m.payload.begin() + 2, m.payload.end());  // strip msg id (identity enc)
        }
    }
    REQUIRE(!resp.empty());

    pb::Reader r(resp);
    int channelDescriptors = 0;
    std::string huName;
    uint32_t field;
    uint8_t wire;
    while (!r.eof()) {
        REQUIRE(r.tag(field, wire));
        if (field == 1 && wire == 2) {
            pb::Reader sub(nullptr, 0);
            REQUIRE(r.submessage(sub));
            ++channelDescriptors;
        } else if (field == 2 && wire == 2) {
            REQUIRE(r.str(huName));
        } else {
            REQUIRE(r.skip(wire));
        }
    }
    CHECK(channelDescriptors == 5);  // media-audio, av-input, sensor, input, video
    CHECK_EQ(huName, std::string("carsh"));
}
