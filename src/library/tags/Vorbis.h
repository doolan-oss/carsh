// Vorbis comment parser (PLAN.md §2.10) — the tag format used inside FLAC
// metadata blocks and (after the "OpusTags" magic) in Opus streams.
//
// Layout: [u32le vendorLen][vendor][u32le count] then count × [u32le len]
// ["KEY=value"]. Keys are case-insensitive. We map the standard keys into
// TrackMeta and parse "n/total" track/disc forms and 4-digit years.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "library/Model.h"

namespace carsh::library::tags {

inline uint32_t rdU32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

inline std::string upper(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'a' && c <= 'z') c = char(c - 32);
    return o;
}

// Parse "6" or "6/12" -> 6.
inline uint16_t parseLeadingInt(const std::string& s) {
    uint32_t v = 0;
    bool any = false;
    for (char c : s) {
        if (c < '0' || c > '9') break;
        v = v * 10 + uint32_t(c - '0');
        any = true;
    }
    return any ? uint16_t(v) : 0;
}

// First 4 consecutive digits (handles "1973" and "1973-03-01").
inline uint16_t parseYear(const std::string& s) {
    for (std::size_t i = 0; i + 4 <= s.size(); ++i) {
        bool ok = true;
        for (int k = 0; k < 4; ++k)
            if (s[i + k] < '0' || s[i + k] > '9') { ok = false; break; }
        if (ok) return uint16_t(std::stoi(s.substr(i, 4)));
    }
    return 0;
}

// Parse a Vorbis comment block into meta. Returns false on malformed input.
inline bool parseVorbisComment(const uint8_t* data, std::size_t len,
                               TrackMeta& meta) {
    std::size_t p = 0;
    if (p + 4 > len) return false;
    uint32_t vlen = rdU32le(data + p);
    p += 4;
    if (p + vlen + 4 > len) return false;
    p += vlen;  // skip vendor
    uint32_t count = rdU32le(data + p);
    p += 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 4 > len) return false;
        uint32_t clen = rdU32le(data + p);
        p += 4;
        if (p + clen > len) return false;
        std::string comment(reinterpret_cast<const char*>(data + p), clen);
        p += clen;
        auto eq = comment.find('=');
        if (eq == std::string::npos) continue;
        std::string key = upper(comment.substr(0, eq));
        std::string val = comment.substr(eq + 1);
        if (key == "TITLE") meta.title = val;
        else if (key == "ARTIST") meta.artist = val;
        else if (key == "ALBUM") meta.album = val;
        else if (key == "ALBUMARTIST" || key == "ALBUM ARTIST")
            meta.albumArtist = val;
        else if (key == "GENRE") meta.genre = val;
        else if (key == "TRACKNUMBER") meta.trackNo = parseLeadingInt(val);
        else if (key == "DISCNUMBER") meta.discNo = parseLeadingInt(val);
        else if (key == "DATE" || key == "YEAR") meta.year = parseYear(val);
    }
    return true;
}

}  // namespace carsh::library::tags
