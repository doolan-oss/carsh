// Vorbis comment parser (PLAN.md §2.10) — the tag format used inside FLAC
// metadata blocks and (after the "OpusTags" magic) in Opus streams.
//
// Layout: [u32le vendorLen][vendor][u32le count] then count × [u32le len]
// ["KEY=value"]. Keys are case-insensitive. We map the standard keys into
// TrackMeta and parse "n/total" track/disc forms and 4-digit years.
//
// Two entry points: parseVorbisComment() over an in-RAM block (Opus reassembles
// its comment packet), and parseVorbisCommentStream() which walks a FLAC
// VORBIS_COMMENT block straight from the stream one comment at a time, seeking
// past oversized ones (embedded METADATA_BLOCK_PICTURE, lyrics) so the block is
// never buffered whole — a freed block > 512 KiB is lost on Circle's stock heap
// (review L1).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "hal/Vfs.h"
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

// Map one "KEY=value" comment into meta (unknown keys are ignored).
inline void applyVorbisComment(const char* c, std::size_t n, TrackMeta& meta) {
    const void* eqp = std::memchr(c, '=', n);
    if (!eqp) return;
    const std::size_t eq = std::size_t(static_cast<const char*>(eqp) - c);
    std::string key = upper(std::string(c, eq));
    std::string val(c + eq + 1, n - eq - 1);
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

// Parse a Vorbis comment block into meta. Returns false on malformed input.
inline bool parseVorbisComment(const uint8_t* data, std::size_t len,
                               TrackMeta& meta) {
    std::size_t p = 0;
    if (p + 4 > len) return false;
    uint32_t vlen = rdU32le(data + p);
    p += 4;
    if (vlen > len - p || len - p - vlen < 4) return false;
    p += vlen;  // skip vendor
    uint32_t count = rdU32le(data + p);
    p += 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (len - p < 4) return false;
        uint32_t clen = rdU32le(data + p);
        p += 4;
        if (clen > len - p) return false;
        applyVorbisComment(reinterpret_cast<const char*>(data + p), clen, meta);
        p += clen;
    }
    return true;
}

// Largest single comment parseVorbisCommentStream() will read; anything bigger
// (base64 cover art, lyrics) is seeked past. Keeps every buffer in a small heap
// bucket.
constexpr std::size_t kMaxVorbisComment = 64 * 1024;

// Parse the Vorbis comment block occupying the next `len` bytes of `s` (the
// stream is positioned at its first byte). Reads one comment at a time into a
// small reused buffer; the block length bounds every read and skip, so a corrupt
// count or length cannot run past it. Returns false on malformed input (fields
// parsed before the fault are kept).
inline bool parseVorbisCommentStream(carsh::hal::IStream* s, std::uint64_t len,
                                     TrackMeta& meta) {
    if (!s) return false;
    std::uint64_t pos = s->tell();
    const std::uint64_t end = pos + len;
    uint8_t w[4];
    auto rd32 = [&](uint32_t& v) {
        if (end - pos < 4 || s->read(w, 4) != 4) return false;
        pos += 4;
        v = rdU32le(w);
        return true;
    };
    uint32_t vlen = 0, count = 0;
    if (!rd32(vlen) || vlen > end - pos) return false;
    pos += vlen;  // skip vendor
    if (!s->seek(pos) || !rd32(count)) return false;
    std::vector<uint8_t> buf;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t clen = 0;
        if (!rd32(clen) || clen > end - pos) return false;
        if (clen <= kMaxVorbisComment) {
            buf.resize(clen);
            if (clen && s->read(buf.data(), clen) != clen) return false;
            applyVorbisComment(reinterpret_cast<const char*>(buf.data()), clen, meta);
            pos += clen;
        } else {
            pos += clen;  // oversized (picture/lyrics): never buffered
            if (!s->seek(pos)) return false;
        }
    }
    return true;
}

}  // namespace carsh::library::tags
