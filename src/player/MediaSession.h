// MediaSession — arbitrates the head unit's "now playing" between the local file
// player, a connected Bluetooth phone (A2DP audio + AVRCP metadata/transport) and
// the Android Auto projection, and produces one unified view-model for the Now
// Playing screen plus the transport target (which backend a play/pause/next/prev
// should drive).
//
// It is platform-independent and host-tested: the app feeds it a snapshot each frame
// (local player fields + the latest AVRCP metadata + whether BT / AA are connected
// and their audio is arriving, with a monotonic clock) and reads back the view +
// source. No player/BTstack types leak in —
// only plain values and bt::AvrcpMetadata — so it sits cleanly between the two.
//
// Ownership is LATCHED, not re-derived from audio activity every frame. A paused
// phone produces no audio, yet it is still what the user is listening to: Now
// Playing must keep showing it and '>' must resume it, not flip to (and start) the
// local track underneath. So:
//   * A remote source (Android Auto or Bluetooth) that STARTS playing takes the
//     session: it becomes the owner and stays the owner while it is paused, until
//     another source is explicitly started or it disconnects. If both play at
//     once, Android Auto wins (it outranks A2DP in the audio mix).
//   * "Started playing" is not "some audio arrived". A phone also sends
//     notification chimes and navigation prompts, and those must not take the
//     session: that would pause the local track for good. A BT phone whose AVRCP
//     has reported on this connection started playing when AVRCP says PLAYING,
//     whatever its A2DP carries. A remote with no play-status channel (Android
//     Auto, which carsh gives only the media audio channel, or a phone without
//     AVRCP) started playing once its audio has kept coming for kTakeoverMs;
//     anything shorter just plays over the local track (the mix) and ends.
//   * The user explicitly starting local playback (picking a track, PLAY/SHUFFLE,
//     a queue row) calls selectLocal(): local owns the session again, and a remote
//     still playing is held off until it stops — so the tail of a stream the app
//     just paused can't grab the session straight back. The returned
//     PauseRequest names the remotes the app should pause.
//   * With nothing latched: Android Auto if it's connected and there's no local
//     track, else Bluetooth under the same rule, else the local player. This
//     fallback only picks what Now Playing shows; it never pauses the local player.
// Transport (play/pause/next/prev) routes to the owner: AA media keys over the input
// channel, AVRCP to the BT phone, or the local player. While a remote has TAKEN the
// session (latched) the local player must not play underneath it
// (shouldPauseLocal()). The BT phone's '>' / '||' (and so its play/pause command)
// follows AVRCP while AVRCP is up and has reported; its A2DP stream stands in only
// for a phone without that.
#pragma once

#include <cstdint>
#include <string>

#include "bt/Bluetooth.h"

namespace carsh::player {

enum class Source : uint8_t { Local, Bluetooth, AndroidAuto };

// Debounces "new data arrived" into a streaming flag. The app samples a counter the
// producer bumps per delivered PCM chunk (A2DP, AA media) once per UI frame, with a
// monotonic clock: streaming while data arrived within the last holdMs. Unlike ring
// occupancy it can't be pinned on by a stale sub-block tail left in a ring, and the
// hold rides out bursty delivery (AA over the dongle's WiFi gaps 200-600 ms).
class StreamActivity {
public:
    explicit StreamActivity(uint32_t holdMs) : holdUs_(uint64_t(holdMs) * 1000u) {}

    // counter: the producer's running count of delivered chunks. nowUs: monotonic.
    bool update(uint32_t counter, uint64_t nowUs) {
        if (!primed_) {
            // First sample only establishes the baseline (data delivered before
            // the app started watching is not "streaming now").
            primed_ = true;
            last_ = counter;
            return false;
        }
        if (counter != last_) {
            last_ = counter;
            lastArrivalUs_ = nowUs;
            seen_ = true;
        }
        streaming_ = seen_ && nowUs - lastArrivalUs_ < holdUs_;
        return streaming_;
    }
    bool streaming() const { return streaming_; }

private:
    uint64_t holdUs_;
    uint32_t last_ = 0;
    uint64_t lastArrivalUs_ = 0;
    bool primed_ = false;
    bool seen_ = false;
    bool streaming_ = false;
};

class MediaSession {
public:
    // How long the audio of a remote WITHOUT a play-status channel must keep coming,
    // unbroken, before it counts as the remote having started playing (and so takes
    // the session and pauses the local track). The kernel's audible signal outlasts
    // the audio by ~1 s (StreamActivity's 800 ms hold; for AA also the jitter
    // buffer playing out), so this is ~5 s of actual audio. That is longer than a
    // notification chime (< 2 s) and than nearly every navigation prompt (typically
    // 2-5 s spoken), while music is continuous and gets there anyway. Until then
    // nothing is lost: remote audio outranks local in the output mix, so it is heard
    // at once; only the Now Playing switch and the local pause wait. A longer
    // utterance (an assistant answer) still takes the session: without a status
    // channel it can't be told from music.
    static constexpr uint32_t kTakeoverMs = 6000;

    // Local file-player snapshot (plain values; no PlayerController dependency).
    struct LocalState {
        bool hasTrack = false;   // a track is queued/loaded
        bool playing = false;
        std::string title, artist, album;
        uint64_t positionMs = 0, durationMs = 0;
    };

    // Unified now-playing view for the UI.
    struct NowPlayingView {
        Source source = Source::Local;
        std::string title, artist, album;
        uint64_t positionMs = 0, durationMs = 0;
        bool playing = false;
    };

    // The remote sources still playing when local playback was selected: a source
    // switch pauses the source it leaves, so the app sends these a pause.
    struct PauseRequest {
        bool androidAuto = false;
        bool bluetooth = false;
    };

    // --- Inputs, refreshed each frame --------------------------------------
    void setLocal(const LocalState& s) { local_ = s; }
    // connected: a phone is linked. audible: its A2DP PCM is arriving (the kernel's
    // StreamActivity over the delivered-chunk count; not ring occupancy). md: the
    // latest AVRCP metadata. avrcpReported: AVRCP is up on THIS connection and has
    // reported (a playback status or a track attribute) since it came up; the
    // caller clears it when that AVRCP session ends (released, or the link
    // dropped) and on every (re)connect. While it is set, AVRCP alone decides
    // whether the phone is playing: its PLAYING takes the session, and its
    // PLAYING/PAUSED drives the '>' / '||' and the play/pause command, whatever
    // A2DP carries (a paused phone's silence tail, a navigation prompt). Without
    // it the phone has no status channel and its A2DP audio stands in. nowUs: the
    // snapshot's monotonic clock (µs).
    void setBluetooth(bool connected, bool audible, const carsh::bt::AvrcpMetadata& md,
                      bool avrcpReported, uint64_t nowUs) {
        bt_.update(connected, audible, avrcpReported, md.playing, nowUs);
        btMeta_ = md;
        resolve();
    }
    // connected: a projection session is up (AaProjection::connected()). audible:
    // AA media audio is arriving or still playing out (media PCM chunks, or the
    // jitter buffer engaged). AA has no metadata or play-status channel, so its
    // audio is all there is (kTakeoverMs). nowUs: as above.
    void setAndroidAuto(bool connected, bool audible, uint64_t nowUs) {
        aa_.update(connected, audible, false, false, nowUs);
        resolve();
    }

    // The user explicitly started local playback: local owns the session until a
    // remote source starts playing again. Returns the remotes to pause.
    PauseRequest selectLocal() {
        PauseRequest r;
        r.androidAuto = aa_.holdOff();
        r.bluetooth = bt_.holdOff();
        latched_ = Source::Local;
        return r;
    }

    // --- Outputs ------------------------------------------------------------
    Source source() const {
        if (latched_ == Source::AndroidAuto && aa_.connected) return Source::AndroidAuto;
        if (latched_ == Source::Bluetooth && bt_.connected) return Source::Bluetooth;
        if (!local_.hasTrack) {
            if (aa_.connected) return Source::AndroidAuto;
            if (bt_.connected) return Source::Bluetooth;
        }
        return Source::Local;
    }

    NowPlayingView view() const {
        NowPlayingView v;
        v.source = source();
        if (v.source == Source::AndroidAuto) {
            // No AA metadata channel yet — the projection tab shows the rich UI; here
            // we just label the source and let the transport buttons drive it.
            v.title = "Android Auto";
            v.artist = "Projecting";
            v.playing = aa_.playing();
        } else if (v.source == Source::Bluetooth) {
            // Before the first AVRCP attribute arrives, show a neutral label so the
            // screen isn't blank while connected.
            v.title = btMeta_.title.empty() ? "Bluetooth" : btMeta_.title;
            v.artist = btMeta_.artist;
            v.album = btMeta_.album;
            v.durationMs = btMeta_.durationMs;
            v.positionMs = 0;  // AVRCP play position not tracked yet (no BT scrub)
            v.playing = bt_.playing();
        } else if (local_.hasTrack) {
            v.title = local_.title;
            v.artist = local_.artist;
            v.album = local_.album;
            v.positionMs = local_.positionMs;
            v.durationMs = local_.durationMs;
            v.playing = local_.playing;
        } else {
            v.title = "carsh";
            v.artist = "select a track";
        }
        return v;
    }

    // Where a transport intent should go right now.
    Source transportTarget() const { return source(); }

    // A remote source has TAKEN the session (it started playing and is latched)
    // but the local player is still playing underneath it: the app must pause the
    // local player. It is only ever resumed by an explicit local action. The
    // unlatched fallback (no local track + a phone connected) never pauses local:
    // nothing was started there, and a local player that is Playing without a
    // track row must not be paused again every frame.
    bool shouldPauseLocal() const {
        return latched_ != Source::Local && source() == latched_ && local_.playing;
    }

    // Convenience for a play/pause toggle: the AVRCP command to send when BT is the
    // target (Play when currently paused/stopped, Pause when playing). Matches the
    // view's '>' / '||' for the BT source.
    carsh::bt::AvrcpCommand playPauseCommand() const {
        return bt_.playing() ? carsh::bt::AvrcpCommand::Pause
                             : carsh::bt::AvrcpCommand::Play;
    }

    // Is `s`'s audio arriving now, as live audio rather than the tail of a stream
    // selectLocal() just paused (a phone keeps sending A2DP for up to a few seconds
    // after a pause, mostly silence)? The kernel feeds the Bluetooth answer to the
    // output-rate choice (player/OutputRate.h), so a chime or prompt from a phone
    // that doesn't own the session still plays at its own rate. (For AA the kernel
    // uses its jitter buffer's state, which is exactly when AA is output.) Local:
    // the local player is playing.
    bool audible(Source s) const {
        switch (s) {
            case Source::AndroidAuto: return aa_.audible && !aa_.tail;
            case Source::Bluetooth: return bt_.audible && !bt_.tail;
            case Source::Local: break;
        }
        return local_.playing;
    }

private:
    // Per-remote input state, the takeover rule, and the hold-off that follows
    // selectLocal().
    struct Remote {
        bool connected = false;
        bool audible = false;        // its audio is arriving (connected-gated)
        bool hasStatus = false;      // it reports play status (AVRCP up and reporting)
        bool statusPlaying = false;  // ...and that status is PLAYING
        uint64_t audibleSinceUs = 0; // start of the current unbroken audible run
        bool sustained = false;      // audible for >= kTakeoverMs without a break
        bool suppressed = false;     // playing when local was selected; until it stops
        bool tail = false;           // its audio when it was paused for local; until
                                     // that audio stops

        void update(bool conn, bool aud, bool status, bool statusPlay, uint64_t nowUs) {
            connected = conn;
            const bool wasAudible = audible;
            const bool hadStatus = hasStatus;
            audible = conn && aud;
            hasStatus = conn && status;
            statusPlaying = hasStatus && statusPlay;
            // A run starts when audio starts, or when the status channel goes away
            // mid-run: audio heard while the phone still reported (e.g. PAUSED) must
            // not count toward the status-less takeover window.
            if (audible && (!wasAudible || (hadStatus && !hasStatus))) audibleSinceUs = nowUs;
            sustained = audible && nowUs >= audibleSinceUs &&
                        nowUs - audibleSinceUs >= uint64_t(kTakeoverMs) * 1000u;
            if (!playing()) suppressed = false;  // it stopped (or unplugged)
            if (!audible) tail = false;
        }
        // Playing as far as this remote can tell: its status when it reports one,
        // else whether its audio is arriving. Drives '>' / '||' and play/pause.
        bool playing() const { return hasStatus ? statusPlaying : audible; }
        // It has STARTED playing, so it may take the session: its status says
        // PLAYING, or, with no status, its audio has kept coming for kTakeoverMs
        // (a chime or a prompt never does). Not while held off by selectLocal().
        bool claims() const {
            return !suppressed && (hasStatus ? statusPlaying : sustained);
        }
        // selectLocal(): hold a remote that is playing off until it stops, and
        // report that it needs a pause. Its audio still arriving from then on is
        // that pause's tail.
        bool holdOff() {
            if (!playing()) return false;
            suppressed = true;
            tail = audible;
            return true;
        }
    };

    void resolve() {
        // A remote that started playing takes (or keeps) the session; AA outranks
        // BT. With neither, the latch holds, so a paused remote stays the owner.
        if (aa_.claims()) {
            latched_ = Source::AndroidAuto;
        } else if (bt_.claims()) {
            latched_ = Source::Bluetooth;
        } else if ((latched_ == Source::AndroidAuto && !aa_.connected) ||
                   (latched_ == Source::Bluetooth && !bt_.connected)) {
            latched_ = Source::Local;  // the owner went away: fall back
        }
    }

    LocalState local_;
    Remote bt_;
    Remote aa_;
    Source latched_ = Source::Local;  // Local = nothing latched / local selected
    carsh::bt::AvrcpMetadata btMeta_;
};

}  // namespace carsh::player
