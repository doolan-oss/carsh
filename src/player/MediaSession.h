// MediaSession — arbitrates the head unit's "now playing" between the local file
// player and a connected Bluetooth phone (A2DP audio + AVRCP metadata/transport),
// and produces one unified view-model for the Now Playing screen plus the transport
// target (which backend a play/pause/next/prev should drive).
//
// It is platform-independent and host-tested: the app feeds it a snapshot each frame
// (local player fields + the latest AVRCP metadata + whether BT is connected /
// streaming) and reads back the view + source. No player/BTstack types leak in —
// only plain values and bt::AvrcpMetadata — so it sits cleanly between the two.
//
// Source rule (priority Android Auto > Bluetooth > Local, mirroring the audio mix in
// BtMixSink so the screen matches what you hear):
//   * Android Auto owns now-playing while its audio is streaming;
//   * else Bluetooth owns it when the phone is streaming (A2DP PCM / AVRCP PLAYING)
//     or it's connected and there's no local track to show;
//   * else the local file player owns it.
// Transport (play/pause/next/prev) routes to whichever owns it: AA media keys over
// the input channel, AVRCP to the BT phone, or the local player.
#pragma once

#include <cstdint>
#include <string>

#include "bt/Bluetooth.h"

namespace carsh::player {

enum class Source : uint8_t { Local, Bluetooth, AndroidAuto };

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

    // --- Inputs, refreshed each frame --------------------------------------
    void setLocal(const LocalState& s) { local_ = s; }
    // connected: a phone is linked. streaming: it's actively producing audio
    // (A2DP PCM in the ring, or AVRCP playback status == PLAYING). md: latest AVRCP.
    void setBluetooth(bool connected, bool streaming, const carsh::bt::AvrcpMetadata& md) {
        btConnected_ = connected;
        btStreaming_ = streaming;
        btMeta_ = md;
    }
    // active: Android Auto audio is currently streaming out the jack (the AA jitter
    // buffer is engaged). AA has no metadata channel yet, so only its activity matters.
    void setAndroidAuto(bool active) { aaActive_ = active; }

    // --- Outputs ------------------------------------------------------------
    Source source() const {
        if (aaActive_) return Source::AndroidAuto;
        if (btConnected_ && (btStreaming_ || !local_.hasTrack)) return Source::Bluetooth;
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
            v.playing = true;
        } else if (v.source == Source::Bluetooth) {
            // Before the first AVRCP attribute arrives, show a neutral label so the
            // screen isn't blank while connected.
            v.title = btMeta_.title.empty() ? "Bluetooth" : btMeta_.title;
            v.artist = btMeta_.artist;
            v.album = btMeta_.album;
            v.durationMs = btMeta_.durationMs;
            v.positionMs = 0;  // AVRCP play position not tracked yet (no BT scrub)
            v.playing = btMeta_.playing;
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

    // Convenience for a play/pause toggle: the AVRCP command to send when BT is the
    // target (Play when currently paused/stopped, Pause when playing).
    carsh::bt::AvrcpCommand playPauseCommand() const {
        return btMeta_.playing ? carsh::bt::AvrcpCommand::Pause
                               : carsh::bt::AvrcpCommand::Play;
    }

private:
    LocalState local_;
    bool btConnected_ = false;
    bool btStreaming_ = false;
    bool aaActive_ = false;
    carsh::bt::AvrcpMetadata btMeta_;
};

}  // namespace carsh::player
