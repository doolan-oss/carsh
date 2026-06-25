// ID3v2 (.2.3/.2.4) parser for MP3 tags (PLAN.md §2.10).
//
// Reads the tag at the start of the stream into a buffer and walks text frames
// (TIT2/TPE1/TPE2/TALB/TCON/TRCK/TPOS/TYER/TDRC) plus APIC (embedded art flag).
// Handles the four text encodings (ISO-8859-1, UTF-16 BOM/BE, UTF-8) for the
// Basic Multilingual Plane. Unsynchronization + extended headers are uncommon in
// practice and treated as best-effort (documented limitation).
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

inline bool parseId3v2(carsh::hal::IStream* s, TrackMeta& meta) {
    if (!s) return false;
    s->seek(0);
    uint8_t h[10];
    if (s->read(h, 10) != 10) return false;
    if (h[0] != 'I' || h[1] != 'D' || h[2] != '3') return false;
    uint8_t major = h[3];
    uint32_t size = syncsafe(h + 6);
    std::vector<uint8_t> buf(size);
    if (size && s->read(buf.data(), size) != size) return false;

    std::size_t p = 0;
    while (p + 10 <= buf.size()) {
        const uint8_t* f = buf.data() + p;
        if (f[0] == 0) break;  // padding
        std::string id(reinterpret_cast<const char*>(f), 4);
        uint32_t fsize = (major >= 4) ? syncsafe(f + 4) : be32(f + 4);
        p += 10;
        if (fsize == 0 || p + fsize > buf.size()) break;
        const uint8_t* data = buf.data() + p;

        if (id == "APIC") {
            meta.hasEmbeddedArt = true;
        } else if (id[0] == 'T') {
            std::string txt = decodeId3Text(data, fsize);
            if (id == "TIT2") meta.title = txt;
            else if (id == "TPE1") meta.artist = txt;
            else if (id == "TPE2") meta.albumArtist = txt;
            else if (id == "TALB") meta.album = txt;
            else if (id == "TCON") meta.genre = txt;
            else if (id == "TRCK") meta.trackNo = parseLeadingInt(txt);
            else if (id == "TPOS") meta.discNo = parseLeadingInt(txt);
            else if (id == "TYER" || id == "TDRC") meta.year = parseYear(txt);
        }
        p += fsize;
    }
    return true;
}

}  // namespace carsh::library::tags
