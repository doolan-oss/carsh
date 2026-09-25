// ID3v2 (v2.2/v2.3/v2.4) parser for MP3 tags (PLAN.md §2.10).
//
// Walks the tag frame by frame straight from the stream: only the text frames we
// map (TIT2/TPE1/TPE2/TALB/TCON/TRCK/TPOS/TYER/TDRC, v2.2 TT2/TP1/...) are read,
// each into one small reused buffer (<= kMaxId3TextFrame); every other frame —
// APIC/PIC cover art above all — is seeked past, and APIC only sets the
// hasEmbeddedArt flag. The header's tag size is clamped to the stream size, so a
// corrupt header claiming 256 MiB costs nothing. Nothing is allocated in
// proportion to the tag: on Circle's stock heap a freed block > 512 KiB is lost
// for good, and buffering whole tags (embedded art included) per file could
// exhaust memory while indexing a big library (review L1).
//
// Handles the four text encodings (ISO-8859-1, UTF-16 BOM/BE, UTF-8) for the
// Basic Multilingual Plane, extended headers, v2.2/v2.3 tag-level and v2.4
// frame-level unsynchronisation, v2.4 data-length indicators and grouping bytes.
// Compressed or encrypted frames are skipped.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hal/Vfs.h"
#include "library/Model.h"
#include "library/tags/Vorbis.h"  // parseLeadingInt / parseYear

namespace carsh::library::tags {

// 7-bit "syncsafe" 4-byte integer (ID3 tag/frame sizes in v2.4).
inline uint32_t syncsafe(const uint8_t* p) {
    return (uint32_t(p[0] & 0x7F) << 21) | (uint32_t(p[1] & 0x7F) << 14) |
           (uint32_t(p[2] & 0x7F) << 7) | uint32_t(p[3] & 0x7F);
}
inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out.push_back(char(cp));
    else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

// Decode an ID3 text-frame body (first byte is the encoding) to UTF-8.
inline std::string decodeId3Text(const uint8_t* d, std::size_t len) {
    if (len == 0) return "";
    uint8_t enc = d[0];
    const uint8_t* p = d + 1;
    std::size_t n = len - 1;
    std::string out;
    auto trimNul = [&](std::string s) {
        while (!s.empty() && (s.back() == '\0')) s.pop_back();
        return s;
    };
    if (enc == 0 || enc == 3) {  // ISO-8859-1 or UTF-8
        if (enc == 3) {  // already UTF-8
            out.assign(reinterpret_cast<const char*>(p), n);
        } else {  // Latin-1 -> UTF-8
            for (std::size_t i = 0; i < n; ++i) appendUtf8(out, p[i]);
        }
        return trimNul(out);
    }
    // UTF-16 (enc 1 = with BOM, enc 2 = big-endian, no BOM)
    bool be = (enc == 2);
    std::size_t i = 0;
    if (enc == 1 && n >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
        else if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
    }
    // Real-world quirk (e.g. some LAME builds): a frame is declared UTF-16 with
    // a BOM but the payload is actually 8-bit. Genuine UTF-16 ASCII text is
    // ~50% zero bytes; if the post-BOM payload has very few zeros, decode it as
    // Latin-1 instead so we don't produce garbage CJK code points.
    {
        std::size_t m = n - i, zeros = 0;
        for (std::size_t k = i; k < n; ++k) if (p[k] == 0) ++zeros;
        if (m >= 2 && zeros * 4 < m) {
            for (std::size_t k = i; k < n; ++k) {
                if (p[k] == 0) break;
                appendUtf8(out, p[k]);
            }
            return out;
        }
    }
    for (; i + 1 < n; i += 2) {
        uint32_t cp = be ? (uint32_t(p[i]) << 8 | p[i + 1])
                         : (uint32_t(p[i + 1]) << 8 | p[i]);
        if (cp == 0) break;
        appendUtf8(out, cp);  // BMP only (no surrogate pairs)
    }
    return out;
}

// Largest text frame parseId3v2() will read; bigger ones are skipped (no real
// title/artist is anywhere near this). Keeps the buffer in a small heap bucket.
constexpr std::size_t kMaxId3TextFrame = 64 * 1024;

namespace id3 {

// Bounded, forward-only view of the tag bytes [pos, end) of the stream. With
// `unsync` set (the v2.2/v2.3 tag-level flag) reads undo the unsynchronisation
// scheme (0xFF 0x00 -> 0xFF) across the whole tag, which is how v2.3 defines
// frame sizes; skips then have to read through (via a small fixed buffer)
// instead of seeking, since the raw byte count is not known up front.
class TagCursor {
public:
    TagCursor(carsh::hal::IStream* s, std::uint64_t pos, std::uint64_t end,
              bool unsync)
        : s_(s), pos_(pos), end_(end), unsync_(unsync) {
        s_->seek(pos_);
    }

    // Raw tag bytes left — an upper bound on the logical bytes when unsync is on.
    std::uint64_t left() const { return (end_ - pos_) + (len_ - off_); }

    bool read(std::uint8_t* dst, std::size_t n) {
        if (!unsync_) {
            if (n > end_ - pos_) return false;
            const std::size_t got = n ? s_->read(dst, n) : 0;
            pos_ += got;
            return got == n;
        }
        for (std::size_t i = 0; i < n; ++i)
            if (!next(dst[i])) return false;
        return true;
    }

    bool skip(std::uint64_t n) {
        if (!unsync_) {
            if (n > end_ - pos_) return false;
            pos_ += n;
            return s_->seek(pos_);
        }
        std::uint8_t b;
        for (std::uint64_t i = 0; i < n; ++i)
            if (!next(b)) return false;
        return true;
    }

private:
    bool rawByte(std::uint8_t& b) {
        if (off_ == len_) {
            std::uint64_t want = end_ - pos_;
            if (want > sizeof buf_) want = sizeof buf_;
            len_ = want ? s_->read(buf_, std::size_t(want)) : 0;
            off_ = 0;
            pos_ += len_;
            if (len_ == 0) return false;
        }
        b = buf_[off_++];
        return true;
    }
    bool next(std::uint8_t& b) {
        if (!rawByte(b)) return false;
        if (prevFF_ && b == 0x00 && !rawByte(b)) return false;  // drop the stuffed 0
        prevFF_ = (b == 0xFF);
        return true;
    }

    carsh::hal::IStream* s_;
    std::uint64_t pos_, end_;  // stream position of the next unbuffered byte
    bool unsync_;
    bool prevFF_ = false;
    std::uint8_t buf_[512];
    std::size_t off_ = 0, len_ = 0;
};

// Undo unsynchronisation in place (v2.4 per-frame); returns the new length.
inline std::size_t undoUnsync(std::uint8_t* d, std::size_t n) {
    std::size_t w = 0;
    for (std::size_t r = 0; r < n; ++r) {
        d[w++] = d[r];
        if (d[r] == 0xFF && r + 1 < n && d[r + 1] == 0x00) ++r;
    }
    return w;
}

inline bool validFrameId(const std::uint8_t* id, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t c = id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

enum class Field : std::uint8_t {
    None, Art, Title, Artist, AlbumArtist, Album, Genre, Track, Disc, Year
};

// Frame id -> field (4-char v2.3/v2.4 ids, 3-char v2.2 ids).
inline Field fieldFor(const std::string& id) {
    struct Map { const char* v3; const char* v2; Field f; };
    static const Map kMap[] = {
        {"APIC", "PIC", Field::Art},    {"TIT2", "TT2", Field::Title},
        {"TPE1", "TP1", Field::Artist}, {"TPE2", "TP2", Field::AlbumArtist},
        {"TALB", "TAL", Field::Album},  {"TCON", "TCO", Field::Genre},
        {"TRCK", "TRK", Field::Track},  {"TPOS", "TPA", Field::Disc},
        {"TYER", "TYE", Field::Year},   {"TDRC", nullptr, Field::Year},
    };
    for (const Map& m : kMap)
        if (id == m.v3 || (m.v2 && id == m.v2)) return m.f;
    return Field::None;
}

inline void applyText(Field f, const std::string& txt, TrackMeta& meta) {
    switch (f) {
        case Field::Title: meta.title = txt; break;
        case Field::Artist: meta.artist = txt; break;
        case Field::AlbumArtist: meta.albumArtist = txt; break;
        case Field::Album: meta.album = txt; break;
        case Field::Genre: meta.genre = txt; break;
        case Field::Track: meta.trackNo = parseLeadingInt(txt); break;
        case Field::Disc: meta.discNo = parseLeadingInt(txt); break;
        case Field::Year: meta.year = parseYear(txt); break;
        default: break;
    }
}

}  // namespace id3

// Returns false when the stream has no valid ID3v2 header; true otherwise, with
// whatever frames could be read (a truncated or partly corrupt tag still yields
// the frames before the damage).
inline bool parseId3v2(carsh::hal::IStream* s, TrackMeta& meta) {
    if (!s) return false;
    s->seek(0);
    uint8_t h[10];
    if (s->read(h, 10) != 10) return false;
    if (h[0] != 'I' || h[1] != 'D' || h[2] != '3') return false;
    const uint8_t major = h[3];
    if (major < 2 || major > 4 || h[4] == 0xFF) return false;
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return false;  // size not syncsafe
    const uint8_t flags = h[5];
    // v2.2's "compression" bit never got a defined scheme: ignore the tag.
    if (major == 2 && (flags & 0x40)) return true;

    // Clamp the claimed size to what the stream actually holds.
    const std::uint64_t streamSize = s->size();
    std::uint64_t end = 10 + std::uint64_t(syncsafe(h + 6));
    if (end > streamSize) end = streamSize;
    const bool tagUnsync = (flags & 0x80) != 0;
    // v2.2/v2.3 unsynchronise the whole tag (frame sizes are pre-unsync);
    // v2.4 does it per frame (sizes are the stored bytes).
    id3::TagCursor c(s, 10, end, tagUnsync && major < 4);

    if (major >= 3 && (flags & 0x40)) {  // extended header: skip it
        uint8_t e[4];
        if (!c.read(e, 4)) return true;
        if (major == 3) {  // size excludes its own 4 bytes
            if (!c.skip(be32(e))) return true;
        } else {  // v2.4: syncsafe, includes its own 4 bytes
            const uint32_t n = syncsafe(e);
            if (n < 6 || !c.skip(n - 4)) return true;
        }
    }

    const std::size_t idLen = major == 2 ? 3 : 4;
    const std::size_t hdrLen = major == 2 ? 6 : 10;
    std::vector<uint8_t> body;  // reused text-frame buffer, <= kMaxId3TextFrame
    while (c.left() >= hdrLen) {
        uint8_t f[10];
        if (!c.read(f, hdrLen)) break;
        if (f[0] == 0) break;                       // padding
        if (!id3::validFrameId(f, idLen)) break;    // garbage: keep what we have
        uint32_t fsize;
        uint8_t fmt = 0;  // v2.3/v2.4 format-flags byte
        if (major == 2) {
            fsize = (uint32_t(f[3]) << 16) | (uint32_t(f[4]) << 8) | f[5];
        } else if (major == 3) {
            fsize = be32(f + 4);
            fmt = f[9];
        } else {
            // v2.4 sizes are syncsafe; some writers (old iTunes) store plain
            // big-endian — a set high bit can only mean that.
            fsize = ((f[4] | f[5] | f[6] | f[7]) & 0x80) ? be32(f + 4)
                                                          : syncsafe(f + 4);
            fmt = f[9];
        }
        if (fsize > c.left()) break;  // runs past the (clamped) tag end

        const id3::Field field =
            id3::fieldFor(std::string(reinterpret_cast<const char*>(f), idLen));
        if (field == id3::Field::Art) meta.hasEmbeddedArt = true;

        bool readIt = field != id3::Field::None && field != id3::Field::Art &&
                      fsize <= kMaxId3TextFrame;
        std::size_t prefix = 0;      // flag-added bytes before the text
        bool frameUnsync = false;
        if (major == 3) {
            if (fmt & 0xC0) readIt = false;           // compressed / encrypted
            if (fmt & 0x20) prefix += 1;              // grouping id
        } else if (major == 4) {
            if (fmt & 0x0C) readIt = false;           // compressed / encrypted
            if (fmt & 0x40) prefix += 1;              // grouping id
            if (fmt & 0x01) prefix += 4;              // data length indicator
            frameUnsync = tagUnsync || (fmt & 0x02);
        }
        if (!readIt) {
            if (!c.skip(fsize)) break;
            continue;
        }
        body.resize(fsize);
        if (!c.read(body.data(), fsize)) break;
        if (prefix >= body.size()) continue;
        std::size_t n = body.size() - prefix;
        if (frameUnsync) n = id3::undoUnsync(body.data() + prefix, n);
        id3::applyText(field, decodeId3Text(body.data() + prefix, n), meta);
    }
    return true;
}

}  // namespace carsh::library::tags
