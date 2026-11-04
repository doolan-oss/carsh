// Bluetooth integration-seam tests (logic stubs). The radio/transport are
// hardware-gated (H6); these verify the app-side wiring: connect -> profile
// switch, AVRCP metadata -> now-playing, AVRCP CT command emission, and A2DP
// PCM flowing into a ring — all driven by FakeBluetooth.
#include "bt/FakeBluetooth.h"

#include <vector>

#include "Test.h"
#include "audio/PcmRing.h"
#include "profile/ProfileManager.h"

using namespace carsh;

TEST(Bt, ConnectSwitchesProfileByClassicAddress) {
    host::FakeBluetooth bt;
    profile::ProfileManager pm;
    profile::Profile dyl;
    dyl.addr = {{0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03}};
    dyl.displayName = "Dylan";
    dyl.theme = profile::ThemeVariant::Amber;
    pm.upsert(dyl);

    bt::BluetoothCallbacks cb;
    cb.onConnected = [&](const bt::BdAddr& a) {
        profile::BdAddr pa;
        pa.b = a;
        pm.onBtConnected(pa);
    };
    cb.onDisconnected = [&](const bt::BdAddr&) { pm.onBtDisconnected(); };
    bt.setCallbacks(cb);

    CHECK_EQ(pm.active().displayName, std::string("Guest"));
    bt.simulateConnect({{0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03}});
    CHECK_EQ(pm.active().displayName, std::string("Dylan"));
    CHECK(pm.active().theme == profile::ThemeVariant::Amber);
    bt.simulateDisconnect();
    CHECK_EQ(pm.active().displayName, std::string("Guest"));
}

TEST(Bt, AvrcpMetadataReachesNowPlaying) {
    host::FakeBluetooth bt;
    bt::AvrcpMetadata latest;
    bool got = false;
    bt::BluetoothCallbacks cb;
    cb.onMetadata = [&](const bt::AvrcpMetadata& m) { latest = m; got = true; };
    bt.setCallbacks(cb);

    bt.simulateMetadata({"Teardrop", "Massive Attack", "Mezzanine", 330000, true});
    REQUIRE(got);
    CHECK_EQ(latest.title, std::string("Teardrop"));
    CHECK_EQ(latest.artist, std::string("Massive Attack"));
    CHECK(latest.playing);
}

// The kernel's wiring: metadata makes AVRCP the phone's play-status source; the
// session-end signal (AVRCP released while A2DP may still stream) drops it again.
// An empty metadata is NOT that signal: it is a report (e.g. a status-only PAUSED).
TEST(Bt, AvrcpEndedIsItsOwnSignalNotAnEmptyMetadata) {
    host::FakeBluetooth bt;
    bt::AvrcpMetadata meta;   // g_btMeta
    bool reported = false;    // g_btAvrcpReported
    bt::BluetoothCallbacks cb;
    cb.onMetadata = [&](const bt::AvrcpMetadata& m) { meta = m; reported = true; };
    cb.onAvrcpEnded = [&] { meta = {}; reported = false; };
    bt.setCallbacks(cb);

    bt.simulateConnect({{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}});
    bt.simulateMetadata({"Teardrop", "Massive Attack", "Mezzanine", 330000, true});
    CHECK(reported);
    bt.simulateMetadata(bt::AvrcpMetadata{});  // an AVRCP report with nothing in it
    CHECK(reported);
    bt.simulateAvrcpEnded();
    CHECK(!reported);
    CHECK(meta.title.empty());
    CHECK(bt.connected());  // A2DP / the link are unaffected
}

TEST(Bt, AvrcpControllerCommandsRecorded) {
    host::FakeBluetooth bt;
    bt.sendAvrcpCommand(bt::AvrcpCommand::Play);
    bt.sendAvrcpCommand(bt::AvrcpCommand::Next);
    REQUIRE(bt.sentCommands().size() == 2);
    CHECK(bt.sentCommands()[0] == bt::AvrcpCommand::Play);
    CHECK(bt.sentCommands()[1] == bt::AvrcpCommand::Next);
}

TEST(Bt, A2dpPcmFlowsIntoRing) {
    host::FakeBluetooth bt;
    audio::PcmRing<float> ring(4096);
    bt::BluetoothCallbacks cb;
    cb.onA2dpPcm = [&](const float* pcm, std::size_t frames, uint8_t ch) {
        ring.write(pcm, frames * ch);
    };
    bt.setCallbacks(cb);

    std::vector<float> block(256 * 2, 0.25f);
    bt.simulateA2dpPcm(block.data(), 256, 2);
    CHECK_EQ(ring.readable(), std::size_t(512));
}
