// NowPlaying — assembles AVRCP "now playing" metadata from the individual media
// attributes and the playback-status notification that the controller delivers
// as SEPARATE events, into one bt::AvrcpMetadata for the UI / BtSource.
//
// This is the app-side half of the AVRCP metadata path, kept platform-independent
// and host-tested (tests/bt/BtParse_test). The device glue (BtStackGlue.cpp) does
// only the thin, BTstack-specific extraction — pull each attribute's id + value
// bytes and playback status off the subevent — and feeds them here; the host
// tests feed the same calls with synthetic values. No BTstack types appear in
// this header, exactly like bt::deviceRow().
//
// AVRCP returns ALL media attributes as text (AVRCP spec §26), including the song
// length, which arrives as an ASCII decimal count of milliseconds — so the
// duration parse lives here too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "bt/Bluetooth.h"

namespace carsh::bt {

// AVRCP media attribute ids (AVRCP spec, GetElementAttributes / now-playing).
enum class AvrcpAttr : uint8_t {
    Title = 1,
    Artist = 2,
    Album = 3,
    TrackNumber = 4,
    TotalTracks = 5,
    Genre = 6,
    PlayingTimeMs = 7,
};

// AVRCP playback status (PLAY_STATUS), as carried by the
// NOTIFICATION_PLAYBACK_STATUS_CHANGED event.
enum class AvrcpPlayStatus : uint8_t {
    Stopped = 0x00,
    Playing = 0x01,
    Paused = 0x02,
    FwdSeek = 0x03,
    RevSeek = 0x04,
    Error = 0xFF,
};

class NowPlayingAssembler {
public:
    // Clear all fields. Call on track-changed and when a new now-playing query
    // begins, so stale title/artist don't bleed across tracks.
    void reset() { md_ = AvrcpMetadata{}; }

    // Feed one media attribute. `value` is NOT NUL-terminated (length-delimited,
    // as AVRCP delivers it). Unknown ids are ignored. The playing flag is owned
    // by setPlaybackStatus(), not the attributes.
    void setAttribute(uint8_t attrId, const char* value, std::size_t len) {
        switch (static_cast<AvrcpAttr>(attrId)) {
            case AvrcpAttr::Title:  md_.title.assign(value, len); break;
            case AvrcpAttr::Artist: md_.artist.assign(value, len); break;
            case AvrcpAttr::Album:  md_.album.assign(value, len); break;
            case AvrcpAttr::PlayingTimeMs: md_.durationMs = parseMs(value, len); break;
            default: break;  // TrackNumber/TotalTracks/Genre: not surfaced (yet)
        }
    }

    // Set the track duration directly. BTstack pre-parses the SONG_LENGTH_MS
    // notification into a number, so the device glue uses this; the text form
    // (PlayingTimeMs attribute via setAttribute) is for raw GetElementAttributes.
    void setDurationMs(uint32_t ms) { md_.durationMs = ms; }

    // Map the AVRCP playback status to the boolean the UI consumes. Only PLAYING
    // counts as playing; paused/stopped/seek/error are all "not playing".
    void setPlaybackStatus(uint8_t avrcpStatus) {
        md_.playing = static_cast<AvrcpPlayStatus>(avrcpStatus) == AvrcpPlayStatus::Playing;
    }

    const AvrcpMetadata& metadata() const { return md_; }

    // True once any human-visible field has been populated — lets the glue avoid
    // emitting an all-empty metadata before the first attribute arrives.
    bool hasText() const {
        return !md_.title.empty() || !md_.artist.empty() || !md_.album.empty();
    }

private:
    // Parse an ASCII decimal millisecond count. Defensive: reads leading digits
    // only and saturates rather than overflowing on absurd input.
    static uint32_t parseMs(const char* value, std::size_t len) {
        uint64_t ms = 0;
        for (std::size_t i = 0; i < len; ++i) {
            char c = value[i];
            if (c < '0' || c > '9') break;  // stop at first non-digit
            ms = ms * 10 + uint64_t(c - '0');
            if (ms > 0xFFFFFFFFull) return 0xFFFFFFFFu;  // saturate
        }
        return uint32_t(ms);
    }

    AvrcpMetadata md_;
};

}  // namespace carsh::bt
