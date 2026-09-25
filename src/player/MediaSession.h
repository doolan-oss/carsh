// MediaSession — arbitrates the head unit's "now playing" between the local file
// player, a connected Bluetooth phone (A2DP audio + AVRCP metadata/transport) and
// the Android Auto projection, and produces one unified view-model for the Now
// Playing screen plus the transport target (which backend a play/pause/next/prev
// should drive).
//
// It is platform-independent and host-tested: the app feeds it a snapshot each frame
// (local player fields + the latest AVRCP metadata + whether BT / AA are connected
// and streaming) and reads back the view + source. No player/BTstack types leak in —
// only plain values and bt::AvrcpMetadata — so it sits cleanly between the two.
//
// Ownership is LATCHED, not re-derived from audio activity every frame. A paused
// phone produces no audio, yet it is still what the user is listening to: Now
// Playing must keep showing it and '>' must resume it, not flip to (and start) the
// local track underneath. So:
//   * A remote source (Android Auto or Bluetooth) that STARTS playing takes the
//     session: it becomes the owner and stays the owner while it is paused, until
//     another source is explicitly started or it disconnects. If both stream at
//     once, Android Auto wins (it outranks A2DP in the audio mix).
//   * The user explicitly starting local playback (picking a track, PLAY/SHUFFLE,
//     a queue row) calls selectLocal(): local owns the session again, and any
//     remote still streaming is suppressed until its stream stops — so the tail of
//     a stream the app just paused can't grab the session straight back. The
//     returned PauseRequest names the remotes the app should pause.
//   * With nothing latched: Android Auto if it's connected and there's no local
//     track, else Bluetooth under the same rule, else the local player. This
//     fallback only picks what Now Playing shows; it never pauses the local player.
// Transport (play/pause/next/prev) routes to the owner: AA media keys over the input
// channel, AVRCP to the BT phone, or the local player. While a remote has TAKEN the
// session (latched) the local player must not play underneath it
// (shouldPauseLocal()).
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

    // The remote sources still streaming when local playback was selected: a source
    // switch pauses the source it leaves, so the app sends these a pause.
    struct PauseRequest {
        bool androidAuto = false;
        bool bluetooth = false;
    };

    // --- Inputs, refreshed each frame --------------------------------------
    void setLocal(const LocalState& s) { local_ = s; }
    // connected: a phone is linked. streaming: it's actively producing audio
    // (A2DP PCM arriving, or AVRCP playback status == PLAYING). md: latest AVRCP.
    void setBluetooth(bool connected, bool streaming, const carsh::bt::AvrcpMetadata& md) {
        bt_.update(connected, streaming);
        btMeta_ = md;
        resolve();
    }
    // connected: a projection session is up (AaProjection::connected()).
    // streaming: AA media audio is playing (media PCM arriving / the output engaged).
    // AA has no metadata channel yet, so only its state matters.
    void setAndroidAuto(bool connected, bool streaming) {
        aa_.update(connected, streaming);
        resolve();
    }

    // The user explicitly started local playback: local owns the session until a
    // remote source starts playing again. Returns the remotes to pause.
    PauseRequest selectLocal() {
        PauseRequest r;
        r.androidAuto = aa_.suppressIfStreaming();
        r.bluetooth = bt_.suppressIfStreaming();
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
            v.playing = aa_.streaming;
        } else if (v.source == Source::Bluetooth) {
            // Before the first AVRCP attribute arrives, show a neutral label so the
            // screen isn't blank while connected.
            v.title = btMeta_.title.empty() ? "Bluetooth" : btMeta_.title;
            v.artist = btMeta_.artist;
            v.album = btMeta_.album;
            v.durationMs = btMeta_.durationMs;
            v.positionMs = 0;  // AVRCP play position not tracked yet (no BT scrub)
            v.playing = btPlaying();
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
        return btPlaying() ? carsh::bt::AvrcpCommand::Pause
                           : carsh::bt::AvrcpCommand::Play;
    }

private:
    // Per-remote input state + the suppression that follows selectLocal().
    struct Remote {
        bool connected = false;
        bool streaming = false;   // as reported (connected-gated)
        bool suppressed = false;  // streaming when local was selected; until it stops

        void update(bool conn, bool str) {
            connected = conn;
            streaming = conn && str;
            if (!streaming) suppressed = false;  // that stream ended (or unplugged)
        }
        // Playing now and not held off by an explicit local selection.
        bool active() const { return streaming && !suppressed; }
        bool suppressIfStreaming() {
            if (streaming) suppressed = true;
            return streaming;
        }
    };

    // The BT phone is playing: AVRCP says so, or its A2DP stream is live (a phone
    // without AVRCP, or before the first status arrives, must still be pausable).
    bool btPlaying() const { return btMeta_.playing || bt_.streaming; }

    void resolve() {
        // A remote that is playing takes (or keeps) the session; AA outranks BT.
        // With neither active the latch holds, so a paused remote stays the owner.
        if (aa_.active()) {
            latched_ = Source::AndroidAuto;
        } else if (bt_.active()) {
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
