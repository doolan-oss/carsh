// MediaSession — arbitration between the local file player, a Bluetooth phone and
// the Android Auto projection for the Now Playing view + transport routing.
#include "player/MediaSession.h"

#include <string>

#include "Test.h"
#include "bt/Bluetooth.h"

using carsh::player::MediaSession;
using carsh::player::Source;
using carsh::player::StreamActivity;

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
    CHECK(!s.shouldPauseLocal());
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

TEST(MediaSession, AndroidAutoOutranksEverything) {
    MediaSession s;
    s.setLocal(localTrack("Song A", true));
    s.setBluetooth(true, true, btMeta("Phone Song", true));  // BT streaming too
    s.setAndroidAuto(true, true);                             // ...but AA is playing
    CHECK(s.source() == Source::AndroidAuto);
    auto v = s.view();
    CHECK_EQ(v.title, std::string("Android Auto"));
    CHECK(v.playing);
    // AA pauses while BT keeps streaming: BT is what's audible now, so it takes over.
    s.setAndroidAuto(true, false);
    CHECK(s.source() == Source::Bluetooth);
}

TEST(MediaSession, TransportTargetAndPlayPauseCommand) {
    MediaSession s;
    s.setLocal(localTrack("Song A", false));
    CHECK(s.transportTarget() == Source::Local);

    // BT playing -> a play/pause toggle should PAUSE.
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);

    // BT paused -> toggle should PLAY, even while A2DP still delivers PCM (the
    // kernel's activity hold + the phone's post-pause silence tail): AVRCP has
    // reported, so its PAUSED wins over the stream.
    s.setBluetooth(true, true, btMeta("Phone Song", false));
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);

    // ...and once no PCM arrives either.
    s.setBluetooth(true, false, btMeta("Phone Song", false));
    CHECK(s.transportTarget() == Source::Bluetooth);  // still latched while paused
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);
}

// "Pause, then resume a moment later" from the head unit, on an AVRCP phone that
// keeps streaming A2DP after the pause (Android streams silence until its output
// goes to standby, ~3 s). Every frame of that tail must show '>' and send PLAY.
// A navigation prompt streaming while the media app is paused is the same case.
TEST(MediaSession, AvrcpPauseWinsOverTrailingA2dpStream) {
    MediaSession s;
    s.setLocal(localTrack("Song A", false));
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);  // the user pauses

    for (int frame = 0; frame < 5; ++frame) {  // AVRCP PAUSED, PCM still arriving
        s.setBluetooth(true, true, btMeta("Phone Song", false));
        CHECK(s.source() == Source::Bluetooth);
        CHECK(!s.view().playing);
        CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);  // resume works
    }

    // AVRCP authority survives a report that carries nothing (the flag latches for
    // the connection), e.g. a status-only update after a track change cleared text.
    s.setBluetooth(true, true, carsh::bt::AvrcpMetadata{});
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);

    // A phone that connects with its media app already paused (AVRCP reports the
    // track, never PLAYING) while a navigation prompt streams: '>' and PLAY from
    // the first frame. The track attributes alone count as an AVRCP report.
    MediaSession n;
    n.setLocal(localTrack("Song A", false));
    carsh::bt::AvrcpMetadata titleOnly;
    titleOnly.title = "Phone Song";
    n.setBluetooth(true, true, titleOnly);
    CHECK(n.source() == Source::Bluetooth);
    CHECK(!n.view().playing);
    CHECK(n.playPauseCommand() == carsh::bt::AvrcpCommand::Play);

    // A PLAYING status before any track text (the status INTERIM arrives first)
    // is a report too: the pause that follows wins over the stream's tail.
    MediaSession p;
    carsh::bt::AvrcpMetadata statusPlaying;
    statusPlaying.playing = true;
    p.setBluetooth(true, true, statusPlaying);
    CHECK(p.view().playing);
    p.setBluetooth(true, true, carsh::bt::AvrcpMetadata{});  // PAUSED, PCM tail
    CHECK(!p.view().playing);
    CHECK(p.playPauseCommand() == carsh::bt::AvrcpCommand::Play);
}

// A phone that has never reported over AVRCP on this connection (no AVRCP, so no
// status and no title): its A2DP stream stands in for "playing", so Now Playing
// shows '||' while it streams and '>' once it stops.
TEST(MediaSession, BluetoothStreamWithoutAvrcpStatusIsPausable) {
    MediaSession s;
    s.setLocal(localTrack("Song A", false));
    const carsh::bt::AvrcpMetadata none;  // nothing ever reported
    s.setBluetooth(true, true, none);     // PCM arriving
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK_EQ(s.view().title, std::string("Bluetooth"));
    CHECK(s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);
    // The stream stops: '>' and PLAY.
    s.setBluetooth(true, false, none);
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);

    // AVRCP comes up and reports PLAYING: from now on it is authoritative.
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.view().playing);
    s.setBluetooth(true, true, btMeta("Phone Song", false));  // paused, PCM tail
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);

    // A disconnect forgets it: the next connection (a phone without AVRCP) falls
    // back to its stream again.
    s.setBluetooth(false, false, none);
    s.setBluetooth(true, true, none);
    CHECK(s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);
}

// With the explicit report flag (what the kernel passes: an AVRCP callback arrived
// since this connection came up), a status-only report of PAUSED/STOPPED with no
// track text still makes AVRCP authoritative, so other phone audio streaming while
// no media plays shows '>' and the toggle sends PLAY. Without the flag the same
// metadata can't be told apart from a phone without AVRCP.
TEST(MediaSession, ExplicitAvrcpReportMakesStatusOnlyPauseAuthoritative) {
    const carsh::bt::AvrcpMetadata pausedNoText;  // PAUSED, no attributes yet

    MediaSession s;
    s.setLocal(localTrack("Song A", false));
    s.setBluetooth(true, true, pausedNoText, /*avrcpReported=*/true);  // nav prompt
    CHECK(s.source() == Source::Bluetooth);
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);
    s.setBluetooth(true, true, pausedNoText, true);
    CHECK(!s.view().playing);

    MediaSession n;  // not reported: the stream stands in
    n.setLocal(localTrack("Song A", false));
    n.setBluetooth(true, true, pausedNoText, /*avrcpReported=*/false);
    CHECK(n.view().playing);
    CHECK(n.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);
    // The flag latches for the connection: a later frame without it (the caller's
    // own flag is per-connection too) keeps AVRCP authoritative...
    n.setBluetooth(true, true, pausedNoText, true);
    n.setBluetooth(true, true, pausedNoText, false);
    CHECK(!n.view().playing);
    // ...until the phone disconnects.
    n.setBluetooth(false, false, pausedNoText, false);
    n.setBluetooth(true, true, pausedNoText, false);
    CHECK(n.view().playing);
}

// --- Latched ownership (review A2 / P4) --------------------------------------

// Pausing AA from the head unit stops its PCM; the session must still be AA (so '>'
// resumes the phone) and show it paused, not fall back to the local track.
TEST(MediaSession, AndroidAutoKeepsTransportWhilePaused) {
    MediaSession s;
    s.setLocal(localTrack("Song A", false));  // a local track is queued
    s.setAndroidAuto(true, true);             // AA media playing
    CHECK(s.transportTarget() == Source::AndroidAuto);
    CHECK(s.view().playing);

    s.setAndroidAuto(true, false);            // paused: audio stops, still connected
    CHECK(s.source() == Source::AndroidAuto);
    CHECK(s.transportTarget() == Source::AndroidAuto);
    auto v = s.view();
    CHECK_EQ(v.title, std::string("Android Auto"));
    CHECK(!v.playing);                         // the view shows '>' (paused)

    s.setAndroidAuto(true, true);             // '>' resumed it
    CHECK(s.source() == Source::AndroidAuto);
    CHECK(s.view().playing);
}

// Same for BT: a paused phone (no PCM, AVRCP PAUSED) keeps the session even though a
// local track is queued.
TEST(MediaSession, BluetoothKeepsTransportWhilePaused) {
    MediaSession s;
    s.setLocal(localTrack("Song A", false));
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Bluetooth);
    s.setBluetooth(true, false, btMeta("Phone Song", false));  // phone paused
    CHECK(s.source() == Source::Bluetooth);
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);
}

// The latch is released when the owning phone goes away.
TEST(MediaSession, LatchDropsOnDisconnect) {
    MediaSession s;
    s.setLocal(localTrack("Song A", false));
    s.setAndroidAuto(true, true);
    s.setAndroidAuto(true, false);
    CHECK(s.source() == Source::AndroidAuto);
    s.setAndroidAuto(false, false);           // dongle unplugged / session ended
    CHECK(s.source() == Source::Local);
    CHECK_EQ(s.view().title, std::string("Song A"));

    s.setBluetooth(true, true, btMeta("Phone Song", true));
    s.setBluetooth(true, false, btMeta("Phone Song", false));
    CHECK(s.source() == Source::Bluetooth);
    s.setBluetooth(false, false, btMeta("", false));
    CHECK(s.source() == Source::Local);
}

// Streaming reported while disconnected is ignored (stale ring tail after unplug).
TEST(MediaSession, StreamingIgnoredWhenNotConnected) {
    MediaSession s;
    s.setLocal(localTrack("Song A", true));
    s.setAndroidAuto(false, true);
    s.setBluetooth(false, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Local);
    CHECK(!s.shouldPauseLocal());
}

// P4: a remote that starts playing takes over; the local player must be paused
// (and stays paused: nothing here asks to resume it).
TEST(MediaSession, RemoteTakeoverPausesLocal) {
    MediaSession s;
    s.setLocal(localTrack("Song A", true));   // USB track playing
    CHECK(!s.shouldPauseLocal());
    s.setBluetooth(true, true, btMeta("Phone Song", true));  // Spotify starts
    CHECK(s.source() == Source::Bluetooth);
    CHECK(s.shouldPauseLocal());
    s.setLocal(localTrack("Song A", false));  // the app paused it
    CHECK(!s.shouldPauseLocal());
    // The phone pauses: the session stays on BT and local stays paused.
    s.setBluetooth(true, false, btMeta("Phone Song", false));
    CHECK(s.source() == Source::Bluetooth);
    CHECK(!s.view().playing);

    MediaSession a;
    a.setLocal(localTrack("Song A", true));
    a.setAndroidAuto(true, true);
    CHECK(a.shouldPauseLocal());
}

// Explicitly starting local playback takes the session back and asks the app to
// pause whichever remote is still streaming; that stream's tail can't re-grab it.
TEST(MediaSession, SelectLocalTakesSessionAndPausesRemote) {
    MediaSession s;
    s.setLocal(localTrack("Song A", false));
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Bluetooth);

    MediaSession::PauseRequest r = s.selectLocal();  // user picked a library track
    CHECK(r.bluetooth);
    CHECK(!r.androidAuto);
    s.setLocal(localTrack("Song B", true));
    CHECK(s.source() == Source::Local);
    CHECK(!s.shouldPauseLocal());

    // The phone's stream is still draining for a few frames: no takeover.
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Local);
    CHECK_EQ(s.view().title, std::string("Song B"));

    // It stops (paused), then the user presses play on the phone: BT takes over.
    s.setBluetooth(true, false, btMeta("Phone Song", false));
    CHECK(s.source() == Source::Local);
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Bluetooth);
    CHECK(s.shouldPauseLocal());
}

TEST(MediaSession, SelectLocalFromPausedRemoteRequestsNoPause) {
    MediaSession s;
    s.setLocal(localTrack("Song A", false));
    s.setAndroidAuto(true, true);
    s.setAndroidAuto(true, false);            // AA paused, still the owner
    MediaSession::PauseRequest r = s.selectLocal();
    CHECK(!r.androidAuto);
    CHECK(!r.bluetooth);
    CHECK(s.source() == Source::Local);
    // AA starts playing again later (phone-side play): it takes the session back.
    s.setAndroidAuto(true, true);
    CHECK(s.source() == Source::AndroidAuto);
}

TEST(MediaSession, SelectLocalSuppressesStreamingAndroidAuto) {
    MediaSession s;
    s.setLocal(localTrack("Song A", false));
    s.setAndroidAuto(true, true);
    MediaSession::PauseRequest r = s.selectLocal();
    CHECK(r.androidAuto);
    s.setLocal(localTrack("Song A", true));
    s.setAndroidAuto(true, true);             // jitter tail still engaged
    CHECK(s.source() == Source::Local);
    s.setAndroidAuto(true, false);
    s.setAndroidAuto(true, true);             // a genuinely new AA stream
    CHECK(s.source() == Source::AndroidAuto);
}

// Nothing latched and no local track: a connected AA session owns Now Playing (so
// '>' can start AA media), ahead of a connected BT phone.
TEST(MediaSession, ConnectedAndroidAutoShownWithNoLocalTrack) {
    MediaSession s;
    s.setAndroidAuto(true, false);
    CHECK(s.source() == Source::AndroidAuto);
    CHECK(!s.view().playing);
    s.setBluetooth(true, false, btMeta("Phone Song", false));
    CHECK(s.source() == Source::AndroidAuto);
    s.setLocal(localTrack("Song A", false));  // a local track appears: local shows
    CHECK(s.source() == Source::Local);
}

// A remote that STARTS playing takes the session from a paused (latched) owner,
// whichever it is. But while both actually stream, AA outranks BT regardless of
// which started last (it outranks A2DP in the audio mix too).
TEST(MediaSession, StartingRemoteTakesPausedSessionButAaOutranksStreamingBt) {
    MediaSession s;
    s.setAndroidAuto(true, true);
    s.setAndroidAuto(true, false);            // AA paused (latched)
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Bluetooth);
    s.setBluetooth(true, false, btMeta("Phone Song", false));
    CHECK(s.source() == Source::Bluetooth);   // BT paused, still latched
    s.setAndroidAuto(true, true);
    CHECK(s.source() == Source::AndroidAuto);

    // BT starts while AA is still streaming: BT started last, AA keeps it.
    s.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::AndroidAuto);
    CHECK(s.transportTarget() == Source::AndroidAuto);
    s.setAndroidAuto(true, true);             // next frame: both still streaming
    CHECK(s.source() == Source::AndroidAuto);
    // AA pauses; BT is still streaming, so it is what's audible: BT takes over.
    s.setAndroidAuto(true, false);
    CHECK(s.source() == Source::Bluetooth);
}

// shouldPauseLocal() is about a remote TAKING the session, not the unlatched
// fallback: with no local track row and a phone merely connected, Now Playing
// shows the phone, but a local player that is Playing (its queue entry missing
// from the library) must not be paused every frame.
TEST(MediaSession, UnlatchedFallbackNeverPausesLocal) {
    MediaSession::LocalState playingNoRow;  // hasTrack = false
    playingNoRow.playing = true;

    MediaSession a;
    a.setLocal(playingNoRow);
    a.setAndroidAuto(true, false);            // connected, idle
    CHECK(a.source() == Source::AndroidAuto);  // shown via the fallback
    CHECK(!a.shouldPauseLocal());
    a.setAndroidAuto(true, true);             // AA media starts: it takes over
    CHECK(a.shouldPauseLocal());

    MediaSession b;
    b.setLocal(playingNoRow);
    b.setBluetooth(true, false, btMeta("Phone Song", false));
    CHECK(b.source() == Source::Bluetooth);
    CHECK(!b.shouldPauseLocal());
    b.setBluetooth(true, true, btMeta("Phone Song", true));
    CHECK(b.shouldPauseLocal());
    // The owner disconnects: the latch drops and local is left alone again.
    b.setBluetooth(false, false, btMeta("", false));
    CHECK(b.source() == Source::Local);
    CHECK(!b.shouldPauseLocal());
}

// --- StreamActivity ------------------------------------------------------------

TEST(StreamActivity, FirstSampleIsBaselineOnly) {
    StreamActivity a(500);
    CHECK(!a.update(42, 0));          // data from before we watched isn't "now"
    CHECK(!a.update(42, 100000));
    CHECK(a.update(43, 200000));      // a new chunk arrived
}

TEST(StreamActivity, HoldsThroughBurstGapsThenReleases) {
    StreamActivity a(500);            // 500 ms hold
    uint32_t n = 0;
    a.update(n, 0);
    CHECK(a.update(++n, 10000));
    // A 400 ms delivery gap (bursty WiFi) keeps it streaming.
    CHECK(a.update(n, 410000));
    CHECK(a.update(++n, 420000));
    // Then it stops for good: released once the hold expires.
    CHECK(a.update(n, 900000));
    CHECK(!a.update(n, 920001));
    CHECK(!a.streaming());
    CHECK(a.update(++n, 5000000));    // resumes
}
