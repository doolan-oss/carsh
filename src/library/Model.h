// Library data model (PLAN.md §4.1).
//
// TrackMeta is the parsed-tag view a tag parser produces. Track is the in-RAM
// record (string fields interned to ids); it mirrors the persisted 128-byte
// TrackRecord. StableId/albumKey hashing keep ids deterministic across rescans.
#pragma once

#include <cstdint>
#include <string>

#include "library/TextNorm.h"

namespace carsh::library {

// FNV-1a 64-bit — stable, dependency-free hash for ids and album keys.
inline uint64_t fnv1a(const std::string& s, uint64_t seed = 1469598103934665603ull) {
    uint64_t h = seed;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

enum TrackFlags : uint32_t {
    FlagHasEmbeddedArt = 1u << 0,
    FlagIsCueTrack = 1u << 1,
    FlagRgScanned = 1u << 2,
};

// Parsed metadata from a tag parser, before interning.
struct TrackMeta {
    std::string title;
    std::string artist;
    std::string album;
    std::string albumArtist;  // falls back to artist if empty
    std::string genre;
    uint16_t trackNo = 0;
    uint16_t discNo = 0;
    uint16_t year = 0;
    uint32_t durationMs = 0;
    uint32_t sampleRate = 0;
    uint8_t bitsPerSample = 0;
    uint8_t channels = 0;
    bool hasEmbeddedArt = false;

    const std::string& effectiveAlbumArtist() const {
        return albumArtist.empty() ? artist : albumArtist;
    }
};

// In-RAM track record. String fields are ids into the StringTable.
struct Track {
    uint64_t trackId = 0;     // hash(sourceId, path) — stable across rescans
    uint32_t artistRef = 0;
    uint32_t albumRef = 0;
    uint32_t titleRef = 0;
    uint32_t albumArtistRef = 0;
    uint32_t genreRef = 0;
    uint32_t pathRef = 0;
    uint32_t durationMs = 0;
    uint32_t sampleRate = 0;
    uint8_t bitsPerSample = 0;
    uint8_t channels = 0;
    uint8_t sourceId = 0;
    uint8_t format = 0;       // decode::Codec value
    uint16_t trackNo = 0;
    uint16_t discNo = 0;
    uint16_t year = 0;
    float rgTrackGain = 0, rgAlbumGain = 0;
    float rgTrackPeak = 1, rgAlbumPeak = 1;
    uint64_t albumKey = 0;
    uint32_t flags = 0;
};

// Stable track id from source + path (path lowercased for stability).
inline uint64_t makeTrackId(uint8_t sourceId, const std::string& path) {
    std::string key = std::to_string(sourceId) + ":" + path;
    return fnv1a(key);
}

// Album key from album-artist + album (normalized), folder path as fallback.
inline uint64_t makeAlbumKey(const std::string& albumArtist,
                             const std::string& album,
                             const std::string& folderFallback) {
    std::string a = normalize(albumArtist) + "\x1f" + normalize(album);
    if (normalize(albumArtist).empty() && normalize(album).empty())
        return fnv1a(normalize(folderFallback));
    return fnv1a(a);
}

}  // namespace carsh::library
