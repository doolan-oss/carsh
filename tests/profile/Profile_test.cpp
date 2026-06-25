#include "profile/ProfileManager.h"

#include "Test.h"
#include "dsp/EqPreset.h"

using namespace carsh::profile;
using namespace carsh::dsp;

TEST(EqPreset, RoundTrip) {
    EqConfig cfg;
    cfg.preampDb = -3.5;
    cfg.bands.push_back({FilterType::LowShelf, 100, 4.0, 0.707});
    cfg.bands.push_back({FilterType::Peaking, 1000, -6.0, 1.4});
    cfg.bands.push_back({FilterType::HighShelf, 8000, 2.5, 0.707});
    auto blob = EqPreset::serialize(cfg);

    EqConfig out;
    REQUIRE(EqPreset::deserialize(blob.data(), blob.size(), out));
    CHECK_NEAR(out.preampDb, -3.5, 1e-5);
    REQUIRE(out.bands.size() == 3);
    CHECK(out.bands[1].type == FilterType::Peaking);
    CHECK_NEAR(out.bands[1].freqHz, 1000.0, 1e-3);
    CHECK_NEAR(out.bands[1].gainDb, -6.0, 1e-4);
    CHECK_NEAR(out.bands[2].freqHz, 8000.0, 1e-3);
}

TEST(EqPreset, RejectsBadMagic) {
    uint8_t junk[16] = {'X', 'X'};
    EqConfig out;
    CHECK(!EqPreset::deserialize(junk, sizeof(junk), out));
}

static Profile makeProfile(BdAddr addr, const std::string& name) {
    Profile p;
    p.addr = addr;
    p.displayName = name;
    p.theme = ThemeVariant::Amber;
    p.lastSource = SourceKind::Bt;
    p.favorites = {0x1111ull, 0x2222ull, 0x3333ull};
    p.eq.preampDb = -2.0;
    p.eq.bands.push_back({FilterType::Peaking, 60, 5.0, 1.0});
    return p;
}

TEST(Profile, SerializeRoundTrip) {
    BdAddr a{{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02}};
    Profile p = makeProfile(a, "Dylan");
    auto blob = p.serialize();
    Profile q;
    REQUIRE(Profile::deserialize(blob.data(), blob.size(), q));
    CHECK(q.addr == a);
    CHECK_EQ(q.displayName, std::string("Dylan"));
    CHECK(q.theme == ThemeVariant::Amber);
    CHECK(q.lastSource == SourceKind::Bt);
    REQUIRE(q.favorites.size() == 3);
    CHECK_EQ(q.favorites[2], 0x3333ull);
    REQUIRE(q.eq.bands.size() == 1);
    CHECK_NEAR(q.eq.bands[0].gainDb, 5.0, 1e-4);
}

TEST(Profile, AddrFormatting) {
    BdAddr a{{0x00, 0x1A, 0x7D, 0xDA, 0x71, 0x13}};
    CHECK_EQ(a.str(), std::string("00:1A:7D:DA:71:13"));
    CHECK_EQ(a.key(), std::string("001a7dda7113"));
}

TEST(ProfileManager, KnownDeviceActivatesProfile) {
    ProfileManager pm;
    BdAddr a{{1, 2, 3, 4, 5, 6}};
    pm.upsert(makeProfile(a, "Dylan"));
    CHECK_EQ(pm.active().displayName, std::string("Guest"));  // default
    Profile& p = pm.onBtConnected(a);
    CHECK_EQ(p.displayName, std::string("Dylan"));
    CHECK(pm.active().theme == ThemeVariant::Amber);
    CHECK(!pm.wasCreatedOnConnect());
}

TEST(ProfileManager, UnknownDeviceClonesGuestAndFlags) {
    ProfileManager pm;
    pm.guest().theme = ThemeVariant::Dark;
    BdAddr a{{9, 9, 9, 9, 9, 9}};
    Profile& p = pm.onBtConnected(a);
    CHECK(!p.isGuest());
    CHECK(p.addr == a);
    CHECK(pm.wasCreatedOnConnect());
    CHECK_EQ(pm.count(), std::size_t(1));  // remembered
    pm.onBtDisconnected();
    CHECK_EQ(pm.active().displayName, std::string("Guest"));
}

TEST(ProfileManager, PersistenceRoundTrip) {
    ProfileManager pm;
    pm.upsert(makeProfile({{1, 2, 3, 4, 5, 6}}, "Dylan"));
    pm.upsert(makeProfile({{7, 8, 9, 10, 11, 12}}, "Sam"));
    auto blobs = pm.serializeAll();
    REQUIRE(blobs.size() == 3);  // 2 + guest

    ProfileManager pm2;
    for (auto& b : blobs) CHECK(pm2.loadProfile(b.second.data(), b.second.size()));
    CHECK_EQ(pm2.count(), std::size_t(2));
    Profile& p = pm2.onBtConnected({{7, 8, 9, 10, 11, 12}});
    CHECK_EQ(p.displayName, std::string("Sam"));
}
