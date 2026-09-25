// FLAC metadata parser (PLAN.md §2.10): STREAMINFO (audio properties + exact
// sample count for gapless/duration), VORBIS_COMMENT (tags), PICTURE (embedded
// art flag). Reads metadata blocks directly from an IStream; does not decode.
//
// Nothing is buffered in proportion to a block's size: STREAMINFO is a fixed 18
// bytes, the Vorbis comment is walked one comment at a time, and PICTURE /
// PADDING / everything else is seeked past (its length is checked against the
// stream size first). A 1 MiB embedded cover used to be read into a transient
// vector per file during indexing — on Circle's stock heap every freed block
// > 512 KiB is lost, so a big library could exhaust memory mid-scan (review L1).
#pragma once

#include <cstdint>
#include <cstring>

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

    const std::uint64_t streamSize = s->size();
    std::uint64_t pos = 4;
    for (;;) {
        uint8_t hdr[4];
        if (s->read(hdr, 4) != 4) break;
        pos += 4;
        bool last = (hdr[0] & 0x80) != 0;
        uint8_t type = hdr[0] & 0x7F;
        uint32_t len = (uint32_t(hdr[1]) << 16) | (uint32_t(hdr[2]) << 8) |
                       uint32_t(hdr[3]);
        if (pos > streamSize || len > streamSize - pos) break;  // truncated/corrupt
        const std::uint64_t blockEnd = pos + len;

        if (type == 0 && len >= 18) {  // STREAMINFO
            uint8_t b[18];
            if (s->read(b, sizeof b) != sizeof b) break;
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
            parseVorbisCommentStream(s, len, meta);
        } else if (type == 6) {  // PICTURE: flag only, body never read
            meta.hasEmbeddedArt = true;
        }
        if (last) break;
        pos = blockEnd;
        if (!s->seek(pos)) break;
    }
    return true;
}

}  // namespace carsh::library::tags
