// FLAC metadata parser (PLAN.md §2.10): STREAMINFO (audio properties + exact
// sample count for gapless/duration), VORBIS_COMMENT (tags), PICTURE (embedded
// art flag). Reads metadata blocks directly from an IStream; does not decode.
#pragma once

#include <cstdint>
#include <vector>

#include "hal/Vfs.h"
#include "library/Model.h"
#include "library/tags/Vorbis.h"

namespace carsh::library::tags {

inline bool parseFlacTags(carsh::hal::IStream* s, TrackMeta& meta) {
    if (!s) return false;
    s->seek(0);
    uint8_t magic[4];
    if (s->read(magic, 4) != 4 || std::memcmp(magic, "fLaC", 4) != 0)
        return false;

    for (;;) {
        uint8_t hdr[4];
        if (s->read(hdr, 4) != 4) break;
        bool last = (hdr[0] & 0x80) != 0;
        uint8_t type = hdr[0] & 0x7F;
        uint32_t len = (uint32_t(hdr[1]) << 16) | (uint32_t(hdr[2]) << 8) |
                       uint32_t(hdr[3]);
        std::vector<uint8_t> body(len);
        if (len && s->read(body.data(), len) != len) break;

        if (type == 0 && len >= 18) {  // STREAMINFO
            const uint8_t* b = body.data();
            uint32_t sr = (uint32_t(b[10]) << 12) | (uint32_t(b[11]) << 4) |
                          (uint32_t(b[12]) >> 4);
            uint8_t ch = uint8_t(((b[12] >> 1) & 0x07) + 1);
            uint8_t bps = uint8_t((((b[12] & 1) << 4) | (b[13] >> 4)) + 1);
            uint64_t total = (uint64_t(b[13] & 0x0F) << 32) |
                             (uint64_t(b[14]) << 24) | (uint64_t(b[15]) << 16) |
                             (uint64_t(b[16]) << 8) | uint64_t(b[17]);
            meta.sampleRate = sr;
            meta.channels = ch;
            meta.bitsPerSample = bps;
            if (sr) meta.durationMs = uint32_t(total * 1000ull / sr);
        } else if (type == 4) {  // VORBIS_COMMENT
            parseVorbisComment(body.data(), body.size(), meta);
        } else if (type == 6) {  // PICTURE
            meta.hasEmbeddedArt = true;
        }
        if (last) break;
    }
    return true;
}

}  // namespace carsh::library::tags
