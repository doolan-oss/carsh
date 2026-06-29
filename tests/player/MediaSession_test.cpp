// MediaSession — arbitration between the local file player and a Bluetooth phone
// for the Now Playing view + transport routing.
#include "player/MediaSession.h"

#include <string>

#include "Test.h"
#include "bt/Bluetooth.h"

using carsh::player::MediaSession;
using carsh::player::Source;

namespace {
MediaSession::LocalState localTrack(const char* title, bool playing) {
    MediaSession::LocalState s;
    s.hasTrack = true;
    s.playing = playing;
    s.title = title;
    s.artist = "Local Artist";
    s.album = "Local Album";
    s.positionMs = 1000;
    s.durationMs = 200000;
    return s;
}
carsh::bt::AvrcpMetadata btMeta(const char* title, bool playing) {
    carsh::bt::AvrcpMetadata m;
    m.title = title;
    m.artist = "Phone Artist";
    m.album = "Phone Album";
    m.durationMs = 180000;
    m.playing = playing;
    return m;
}
}  // namespace

TEST(MediaSession, DefaultsToLocalPlaceholder) {
    MediaSession s;
    CHECK(s.source() == Source::Local);
    auto v = s.view();
    CHECK_EQ(v.title, std::string("carsh"));
    CHECK_EQ(v.artist, std::string("select a track"));
}

TEST(MediaSession, LocalTrackShownWhenNoBluetooth) {
    MediaSession s;
    s.setLocal(localTrack("Song A", true));
    CHECK(s.source() == Source::Local);
    auto v = s.view();
    CHECK_EQ(v.title, std::string("Song A"));
    CHECK_EQ(v.album, std::string("Local Album"));
    CHECK(v.playing);
    CHECK_EQ(v.durationMs, std::uint64_t(200000));
}

TEST(MediaSession, BluetoothWinsWhenStreaming) {
    MediaSession s;
    s.setLocal(localTrack("Song A", true));            // local also "playing"
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Bluetooth);
    auto v = s.view();
    CHECK_EQ(v.title, std::string("Phone Song"));
    CHECK_EQ(v.artist, std::string("Phone Artist"));
    CHECK(v.playing);
    CHECK_EQ(v.durationMs, std::uint64_t(180000));
}

TEST(MediaSession, LocalKeptWhenBluetoothConnectedButIdle) {
    MediaSession s;
    s.setLocal(localTrack("Song A", true));
    s.setBluetooth(true, false, btMeta("Phone Song", false));  // connected, not streaming
    CHECK(s.source() == Source::Local);
    CHECK_EQ(s.view().title, std::string("Song A"));
}

TEST(MediaSession, BluetoothShownWhenConnectedAndNoLocalTrack) {
    MediaSession s;
    s.setBluetooth(true, false, btMeta("Phone Song", false));  // idle but only source
    CHECK(s.source() == Source::Bluetooth);
    CHECK_EQ(s.view().title, std::string("Phone Song"));
}

TEST(MediaSession, BluetoothNeutralLabelBeforeMetadata) {
    MediaSession s;
    s.setBluetooth(true, true, carsh::bt::AvrcpMetadata{});  // streaming, no text yet
    CHECK(s.source() == Source::Bluetooth);
    CHECK_EQ(s.view().title, std::string("Bluetooth"));
}

TEST(MediaSession, TransportTargetAndPlayPauseCommand) {
    MediaSession s;
    s.setLocal(localTrack("Song A", false));
    CHECK(s.transportTarget() == Source::Local);

    // BT playing -> a play/pause toggle should PAUSE.
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);

    // BT paused -> toggle should PLAY.
    s.setBluetooth(true, true, btMeta("Phone Song", false));
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);
}
