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

// The kernel feeds the session once per UI frame (~33 ms) from a monotonic clock.
constexpr uint64_t kFrameUs = 33000;
constexpr uint64_t kTakeoverUs = uint64_t(MediaSession::kTakeoverMs) * 1000u;

// One frame of a BT snapshot. `reported`: the kernel's per-AVRCP-session flag.
void btFrame(MediaSession& s, uint64_t& t, bool audible,
             const carsh::bt::AvrcpMetadata& md, bool reported = true,
             bool connected = true) {
    t += kFrameUs;
    s.setBluetooth(connected, audible, md, reported, t);
}
// `us` of AA frames with its audio arriving (or not), unbroken.
void aaFor(MediaSession& s, uint64_t& t, uint64_t us, bool audible = true,
           bool connected = true) {
    const uint64_t end = t + us;
    do {
        t += kFrameUs;
        s.setAndroidAuto(connected, audible, t);
    } while (t < end);
}
// AA media that has been playing long enough to take the session.
void aaPlays(MediaSession& s, uint64_t& t) { aaFor(s, t, kTakeoverUs + kFrameUs); }
// A phone without AVRCP streaming, likewise.
void btStreamsWithoutAvrcp(MediaSession& s, uint64_t& t, uint64_t us) {
    const uint64_t end = t + us;
    do btFrame(s, t, true, carsh::bt::AvrcpMetadata{}, false);
    while (t < end);
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

TEST(MediaSession, BluetoothTakesOverWhenAvrcpSaysPlaying) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", true));            // local also "playing"
    btFrame(s, t, true, btMeta("Phone Song", true));   // the phone starts playing
    CHECK(s.source() == Source::Bluetooth);            // at once: AVRCP says so
    auto v = s.view();
    CHECK_EQ(v.title, std::string("Phone Song"));
    CHECK_EQ(v.artist, std::string("Phone Artist"));
    CHECK(v.playing);
    CHECK_EQ(v.durationMs, std::uint64_t(180000));
}

TEST(MediaSession, LocalKeptWhenBluetoothConnectedButIdle) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", true));
    btFrame(s, t, false, btMeta("Phone Song", false));  // connected, not playing
    CHECK(s.source() == Source::Local);
    CHECK_EQ(s.view().title, std::string("Song A"));
    CHECK(!s.shouldPauseLocal());
}

TEST(MediaSession, BluetoothShownWhenConnectedAndNoLocalTrack) {
    MediaSession s;
    uint64_t t = 0;
    btFrame(s, t, false, btMeta("Phone Song", false));  // idle but only source
    CHECK(s.source() == Source::Bluetooth);
    CHECK_EQ(s.view().title, std::string("Phone Song"));
}

TEST(MediaSession, BluetoothNeutralLabelBeforeMetadata) {
    MediaSession s;
    uint64_t t = 0;
    btFrame(s, t, true, carsh::bt::AvrcpMetadata{}, false);  // streaming, no AVRCP yet
    CHECK(s.source() == Source::Bluetooth);
    CHECK_EQ(s.view().title, std::string("Bluetooth"));
}

TEST(MediaSession, AndroidAutoOutranksEverything) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", true));
    btFrame(s, t, true, btMeta("Phone Song", true));  // BT playing too
    aaPlays(s, t);                                     // ...but AA media plays
    CHECK(s.source() == Source::AndroidAuto);
    auto v = s.view();
    CHECK_EQ(v.title, std::string("Android Auto"));
    CHECK(v.playing);
    // AA pauses while BT keeps playing: BT is what's audible now, so it takes over.
    aaFor(s, t, kFrameUs, false);
    CHECK(s.source() == Source::Bluetooth);
}

TEST(MediaSession, TransportTargetAndPlayPauseCommand) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", false));
    CHECK(s.transportTarget() == Source::Local);

    // BT playing -> a play/pause toggle should PAUSE.
    btFrame(s, t, true, btMeta("Phone Song", true));
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);

    // BT paused -> toggle should PLAY, even while A2DP still delivers PCM (the
    // kernel's activity hold + the phone's post-pause silence tail): AVRCP has
    // reported, so its PAUSED wins over the stream.
    btFrame(s, t, true, btMeta("Phone Song", false));
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);

    // ...and once no PCM arrives either.
    btFrame(s, t, false, btMeta("Phone Song", false));
    CHECK(s.transportTarget() == Source::Bluetooth);  // still latched while paused
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);
}

// "Pause, then resume a moment later" from the head unit, on an AVRCP phone that
// keeps streaming A2DP after the pause (Android streams silence until its output
// goes to standby, ~3 s). Every frame of that tail must show '>' and send PLAY.
// A navigation prompt streaming while the media app is paused is the same case.
TEST(MediaSession, AvrcpPauseWinsOverTrailingA2dpStream) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", false));
    btFrame(s, t, true, btMeta("Phone Song", true));
    CHECK(s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);  // the user pauses

    for (int frame = 0; frame < 5; ++frame) {  // AVRCP PAUSED, PCM still arriving
        btFrame(s, t, true, btMeta("Phone Song", false));
        CHECK(s.source() == Source::Bluetooth);
        CHECK(!s.view().playing);
        CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);  // resume works
    }

    // A status-only report (a track change cleared the text) is still AVRCP: the
    // kernel's flag stays set for the whole AVRCP session.
    btFrame(s, t, true, carsh::bt::AvrcpMetadata{});
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);

    // A phone that connects with its media app already paused (AVRCP reports the
    // track, never PLAYING) while a navigation prompt streams: '>' and PLAY from
    // the first frame.
    MediaSession n;
    uint64_t tn = 0;
    n.setLocal(localTrack("Song A", false));
    carsh::bt::AvrcpMetadata titleOnly;
    titleOnly.title = "Phone Song";
    btFrame(n, tn, true, titleOnly);
    CHECK(n.source() == Source::Local);  // a prompt doesn't take the session...
    CHECK(!n.shouldPauseLocal());
    CHECK(n.playPauseCommand() == carsh::bt::AvrcpCommand::Play);  // ...nor show '||'
    n.setLocal(MediaSession::LocalState{});  // no local track: Now Playing shows BT
    btFrame(n, tn, true, titleOnly);
    CHECK(n.source() == Source::Bluetooth);
    CHECK(!n.view().playing);
}

// A phone without AVRCP (never reports on this connection): its A2DP stream stands
// in for both "playing" and "started playing". Once it has streamed for
// kTakeoverMs it owns the session; Now Playing then shows '||' while it streams
// and '>' once it stops.
TEST(MediaSession, BluetoothStreamWithoutAvrcpStatusIsPausable) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", false));
    const carsh::bt::AvrcpMetadata none;  // nothing ever reported
    btStreamsWithoutAvrcp(s, t, kTakeoverUs + kFrameUs);
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK_EQ(s.view().title, std::string("Bluetooth"));
    CHECK(s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);
    // The stream stops: '>' and PLAY.
    btFrame(s, t, false, none, false);
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);

    // AVRCP comes up and reports PLAYING: from now on it is authoritative.
    btFrame(s, t, true, btMeta("Phone Song", true));
    CHECK(s.view().playing);
    btFrame(s, t, true, btMeta("Phone Song", false));  // paused, PCM tail
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);

    // A disconnect forgets it: the next connection (a phone without AVRCP) falls
    // back to its stream again, for the play state and for the takeover.
    btFrame(s, t, false, none, false, /*connected=*/false);
    CHECK(s.source() == Source::Local);
    btFrame(s, t, true, none, false);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);
    CHECK(s.source() == Source::Local);  // not yet: it has only just started
    btStreamsWithoutAvrcp(s, t, kTakeoverUs);
    CHECK(s.source() == Source::Bluetooth);
    CHECK(s.view().playing);
}

// The kernel's flag, not the metadata's content, says whether AVRCP is
// authoritative: a status-only PAUSED/STOPPED with no track text still makes it so,
// and clearing the flag (the AVRCP session ended, or a reconnect) hands the play
// state back to the A2DP stream.
TEST(MediaSession, AvrcpReportFlagDecidesAndItsEndFallsBackToTheStream) {
    const carsh::bt::AvrcpMetadata pausedNoText;  // PAUSED, no attributes yet

    MediaSession s;
    uint64_t t = 0;
    btFrame(s, t, true, pausedNoText, /*reported=*/true);  // a nav prompt, no local
    CHECK(s.source() == Source::Bluetooth);                  // (fallback: no track)
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);

    MediaSession n;  // not reported: the stream stands in
    uint64_t tn = 0;
    btFrame(n, tn, true, pausedNoText, /*reported=*/false);
    CHECK(n.view().playing);
    CHECK(n.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);

    // An AVRCP phone owns the session and plays; then only its AVRCP goes away
    // (released while A2DP keeps streaming). The kernel clears the metadata and
    // the flag: the session stays BT, and '||' follows the stream again instead of
    // a stale PAUSED showing '>' over playing music.
    MediaSession r;
    uint64_t tr = 0;
    r.setLocal(localTrack("Song A", false));
    btFrame(r, tr, true, btMeta("Phone Song", true));
    CHECK(r.source() == Source::Bluetooth);
    btFrame(r, tr, true, carsh::bt::AvrcpMetadata{}, /*reported=*/false);
    CHECK(r.source() == Source::Bluetooth);
    CHECK(r.view().playing);
    CHECK_EQ(r.view().title, std::string("Bluetooth"));
    CHECK(r.playPauseCommand() == carsh::bt::AvrcpCommand::Pause);
    btFrame(r, tr, false, carsh::bt::AvrcpMetadata{}, false);  // the stream stops
    CHECK(!r.view().playing);
    // AVRCP reconnects and reports: authoritative again.
    btFrame(r, tr, true, btMeta("Phone Song", false), true);
    CHECK(!r.view().playing);
}

// --- Chimes and prompts vs real playback (final review) ----------------------
// A notification chime or a navigation prompt from a connected phone must play
// over the local track and leave it playing; only real playback takes the session
// and pauses it.

// AVRCP says PAUSED: no amount of A2DP audio takes the session. This replays the
// kernel's per-frame sequence: A2DP chunk counter -> StreamActivity -> the session.
TEST(MediaSession, ChimeFromAPausedAvrcpPhoneLeavesTheLocalTrackPlaying) {
    MediaSession s;
    StreamActivity btActivity(800);
    uint32_t chunks = 0;
    uint64_t t = 1000000;
    s.setLocal(localTrack("Song A", true));
    const carsh::bt::AvrcpMetadata paused = btMeta("Podcast", false);
    auto frame = [&](bool chunkArrived) {
        t += kFrameUs;
        if (chunkArrived) chunks += 3;  // ~3 A2DP packets per frame
        s.setBluetooth(true, btActivity.update(chunks, t), paused, true, t);
        CHECK(!s.shouldPauseLocal());
        CHECK(s.source() == Source::Local);
    };
    for (int i = 0; i < 10; ++i) frame(false);   // connected, AVRCP PAUSED
    for (int i = 0; i < 30; ++i) frame(true);    // ~1 s chime
    for (int i = 0; i < 90; ++i) frame(false);   // ~3 s idle
    CHECK_EQ(s.view().title, std::string("Song A"));
    CHECK(s.view().playing);
    // Even a long prompt (10 s) can't: AVRCP is authoritative.
    for (int i = 0; i < 300; ++i) frame(true);
    // The user presses play on the phone: now it takes over.
    t += kFrameUs;
    s.setBluetooth(true, true, btMeta("Podcast", true), true, t);
    CHECK(s.source() == Source::Bluetooth);
    CHECK(s.shouldPauseLocal());
}

// Android Auto has no play-status channel: a prompt shorter than kTakeoverMs (with
// the kernel's 800 ms activity hold on top) plays over the local track and ends.
TEST(MediaSession, AndroidAutoPromptLeavesTheLocalTrackPlaying) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", true));
    aaFor(s, t, 2000000, false);                 // projecting, silent
    CHECK(s.source() == Source::Local);
    aaFor(s, t, 800000 + 4000000);               // a 4 s prompt + the hold
    CHECK(s.source() == Source::Local);
    CHECK(!s.shouldPauseLocal());
    CHECK(s.audible(Source::AndroidAuto));       // it is heard, over the local track
    aaFor(s, t, 3000000, false);
    CHECK(s.source() == Source::Local);
    CHECK(s.view().playing);
    CHECK_EQ(s.view().title, std::string("Song A"));

    // Prompts back to back, each with a gap in between: the timer restarts on
    // every gap, so they never add up to a takeover.
    for (int i = 0; i < 4; ++i) {
        aaFor(s, t, kTakeoverUs - 1000000);
        aaFor(s, t, kFrameUs, false);
    }
    CHECK(s.source() == Source::Local);
    CHECK(!s.shouldPauseLocal());

    // Media: audio that keeps coming takes the session once it has lasted
    // kTakeoverMs, and the local track is paused.
    aaFor(s, t, kTakeoverUs - 2 * kFrameUs);
    CHECK(s.source() == Source::Local);
    aaFor(s, t, 3 * kFrameUs);
    CHECK(s.source() == Source::AndroidAuto);
    CHECK(s.shouldPauseLocal());
}

// A phone without AVRCP: the same rule as Android Auto.
TEST(MediaSession, ChimeFromAPhoneWithoutAvrcpLeavesTheLocalTrackPlaying) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", true));
    btStreamsWithoutAvrcp(s, t, 1500000);  // a chime + the hold
    CHECK(s.source() == Source::Local);
    CHECK(!s.shouldPauseLocal());
    btFrame(s, t, false, carsh::bt::AvrcpMetadata{}, false);
    btStreamsWithoutAvrcp(s, t, kTakeoverUs + kFrameUs);  // music
    CHECK(s.source() == Source::Bluetooth);
    CHECK(s.shouldPauseLocal());
}

// A phone whose AVRCP said PAUSED keeps an A2DP stream up (a long prompt, or a
// stream that never goes to standby) past the takeover window, then AVRCP alone is
// released. Only audio heard without a status report may count toward the
// status-less takeover, so local must keep playing until the window elapses anew.
TEST(MediaSession, AvrcpReleaseMidStreamRestartsTheTakeoverWindow) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", true));
    const auto paused = btMeta("Podcast", false);
    const uint64_t end = t + kTakeoverUs + 1000000;  // 7 s of audio while PAUSED
    do btFrame(s, t, true, paused, true);
    while (t < end);
    CHECK(s.source() == Source::Local);
    CHECK(!s.shouldPauseLocal());

    btFrame(s, t, true, carsh::bt::AvrcpMetadata{}, false);  // AVRCP released
    CHECK(s.source() == Source::Local);
    CHECK(!s.shouldPauseLocal());

    btStreamsWithoutAvrcp(s, t, kTakeoverUs + kFrameUs);  // a real, status-less stream
    CHECK(s.source() == Source::Bluetooth);
    CHECK(s.shouldPauseLocal());
}

// --- Latched ownership (review A2 / P4) --------------------------------------

// Pausing AA from the head unit stops its PCM; the session must still be AA (so '>'
// resumes the phone) and show it paused, not fall back to the local track.
TEST(MediaSession, AndroidAutoKeepsTransportWhilePaused) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", false));  // a local track is queued
    aaPlays(s, t);                            // AA media playing
    CHECK(s.transportTarget() == Source::AndroidAuto);
    CHECK(s.view().playing);

    aaFor(s, t, kFrameUs, false);             // paused: audio stops, still connected
    CHECK(s.source() == Source::AndroidAuto);
    CHECK(s.transportTarget() == Source::AndroidAuto);
    auto v = s.view();
    CHECK_EQ(v.title, std::string("Android Auto"));
    CHECK(!v.playing);                         // the view shows '>' (paused)

    aaFor(s, t, kFrameUs);                    // '>' resumed it
    CHECK(s.source() == Source::AndroidAuto);
    CHECK(s.view().playing);
}

// Same for BT: a paused phone (no PCM, AVRCP PAUSED) keeps the session even though a
// local track is queued.
TEST(MediaSession, BluetoothKeepsTransportWhilePaused) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", false));
    btFrame(s, t, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Bluetooth);
    btFrame(s, t, false, btMeta("Phone Song", false));  // phone paused
    CHECK(s.source() == Source::Bluetooth);
    CHECK(s.transportTarget() == Source::Bluetooth);
    CHECK(!s.view().playing);
    CHECK(s.playPauseCommand() == carsh::bt::AvrcpCommand::Play);
}

// The latch is released when the owning phone goes away.
TEST(MediaSession, LatchDropsOnDisconnect) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", false));
    aaPlays(s, t);
    aaFor(s, t, kFrameUs, false);
    CHECK(s.source() == Source::AndroidAuto);
    aaFor(s, t, kFrameUs, false, /*connected=*/false);  // unplugged / session ended
    CHECK(s.source() == Source::Local);
    CHECK_EQ(s.view().title, std::string("Song A"));

    btFrame(s, t, true, btMeta("Phone Song", true));
    btFrame(s, t, false, btMeta("Phone Song", false));
    CHECK(s.source() == Source::Bluetooth);
    btFrame(s, t, false, btMeta("", false), false, /*connected=*/false);
    CHECK(s.source() == Source::Local);
}

// Streaming reported while disconnected is ignored (stale ring tail after unplug).
TEST(MediaSession, StreamingIgnoredWhenNotConnected) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", true));
    aaFor(s, t, kTakeoverUs + kFrameUs, true, /*connected=*/false);
    btFrame(s, t, true, btMeta("Phone Song", true), true, /*connected=*/false);
    CHECK(s.source() == Source::Local);
    CHECK(!s.shouldPauseLocal());
    CHECK(!s.audible(Source::AndroidAuto));
    CHECK(!s.audible(Source::Bluetooth));
}

// P4: a remote that starts playing takes over; the local player must be paused
// (and stays paused: nothing here asks to resume it).
TEST(MediaSession, RemoteTakeoverPausesLocal) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", true));   // USB track playing
    CHECK(!s.shouldPauseLocal());
    btFrame(s, t, true, btMeta("Phone Song", true));  // Spotify starts
    CHECK(s.source() == Source::Bluetooth);
    CHECK(s.shouldPauseLocal());
    s.setLocal(localTrack("Song A", false));  // the app paused it
    CHECK(!s.shouldPauseLocal());
    // The phone pauses: the session stays on BT and local stays paused.
    btFrame(s, t, false, btMeta("Phone Song", false));
    CHECK(s.source() == Source::Bluetooth);
    CHECK(!s.view().playing);

    MediaSession a;
    uint64_t ta = 0;
    a.setLocal(localTrack("Song A", true));
    aaPlays(a, ta);
    CHECK(a.shouldPauseLocal());
}

// Explicitly starting local playback takes the session back and asks the app to
// pause whichever remote is still playing; that stream's tail can't re-grab it.
TEST(MediaSession, SelectLocalTakesSessionAndPausesRemote) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", false));
    btFrame(s, t, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Bluetooth);

    MediaSession::PauseRequest r = s.selectLocal();  // user picked a library track
    CHECK(r.bluetooth);
    CHECK(!r.androidAuto);
    s.setLocal(localTrack("Song B", true));
    CHECK(s.source() == Source::Local);
    CHECK(!s.shouldPauseLocal());

    // AVRCP still says PLAYING for a few frames (the pause's round trip): no takeover.
    btFrame(s, t, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Local);
    CHECK_EQ(s.view().title, std::string("Song B"));

    // The pause lands; A2DP keeps sending its tail. Then the user presses play on
    // the phone while that tail is still arriving: BT takes over at once.
    btFrame(s, t, true, btMeta("Phone Song", false));
    CHECK(s.source() == Source::Local);
    btFrame(s, t, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Bluetooth);
    CHECK(s.shouldPauseLocal());
}

TEST(MediaSession, SelectLocalFromPausedRemoteRequestsNoPause) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", false));
    aaPlays(s, t);
    aaFor(s, t, kFrameUs, false);             // AA paused, still the owner
    MediaSession::PauseRequest r = s.selectLocal();
    CHECK(!r.androidAuto);
    CHECK(!r.bluetooth);
    CHECK(s.source() == Source::Local);
    // AA starts playing again later (phone-side play): it takes the session back.
    aaPlays(s, t);
    CHECK(s.source() == Source::AndroidAuto);
}

TEST(MediaSession, SelectLocalSuppressesStreamingAndroidAuto) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", false));
    aaPlays(s, t);
    MediaSession::PauseRequest r = s.selectLocal();
    CHECK(r.androidAuto);
    s.setLocal(localTrack("Song A", true));
    aaFor(s, t, kTakeoverUs + kFrameUs);      // jitter tail still engaged (or AA
    CHECK(s.source() == Source::Local);       // ignored the pause): held off
    aaFor(s, t, kFrameUs, false);
    aaPlays(s, t);                            // a genuinely new AA stream
    CHECK(s.source() == Source::AndroidAuto);
}

// Nothing latched and no local track: a connected AA session owns Now Playing (so
// '>' can start AA media), ahead of a connected BT phone.
TEST(MediaSession, ConnectedAndroidAutoShownWithNoLocalTrack) {
    MediaSession s;
    uint64_t t = 0;
    aaFor(s, t, kFrameUs, false);
    CHECK(s.source() == Source::AndroidAuto);
    CHECK(!s.view().playing);
    btFrame(s, t, false, btMeta("Phone Song", false));
    CHECK(s.source() == Source::AndroidAuto);
    s.setLocal(localTrack("Song A", false));  // a local track appears: local shows
    CHECK(s.source() == Source::Local);
}

// A remote that STARTS playing takes the session from a paused (latched) owner,
// whichever it is. But while both actually play, AA outranks BT regardless of
// which started last (it outranks A2DP in the audio mix too).
TEST(MediaSession, StartingRemoteTakesPausedSessionButAaOutranksStreamingBt) {
    MediaSession s;
    uint64_t t = 0;
    aaPlays(s, t);
    aaFor(s, t, kFrameUs, false);             // AA paused (latched)
    btFrame(s, t, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::Bluetooth);
    btFrame(s, t, false, btMeta("Phone Song", false));
    CHECK(s.source() == Source::Bluetooth);   // BT paused, still latched
    aaPlays(s, t);
    CHECK(s.source() == Source::AndroidAuto);

    // BT starts while AA is still playing: BT started last, AA keeps it.
    btFrame(s, t, true, btMeta("Phone Song", true));
    CHECK(s.source() == Source::AndroidAuto);
    CHECK(s.transportTarget() == Source::AndroidAuto);
    aaFor(s, t, kFrameUs);                    // next frame: both still playing
    CHECK(s.source() == Source::AndroidAuto);
    // AA pauses; BT is still playing, so it is what's audible: BT takes over.
    aaFor(s, t, kFrameUs, false);
    btFrame(s, t, true, btMeta("Phone Song", true));
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
    uint64_t ta = 0;
    a.setLocal(playingNoRow);
    aaFor(a, ta, kFrameUs, false);            // connected, idle
    CHECK(a.source() == Source::AndroidAuto);  // shown via the fallback
    CHECK(!a.shouldPauseLocal());
    aaPlays(a, ta);                           // AA media starts: it takes over
    CHECK(a.shouldPauseLocal());

    MediaSession b;
    uint64_t tb = 0;
    b.setLocal(playingNoRow);
    btFrame(b, tb, false, btMeta("Phone Song", false));
    CHECK(b.source() == Source::Bluetooth);
    CHECK(!b.shouldPauseLocal());
    btFrame(b, tb, true, btMeta("Phone Song", true));
    CHECK(b.shouldPauseLocal());
    // The owner disconnects: the latch drops and local is left alone again.
    btFrame(b, tb, false, btMeta("", false), false, /*connected=*/false);
    CHECK(b.source() == Source::Local);
    CHECK(!b.shouldPauseLocal());
}

// audible() feeds the output-rate choice: live remote audio counts even when it
// doesn't own the session (a chime at its own rate); the tail of a stream that
// selectLocal() paused doesn't (local plays at its own rate from the start).
TEST(MediaSession, AudibleExcludesTheTailOfAPausedStream) {
    MediaSession s;
    uint64_t t = 0;
    s.setLocal(localTrack("Song A", true));
    CHECK(s.audible(Source::Local));
    btFrame(s, t, true, btMeta("Phone Song", false));  // a prompt, AVRCP PAUSED
    CHECK(s.source() == Source::Local);
    CHECK(s.audible(Source::Bluetooth));
    btFrame(s, t, true, btMeta("Phone Song", true));   // music: it takes over
    CHECK(s.audible(Source::Bluetooth));

    s.selectLocal();                                    // the user picks local
    btFrame(s, t, true, btMeta("Phone Song", false));  // paused; tail arriving
    CHECK(s.source() == Source::Local);
    CHECK(!s.audible(Source::Bluetooth));
    btFrame(s, t, false, btMeta("Phone Song", false)); // the tail ends
    btFrame(s, t, true, btMeta("Phone Song", false));  // a later prompt is live
    CHECK(s.audible(Source::Bluetooth));

    MediaSession a;
    uint64_t ta = 0;
    a.setLocal(localTrack("Song A", false));
    aaPlays(a, ta);
    a.selectLocal();
    aaFor(a, ta, kFrameUs);
    CHECK(!a.audible(Source::AndroidAuto));
    aaFor(a, ta, kFrameUs, false);
    aaFor(a, ta, kFrameUs);
    CHECK(a.audible(Source::AndroidAuto));
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
